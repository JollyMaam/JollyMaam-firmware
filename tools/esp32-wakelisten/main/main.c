/*
 * Wake-burst listener for the Cyclone 2 pad (ESP32, controller only).
 *
 * Starts an HCI CSB receive for the console address, then keeps overwriting
 * the receive control structure the way NS1-Wakeup-Demo does (bb_csb_rx.c):
 * the wake-up BCH parity instead of the standard one, a fixed channel with
 * hopping off, LT_ADDR 0 and whitening off.  The hardware records the clock
 * and bit offset at every access-code correlation in the control structure
 * (+0x52/+0x54/+0x56); a change there means the wake access code was heard,
 * whatever the packet type.  Every such change is printed.
 *
 * Addresses and offsets are the demo's, taken from ESP32 ROM/exchange memory.
 */
#include <stdio.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define CSB_RX_CS_ADDR 0x3ffb231cu
#define CS_LTADDR_OFFSET 0x08u
#define CS_LTADDR_MASK 0x0700u
#define CS_LINKCNTL_WHITENING_DSB 0x1000u
#define CS_BDADDR_OFFSET 0x0Au
#define CS_BCH_OFFSET 0x10u
#define CS_BCH_RX_FLAGS 0xFF40u
#define CS_HOPCNTL_OFFSET 0x16u
#define CS_HOPCNTL_FH_EN 0x8000u
#define CS_HOPCNTL_CHANNEL_MASK 0x7F00u
#define CS_RXCLK_LO 0x52u
#define CS_RXCLK_HI 0x54u
#define CS_RXBITOFF 0x56u
#define BT_CLKN_SAMPLE_REG 0x3ff7101cu

/* Console c8:48:05:00:54:b1 (display order) and its wake parity, computed
 * with the pad's own src/wake/wakeup_code.c. */
static const uint8_t TARGET[6] = {0xc8, 0x48, 0x05, 0x00, 0x54, 0xb1};
#define WAKE_PARITY34 0x086245c86ull
#define STD_PARITY34 0x079dba379ull
static uint64_t s_parity = WAKE_PARITY34;
static uint8_t s_channel = 0;

static SemaphoreHandle_t s_cmd_done;
static volatile uint8_t s_last_status;
static volatile uint32_t s_evt51, s_evt52;
static volatile uint32_t s_status[4];
static volatile uint8_t s_data_shown, s_data_len, s_data_new;
static uint8_t s_data[16];

#define REG16(off) (*(volatile uint16_t *)(CSB_RX_CS_ADDR + (off)))

static uint32_t read_clkn(void)
{
    volatile uint32_t *reg = (volatile uint32_t *)BT_CLKN_SAMPLE_REG;
    __asm__ __volatile__("memw" ::: "memory");
    *reg = (*reg & ~0x80000000u) | 0x80000000u;
    __asm__ __volatile__("memw" ::: "memory");
    for (int i = 0; i < 64; i++) {
        uint32_t v = *reg;
        if ((v & 0x80000000u) == 0) return v & 0x0fffffffu;
    }
    return 0;
}

static uint16_t hop_field(uint8_t channel)
{
    uint16_t v = channel >> 1;
    if (channel & 1) v += 40;
    return v;
}

static void reapply(void)
{
    volatile uint16_t *a = &REG16(CS_BDADDR_OFFSET);
    a[0] = (uint16_t)(TARGET[5] | (TARGET[4] << 8));
    a[1] = (uint16_t)(TARGET[3] | (TARGET[2] << 8));
    a[2] = (uint16_t)(TARGET[1] | (TARGET[0] << 8));
    volatile uint16_t *b = &REG16(CS_BCH_OFFSET);
    b[0] = (uint16_t)(s_parity & 0xffff);
    b[1] = (uint16_t)((s_parity >> 16) & 0xffff);
    b[2] = (uint16_t)(CS_BCH_RX_FLAGS | ((s_parity >> 32) & 3));
    REG16(CS_LTADDR_OFFSET) = (uint16_t)((REG16(CS_LTADDR_OFFSET) & ~CS_LTADDR_MASK) |
                                         CS_LINKCNTL_WHITENING_DSB);
    uint16_t h = REG16(CS_HOPCNTL_OFFSET);
    h &= (uint16_t)~CS_HOPCNTL_FH_EN;
    h = (uint16_t)((h & ~CS_HOPCNTL_CHANNEL_MASK) | (hop_field(s_channel) << 8));
    REG16(CS_HOPCNTL_OFFSET) = h;
}

static void vhci_send_ready(void) {}

static int vhci_recv(uint8_t *data, uint16_t len)
{
    if (len >= 3 && data[0] == 0x04) {
        uint8_t evt = data[1];
        if (evt == 0x0e && len >= 7) {           /* command complete */
            s_last_status = data[6];
            xSemaphoreGive(s_cmd_done);
        } else if (evt == 0x0f && len >= 4) {    /* command status */
            s_last_status = data[3];
            xSemaphoreGive(s_cmd_done);
        } else if (evt == 0x51) {
            s_evt51++;
            /* BD_ADDR(6) LT_ADDR(1) CLK(4) Offset(4) RX_Status(1) Fragment(1) Len(1) */
            if (len >= 3 + 18) {
                uint8_t st = data[3 + 15];
                if (st < 4) s_status[st]++;
                if (data[3 + 17] != 0 && s_data_shown < 5) {
                    s_data_shown++;
                    s_data_len = data[3 + 17];
                    memcpy(s_data, data + 3 + 18, s_data_len > 16 ? 16 : s_data_len);
                    s_data_new = 1;
                }
            }
        } else if (evt == 0x52) {
            s_evt52++;
        }
    }
    return 0;
}

static const esp_vhci_host_callback_t s_cb = {vhci_send_ready, vhci_recv};

static uint8_t cmd(uint16_t opcode, const uint8_t *params, uint8_t len)
{
    uint8_t buf[64];
    buf[0] = 0x01; buf[1] = opcode & 0xff; buf[2] = opcode >> 8; buf[3] = len;
    memcpy(buf + 4, params, len);
    while (!esp_vhci_host_check_send_available()) vTaskDelay(1);
    esp_vhci_host_send_packet(buf, 4 + len);
    if (xSemaphoreTake(s_cmd_done, pdMS_TO_TICKS(2000)) != pdTRUE) return 0xff;
    return s_last_status;
}

static uint8_t csb_rx(uint8_t enable)
{
    uint8_t p[34], *q = p;
    uint32_t next = (read_clkn() + 64) & 0x0ffffffeu;
    *q++ = enable;
    for (int i = 0; i < 6; i++) *q++ = TARGET[5 - i];
    *q++ = 1;                                 /* LT_ADDR (0 is written directly) */
    *q++ = 2; *q++ = 0;                       /* interval: 2 slots */
    *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0;   /* clock offset */
    *q++ = next & 0xff; *q++ = (next >> 8) & 0xff; *q++ = (next >> 16) & 0xff; *q++ = next >> 24;
    *q++ = 0x00; *q++ = 0x10;                 /* supervision timeout 0x1000 slots */
    *q++ = 250;                               /* remote timing accuracy, ppm */
    *q++ = 0;                                 /* skip */
    uint16_t types = 0x3306 | 0x0008;         /* no EDR, DM1 only: see bb_csb_rx.c */
    *q++ = types & 0xff; *q++ = types >> 8;
    for (int i = 0; i < 9; i++) *q++ = 0xff;
    *q++ = 0x7f;
    return cmd(0x0442, p, sizeof(p));
}

void app_main(void)
{
    s_cmd_done = xSemaphoreCreateBinary();
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    esp_vhci_host_register_callback(&s_cb);
    printf("WL reset=%u\n", cmd(0x0c03, NULL, 0));
    uint8_t mask[8] = {0, 0, 0x07, 0, 0, 0, 0, 0};
    printf("WL mask2=%u\n", cmd(0x0c63, mask, 8));
    printf("WL csb_rx=%u parity=0x%09llx channel=%u\n", csb_rx(1),
           (unsigned long long)s_parity, s_channel);

    uint16_t lo = REG16(CS_RXCLK_LO), hi = REG16(CS_RXCLK_HI), off = REG16(CS_RXBITOFF);
    uint32_t hits = 0, last_report = 0, restarts = 0, seen52 = 0;
    int64_t last_restart_us = esp_timer_get_time();
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        reapply();
        uint16_t nlo = REG16(CS_RXCLK_LO), nhi = REG16(CS_RXCLK_HI), noff = REG16(CS_RXBITOFF);
        if (nlo != lo || nhi != hi || noff != off) {
            hits++;
            if (hits <= 40 || (hits % 100) == 0)
                printf("WL SYNC %lu t=%lldms clk=%04x%04x bitoff=%u\n", (unsigned long)hits,
                       (esp_timer_get_time() - t0) / 1000, nhi, nlo, noff & 0x3ff);
            lo = nlo; hi = nhi; off = noff;
        }
        /* The supervision timeout frees the receive schedule without telling
         * us (NS1-Wakeup-Demo, switch1_csb_rx_restart), and a bare start is
         * then refused: stop and start again every 2 s, well inside the
         * 0x1000-slot (2.56 s) timeout. */
        int64_t now_us = esp_timer_get_time();
        if (s_evt52 != seen52 || now_us - last_restart_us > 2000000) {
            seen52 = s_evt52;
            last_restart_us = now_us;
            restarts++;
            csb_rx(0);
            csb_rx(1);
        }
        if (s_data_new) {
            s_data_new = 0;
            printf("WL DATA len=%u:", s_data_len);
            for (int i = 0; i < 16 && i < s_data_len; i++) printf(" %02x", s_data[i]);
            printf("\n");
        }
        uint32_t now = (uint32_t)((esp_timer_get_time() - t0) / 1000000);
        if (now != last_report && now % 5 == 0) {
            last_report = now;
            printf("WL status t=%lus hits=%lu evt51=%lu (st0=%lu st1=%lu st2=%lu st3=%lu) evt52=%lu restarts=%lu\n",
                   (unsigned long)now, (unsigned long)hits, (unsigned long)s_evt51,
                   (unsigned long)s_status[0], (unsigned long)s_status[1],
                   (unsigned long)s_status[2], (unsigned long)s_status[3],
                   (unsigned long)s_evt52, (unsigned long)restarts);
        }
        esp_rom_delay_us(50);
        if ((hits & 0) == 0) taskYIELD();
    }
}
