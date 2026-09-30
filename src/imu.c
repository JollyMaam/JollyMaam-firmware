#include <asm/br23.h>
#include <asm/gpio.h>
#include <system/os/os_api.h>
#include <system/timer.h>

#include "imu.h"
#include "timing.h"
#include "input.h"

#define QMI8658A_ADDRESS          CYCLONE2_IMU_I2C_ADDRESS
#define QMI8658A_DEVICE_ID        0x05U
#define QMI8658A_REG_WHO_AM_I     0x00U
#define QMI8658A_REG_ACCEL_X_L    0x35U
#define QMI8658A_REG_CTRL7        0x08U   /* sensor enables */
#define QMI8658A_CTRL7_OFF        0x00U

#define IIC_ROUTE_MASK            (3U << 18)
#define IIC_ROUTE_GROUP_C         (2U << 18)
#define IIC_ENABLE                BIT(0)
#define IIC_CONFIG_DONE           BIT(2)
#define IIC_DIRECTION_IN          BIT(3)
#define IIC_PRESET_STOP           BIT(4)
#define IIC_PRESET_START          BIT(5)
#define IIC_RECEIVE_NACK          BIT(6)
#define IIC_SENT_NACK             BIT(7)
#define IIC_STOP_PENDING_CLEAR    BIT(12)
#define IIC_STOP_PENDING          BIT(13)
#define IIC_PENDING_CLEAR         BIT(14)
#define IIC_PENDING               BIT(15)
#define IIC_START_PENDING_CLEAR   BIT(14)
/* lsb / (2 * (BAUD + 1)): 29 at the 24 MHz lsb is 400 kHz, the QMI8658A's
 * fast mode (119 was 100 kHz; the imu task busy-waits on the bus, so this is
 * CPU time too).  Measured through status imu.read_us. */
#define IIC_BAUD_DIVISOR          29U
#define IIC_WAIT_ITERATIONS       100000U

/* Sampling is paced in control polls (1 ms each).  The sensor runs at
 * 448 Hz (CTRL3); the OS tick is 10 ms, which is all a sleeping task gets,
 * so the control poll wakes this task instead.  A read busy-waits on the
 * bus in the lowest-priority task: it takes only what the others leave, but
 * that is CPU time and battery.  Measured (2026-09-29): all twelve bytes
 * every 2 ms took 0.6 ms a read and 40% of the CPU.  So the full sensor
 * goes every 4 ms (the rate of the reports that carry it), and the gyro
 * alone, for pointing, every 2 ms. */
#define IMU_FULL_POLLS            4U
#define IMU_GYRO_POLLS            2U
#define QMI8658A_REG_GYRO_X_L     0x3bU

enum imu_error {
    IMU_ERROR_NONE,
    IMU_ERROR_TIMEOUT,
    IMU_ERROR_NACK,
    IMU_ERROR_BAD_ID,
    IMU_ERROR_NOT_INITIALIZED,
};

struct register_value {
    u8 reg;
    u8 value;
};

/* Exact initialization recovered from stock app.bin at 0x01e3c2c6 and
 * verified by the RAM diagnostic. */
static const struct register_value initialization[] = {
    {0x60U, 0xb0U},
    {0x02U, 0x60U},
    {0x03U, 0x14U},
    {0x04U, 0x74U},
    {0x06U, 0x55U},
    {0x0bU, 0x01U},
    {0x0aU, 0x12U},
    {0x0aU, 0x00U},
    {0x08U, 0x83U},
};

static struct cyclone2_imu_status imu_status;
static struct cyclone2_imu_sample sample_history[8];
static volatile u8 requested_enabled;
static volatile u8 gyro_only;
static volatile u8 history_write_index;
static volatile u8 history_count;
static OS_SEM sample_due;
static u8 sample_due_ready;
static u8 polls_since_sample;

static void iic_bus_initialize(void)
{
    JL_IOMAP->CON1 = (JL_IOMAP->CON1 & ~IIC_ROUTE_MASK) | IIC_ROUTE_GROUP_C;
    gpio_set_hd(CYCLONE2_I2C_SCL, 1U);
    gpio_set_hd(CYCLONE2_I2C_SDA, 1U);
    gpio_set_pull_up(CYCLONE2_I2C_SCL, 0U);
    gpio_set_pull_up(CYCLONE2_I2C_SDA, 0U);
    gpio_set_pull_down(CYCLONE2_I2C_SCL, 0U);
    gpio_set_pull_down(CYCLONE2_I2C_SDA, 0U);
    gpio_set_die(CYCLONE2_I2C_SCL, 1U);
    gpio_set_die(CYCLONE2_I2C_SDA, 1U);

    JL_IIC->CON0 = 0U;
    JL_IIC->BAUD = IIC_BAUD_DIVISOR;
    JL_IIC->CON1 |= IIC_START_PENDING_CLEAR;
    JL_IIC->CON0 = IIC_PENDING_CLEAR | IIC_STOP_PENDING_CLEAR | IIC_ENABLE;
}

static void iic_start(void)
{
    JL_IIC->CON0 |= IIC_PRESET_START;
}

static int iic_stop(void)
{
    u32 timeout = IIC_WAIT_ITERATIONS;

    JL_IIC->CON0 |= IIC_PRESET_STOP | IIC_CONFIG_DONE;
    while (timeout-- != 0U) {
        if (JL_IIC->CON0 & IIC_STOP_PENDING) {
            JL_IIC->CON0 |= IIC_STOP_PENDING_CLEAR;
            return IMU_ERROR_NONE;
        }
    }
    return IMU_ERROR_TIMEOUT;
}

static int iic_transmit(u8 value)
{
    u32 timeout = IIC_WAIT_ITERATIONS;
    u32 control;

    JL_IIC->CON0 &= ~IIC_DIRECTION_IN;
    JL_IIC->BUF = value;
    JL_IIC->CON0 |= IIC_CONFIG_DONE;
    while (timeout-- != 0U) {
        control = JL_IIC->CON0;
        if (control & IIC_PENDING) {
            JL_IIC->CON0 |= IIC_PENDING_CLEAR;
            return control & IIC_SENT_NACK ? IMU_ERROR_NACK : IMU_ERROR_NONE;
        }
    }
    return IMU_ERROR_TIMEOUT;
}

static int iic_receive(u8 *value, u8 acknowledge)
{
    u32 timeout = IIC_WAIT_ITERATIONS;

    JL_IIC->CON0 |= IIC_DIRECTION_IN;
    if (acknowledge) JL_IIC->CON0 &= ~IIC_RECEIVE_NACK;
    else JL_IIC->CON0 |= IIC_RECEIVE_NACK;
    JL_IIC->BUF = 0xffU;
    JL_IIC->CON0 |= IIC_CONFIG_DONE;
    while (timeout-- != 0U) {
        if (JL_IIC->CON0 & IIC_PENDING) {
            JL_IIC->CON0 |= IIC_PENDING_CLEAR;
            *value = (u8)JL_IIC->BUF;
            return IMU_ERROR_NONE;
        }
    }
    return IMU_ERROR_TIMEOUT;
}

static int read_registers(u8 reg, u8 *values, u32 length)
{
    u32 index;
    int result;

    iic_start();
    result = iic_transmit(QMI8658A_ADDRESS);
    if (result != IMU_ERROR_NONE) goto done;
    result = iic_transmit(reg);
    if (result != IMU_ERROR_NONE) goto done;
    iic_start();
    result = iic_transmit(QMI8658A_ADDRESS | 1U);
    if (result != IMU_ERROR_NONE) goto done;
    for (index = 0U; index < length; index++) {
        result = iic_receive(values + index, index + 1U < length);
        if (result != IMU_ERROR_NONE) goto done;
    }

done:
    if (iic_stop() != IMU_ERROR_NONE && result == IMU_ERROR_NONE)
        result = IMU_ERROR_TIMEOUT;
    return result;
}

static int write_register(u8 reg, u8 value)
{
    int result;

    iic_start();
    result = iic_transmit(QMI8658A_ADDRESS);
    if (result == IMU_ERROR_NONE) result = iic_transmit(reg);
    if (result == IMU_ERROR_NONE) result = iic_transmit(value);
    if (iic_stop() != IMU_ERROR_NONE && result == IMU_ERROR_NONE)
        result = IMU_ERROR_TIMEOUT;
    return result;
}

static int record_result(int result)
{
    imu_status.last_error = (u8)result;
    if (result != IMU_ERROR_NONE) imu_status.errors++;
    return result == IMU_ERROR_NONE ? 0 : -result;
}

static int probe(void)
{
    u8 id = 0U;
    int result;

    iic_bus_initialize();
    result = read_registers(QMI8658A_REG_WHO_AM_I, &id, 1U);
    imu_status.device_id = id;
    if (result == IMU_ERROR_NONE && id != QMI8658A_DEVICE_ID)
        result = IMU_ERROR_BAD_ID;
    return record_result(result);
}

static int initialize(void)
{
    u32 index;
    int result = probe();

    imu_status.initialized = 0U;
    imu_status.asleep = 0U;
    if (result != 0) return result;
    for (index = 0U; index < sizeof(initialization) / sizeof(initialization[0]);
         index++) {
        result = write_register(initialization[index].reg,
                                initialization[index].value);
        if (result != IMU_ERROR_NONE) return record_result(result);
        /* QMI8658A soft reset needs time before the remaining configuration
         * writes. The normal BR23 I2C START itself requires no delay. */
        if (index == 0U) os_time_dly(2);
    }
    imu_status.initialized = 1U;
    return 0;
}

static s16 signed_word(const u8 *data)
{
    return (s16)((u16)data[0] | ((u16)data[1] << 8));
}

static int sample(struct cyclone2_imu_sample *sample_value)
{
    u8 data[12];
    u8 *gyro = data + 6U;
    int result;

    u32 started_us;

    if (!sample_value || !imu_status.initialized)
        return record_result(IMU_ERROR_NOT_INITIALIZED);
    started_us = cyclone2_time_us();
    if (gyro_only) {
        memset(data, 0, 6U);
        result = read_registers(QMI8658A_REG_GYRO_X_L, gyro, 6U);
    } else {
        result = read_registers(QMI8658A_REG_ACCEL_X_L, data, sizeof(data));
    }
    imu_status.read_us = cyclone2_time_us() - started_us;
    if (result != IMU_ERROR_NONE) return record_result(result);
    sample_value->accel_x = signed_word(data + 0U);
    sample_value->accel_y = signed_word(data + 2U);
    sample_value->accel_z = signed_word(data + 4U);
    sample_value->gyro_x = signed_word(data + 6U);
    sample_value->gyro_y = signed_word(data + 8U);
    sample_value->gyro_z = signed_word(data + 10U);
    sample_value->timestamp_us = cyclone2_time_us();
    imu_status.samples++;
    sample_value->sequence = imu_status.samples;
    imu_status.last_sample_ms = sys_timer_get_ms();
    return record_result(IMU_ERROR_NONE);
}

CYCLONE2_HOT void cyclone2_imu_tick(void)
{
    if (!sample_due_ready || !requested_enabled || !imu_status.initialized)
        return;
    if (++polls_since_sample < (gyro_only ? IMU_GYRO_POLLS : IMU_FULL_POLLS))
        return;
    polls_since_sample = 0U;
    os_sem_post(&sample_due);
}

void cyclone2_imu_enable(void)
{
    gyro_only = 0U;
    requested_enabled = 1U;
    imu_status.enabled = 1U;
    imu_status.enable_requests++;
}

void cyclone2_imu_enable_gyro(void)
{
    gyro_only = 1U;
    requested_enabled = 1U;
    imu_status.enabled = 1U;
    imu_status.enable_requests++;
}

/* The imu task puts the sensor to sleep: it owns the bus. */
void cyclone2_imu_disable(void)
{
    requested_enabled = 0U;
    imu_status.enabled = 0U;
    imu_status.disable_requests++;
}

int cyclone2_imu_latest(struct cyclone2_imu_sample *sample_value)
{
    u8 index;

    if (!sample_value || history_count == 0U) return -1;
    index = (u8)((history_write_index - 1U) & 7U);
    *sample_value = sample_history[index];
    return 0;
}

int cyclone2_imu_since(u32 sequence, struct cyclone2_imu_sample *samples,
                       int count)
{
    struct cyclone2_imu_sample copy[8];
    u8 end;
    u8 have;
    int found = 0;
    int index;

    local_irq_disable();
    memcpy(copy, sample_history, sizeof(copy));
    end = history_write_index;
    have = history_count;
    local_irq_enable();
    /* Newest first, back to the first one not after `sequence`.  The
     * oldest slot may be the one the task is writing: left out. */
    for (index = 1; index < have; index++) {
        const struct cyclone2_imu_sample *sample = &copy[(end - index) & 7U];

        if ((s32)(sample->sequence - sequence) <= 0) break;
        found++;
    }
    if (found > count) found = count;
    for (index = 0; index < found; index++)
        samples[index] = copy[(end - found + index) & 7U];
    return found;
}

/* Accelerometer and gyro off: the profile has no use for motion.  The
 * sensor keeps its configuration; enabling runs the full initialization
 * again anyway. */
static void sleep_sensor(void)
{
    iic_bus_initialize();
    imu_status.initialized = 0U;
    imu_status.asleep =
        write_register(QMI8658A_REG_CTRL7, QMI8658A_CTRL7_OFF) == IMU_ERROR_NONE;
}

void cyclone2_imu_task(void *private_data)
{
    struct cyclone2_imu_sample new_sample;
    u8 index;
    u32 next_retry_ms = 0U;

    (void)private_data;
    os_sem_create(&sample_due, 0);
    sample_due_ready = 1U;
    /* A soft reset leaves the sensor as the last session set it. */
    sleep_sensor();
    for (;;) {
        if (requested_enabled && imu_status.initialized) {
            /* Paced by cyclone2_imu_tick(); the timeout keeps sampling
             * going, slowly, should the polls stop. */
            os_sem_pend(&sample_due, 2);
            os_sem_set(&sample_due, 0);
            if (sample(&new_sample) == 0) {
                index = history_write_index;
                sample_history[index] = new_sample;

                local_irq_disable();
                history_write_index = (u8)((index + 1U) & 7U);
                if (history_count < 8U) history_count++;
                local_irq_enable();
            }
            continue;
        }
        if (requested_enabled) {
            u32 now = sys_timer_get_ms();

            if ((s32)(now - next_retry_ms) >= 0 && initialize() != 0)
                next_retry_ms = now + 1000U;
        } else if (imu_status.initialized || !imu_status.asleep) {
            sleep_sensor();
        }
        os_time_dly(1);
    }
}

void cyclone2_imu_get_status(struct cyclone2_imu_status *status)
{
    if (status) *status = imu_status;
}
