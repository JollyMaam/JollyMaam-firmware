#include <system/includes.h>

#include "config.h"
#include "controller.h"
#include "gyro.h"
#include "imu.h"
#include "profile_playstation.h"

#define TOUCH_TAP_MS   200U
#define TOUCH_CLICK_MS 60U
#define TRIGGER_BUTTON_LEVEL 0x20U

/* CRC-32 (IEEE, reflected, as zlib's crc32()), four bits at a time. */
static u32 crc32(u32 crc, const u8 *data, u16 length)
{
    static const u32 table[16] = {
        0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
        0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
        0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
        0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU,
    };

    crc = ~crc;
    while (length--) {
        crc ^= *data++;
        crc = (crc >> 4) ^ table[crc & 0x0fU];
        crc = (crc >> 4) ^ table[crc & 0x0fU];
    }
    return ~crc;
}

void playstation_put_u16(u8 *output, u16 value)
{
    output[0] = (u8)value;
    output[1] = (u8)(value >> 8);
}

void playstation_put_u32(u8 *output, u32 value)
{
    output[0] = (u8)value;
    output[1] = (u8)(value >> 8);
    output[2] = (u8)(value >> 16);
    output[3] = (u8)(value >> 24);
}

void playstation_seal(u8 header, u8 report_id, u8 *body, u16 size)
{
    u8 prefix[2];
    u32 crc;

    prefix[0] = header;
    prefix[1] = report_id;
    crc = crc32(0U, prefix, sizeof(prefix));
    crc = crc32(crc, body, (u16)(size - 4U));
    playstation_put_u32(body + size - 4U, crc);
}

void playstation_buttons(const struct cyclone2_input_report *input, u8 out[3])
{
    u16 buttons = input->buttons;

    out[0] = input->hat <= CYCLONE2_HAT_NEUTRAL ? input->hat :
             CYCLONE2_HAT_NEUTRAL;
    if (buttons & CYCLONE2_REPORT_BUTTON_X) out[0] |= 0x10U;

    if (buttons & CYCLONE2_REPORT_BUTTON_A) out[0] |= 0x20U;
    if (buttons & CYCLONE2_REPORT_BUTTON_B) out[0] |= 0x40U;
    if (buttons & CYCLONE2_REPORT_BUTTON_Y) out[0] |= 0x80U;

    out[1] = 0U;
    if (buttons & CYCLONE2_REPORT_BUTTON_L1) out[1] |= 0x01U;
    if (buttons & CYCLONE2_REPORT_BUTTON_R1) out[1] |= 0x02U;
    if (cyclone2_trigger_u8(input->left_trigger) > TRIGGER_BUTTON_LEVEL)
        out[1] |= 0x04U;
    if (cyclone2_trigger_u8(input->right_trigger) > TRIGGER_BUTTON_LEVEL)
        out[1] |= 0x08U;
    if (buttons & CYCLONE2_REPORT_BUTTON_SELECT) out[1] |= 0x10U;
    if (buttons & CYCLONE2_REPORT_BUTTON_START) out[1] |= 0x20U;
    if (buttons & CYCLONE2_REPORT_BUTTON_L3) out[1] |= 0x40U;
    if (buttons & CYCLONE2_REPORT_BUTTON_R3) out[1] |= 0x80U;

    out[2] = 0U;
    if (buttons & CYCLONE2_REPORT_BUTTON_HOME) out[2] |= 0x01U;
    if (buttons & CYCLONE2_REPORT_BUTTON_M) out[2] |= 0x02U;
}

int playstation_motion(u8 out[12], u32 *timestamp_us)
{
    struct cyclone2_imu_sample sample;

    if (cyclone2_imu_latest(&sample) != 0) return -1;
    playstation_put_u16(out + 0, (u16)sample.gyro_y);
    playstation_put_u16(out + 2, (u16)sample.gyro_z);
    playstation_put_u16(out + 4, (u16)sample.gyro_x);
    playstation_put_u16(out + 6, (u16)(s16)-sample.accel_y);
    playstation_put_u16(out + 8, (u16)sample.accel_z);
    playstation_put_u16(out + 10, (u16)(s16)-sample.accel_x);
    *timestamp_us = sample.timestamp_us;
    return 0;
}

void playstation_touch_reset(struct playstation_touch *touch, u16 width,
                             u16 height)
{
    touch->width = width;
    touch->height = height;
    touch->down = 0U;
    touch->clicking = 0U;
}

void playstation_touch_update(struct playstation_touch *touch,
                              struct cyclone2_input_report *input, u32 now)
{
    const struct config_profile *settings =
        &config.profile[get_cur_bt_idx() % CONFIG_PROFILES];
    int held = (input->buttons & CYCLONE2_REPORT_BUTTON_M) != 0U;
    s32 x;
    s32 y;

    input->buttons &= (u16)~CYCLONE2_REPORT_BUTTON_M;
    /* Taken every report, so turns made without the finger are dropped. */
    cyclone2_gyro_take(settings->gyro_speed, settings->gyro_accel, &x, &y);
    if (touch->clicking) {
        if (now - touch->click_ms < TOUCH_CLICK_MS && !held) {
            input->buttons |= CYCLONE2_REPORT_BUTTON_M;
            return;
        }
        touch->clicking = 0U;
        touch->down = 0U;
    }
    if (held && !touch->down) {
        touch->down = 1U;
        touch->id = (u8)((touch->id + 1U) & 0x7fU);
        touch->x = touch->width / 2;
        touch->y = touch->height / 2;
        touch->start_ms = now;
    } else if (held) {
        touch->x += x;
        touch->y += y;
        if (touch->x < 0) touch->x = 0;
        if (touch->x > touch->width - 1) touch->x = touch->width - 1;
        if (touch->y < 0) touch->y = 0;
        if (touch->y > touch->height - 1) touch->y = touch->height - 1;
    } else if (touch->down) {
        if (now - touch->start_ms < TOUCH_TAP_MS) {
            touch->clicking = 1U;
            touch->click_ms = now;
            input->buttons |= CYCLONE2_REPORT_BUTTON_M;
        } else {
            touch->down = 0U;
        }
    }
}

void playstation_touch_fill(const struct playstation_touch *touch, u8 point[4])
{
    if (!touch->down) {
        point[0] = 0x80U;
        return;
    }
    point[0] = touch->id;
    point[1] = (u8)touch->x;
    point[2] = (u8)(((touch->x >> 8) & 0x0f) | ((touch->y & 0x0f) << 4));
    point[3] = (u8)(touch->y >> 4);
}
