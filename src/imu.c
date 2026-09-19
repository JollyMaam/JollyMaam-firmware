#include <asm/br23.h>
#include <asm/gpio.h>
#include <system/os/os_api.h>
#include <system/timer.h>

#include "imu.h"
#include "input.h"

#define QMI8658A_ADDRESS          0xd4U
#define QMI8658A_DEVICE_ID        0x05U
#define QMI8658A_REG_WHO_AM_I     0x00U
#define QMI8658A_REG_ACCEL_X_L    0x35U

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
#define IIC_BAUD_DIVISOR          119U
#define IIC_WAIT_ITERATIONS       100000U

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
static volatile u8 history_write_index;
static volatile u8 history_count;

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
    int result;

    if (!sample_value || !imu_status.initialized)
        return record_result(IMU_ERROR_NOT_INITIALIZED);
    result = read_registers(QMI8658A_REG_ACCEL_X_L, data, sizeof(data));
    if (result != IMU_ERROR_NONE) return record_result(result);
    sample_value->accel_x = signed_word(data + 0U);
    sample_value->accel_y = signed_word(data + 2U);
    sample_value->accel_z = signed_word(data + 4U);
    sample_value->gyro_x = signed_word(data + 6U);
    sample_value->gyro_y = signed_word(data + 8U);
    sample_value->gyro_z = signed_word(data + 10U);
    sample_value->timestamp_ms = sys_timer_get_ms();
    imu_status.samples++;
    imu_status.last_sample_ms = sample_value->timestamp_ms;
    return record_result(IMU_ERROR_NONE);
}

void cyclone2_imu_enable(void)
{
    requested_enabled = 1U;
    imu_status.enabled = 1U;
}

void cyclone2_imu_disable(void)
{
    requested_enabled = 0U;
    imu_status.enabled = 0U;
    imu_status.initialized = 0U;
}

int cyclone2_imu_latest(struct cyclone2_imu_sample *sample_value)
{
    u8 index;

    if (!sample_value || history_count == 0U) return -1;
    index = (u8)((history_write_index - 1U) & 7U);
    *sample_value = sample_history[index];
    return 0;
}

u8 cyclone2_imu_recent(struct cyclone2_imu_sample *samples, u8 maximum)
{
    u8 available;
    u8 index;
    u8 copied;

    if (!samples || maximum == 0U) return 0U;
    available = history_count;
    if (available > maximum) available = maximum;
    index = (u8)((history_write_index - available) & 7U);
    for (copied = 0U; copied < available; copied++) {
        samples[copied] = sample_history[index];
        index = (u8)((index + 1U) & 7U);
    }
    return available;
}

void cyclone2_imu_task(void *private_data)
{
    struct cyclone2_imu_sample new_sample;
    u8 index;
    u32 next_retry_ms = 0U;

    (void)private_data;
    for (;;) {
        if (requested_enabled) {
            if (!imu_status.initialized) {
                u32 now = sys_timer_get_ms();
                if ((s32)(now - next_retry_ms) >= 0 && initialize() != 0)
                    next_retry_ms = now + 1000U;
            } else if (sample(&new_sample) == 0) {
                index = history_write_index;
                sample_history[index] = new_sample;
                /* Publish only after the complete sample has been copied. */
                local_irq_disable();
                history_write_index = (u8)((index + 1U) & 7U);
                if (history_count < 8U) history_count++;
                local_irq_enable();
            }
        } else {
            imu_status.initialized = 0U;
        }
        os_time_dly(1);
    }
}

void cyclone2_imu_get_status(struct cyclone2_imu_status *status)
{
    if (status) *status = imu_status;
}
