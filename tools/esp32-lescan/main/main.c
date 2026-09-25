/*
 * Passive LE scanner (ESP32, controller only) for the pad's LE bring-up.
 * Prints every advertisement that carries Nintendo's company ID (0x0553) with
 * its address and data, and a per-5-second count of everything else.
 */
#include <stdio.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_done;
static volatile uint32_t s_reports, s_nintendo;

static void ready(void) {}

static int recv(uint8_t *d, uint16_t len)
{
    if (len >= 3 && d[0] == 0x04 && (d[1] == 0x0e || d[1] == 0x0f)) {
        xSemaphoreGive(s_done);
    } else if (len >= 4 && d[0] == 0x04 && d[1] == 0x3e && d[3] == 0x02) {
        uint8_t n = d[4];
        const uint8_t *p = d + 5;
        for (uint8_t i = 0; i < n && p < d + len; i++) {
            uint8_t evtype = p[0], atype = p[1];
            const uint8_t *addr = p + 2;
            uint8_t dlen = p[8];
            const uint8_t *data = p + 9;
            int8_t rssi = (int8_t)data[dlen];
            s_reports++;
            int nintendo = 0;
            for (uint8_t k = 0; k + 3 < dlen;) {
                uint8_t fl = data[k];
                if (fl == 0) break;
                if (data[k + 1] == 0xff && fl >= 3 && data[k + 2] == 0x53 && data[k + 3] == 0x05)
                    nintendo = 1;
                k += fl + 1;
            }
            if (nintendo) {
                s_nintendo++;
                if (s_nintendo <= 20 || s_nintendo % 50 == 0) {
                    printf("LE NINTENDO %lu type=%u atype=%u addr=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d data=",
                           (unsigned long)s_nintendo, evtype, atype, addr[5], addr[4], addr[3],
                           addr[2], addr[1], addr[0], rssi);
                    for (uint8_t k = 0; k < dlen; k++) printf("%02x", data[k]);
                    printf("\n");
                }
            }
            p += 10 + dlen;
        }
    }
    return 0;
}

static const esp_vhci_host_callback_t s_cb = {ready, recv};

static void cmd(uint16_t op, const uint8_t *prm, uint8_t n)
{
    uint8_t b[64] = {0x01, (uint8_t)op, (uint8_t)(op >> 8), n};
    memcpy(b + 4, prm, n);
    while (!esp_vhci_host_check_send_available()) vTaskDelay(1);
    esp_vhci_host_send_packet(b, 4 + n);
    xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
}

void app_main(void)
{
    s_done = xSemaphoreCreateBinary();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    esp_vhci_host_register_callback(&s_cb);
    cmd(0x0c03, NULL, 0);                                   /* reset */
    uint8_t mask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};
    cmd(0x0c01, mask, 8);                                   /* event mask */
    uint8_t lemask[8] = {0x1f, 0, 0, 0, 0, 0, 0, 0};
    cmd(0x2001, lemask, 8);                                 /* LE event mask */
    uint8_t sp[7] = {0x00, 0x60, 0x00, 0x60, 0x00, 0x00, 0x00}; /* passive, 60 ms */
    cmd(0x200b, sp, 7);
    uint8_t se[2] = {0x01, 0x00};                            /* enable, no dup filter */
    cmd(0x200c, se, 2);
    printf("LE scanning\n");
    for (int t = 5;; t += 5) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        printf("LE status t=%ds reports=%lu nintendo=%lu\n", t,
               (unsigned long)s_reports, (unsigned long)s_nintendo);
    }
}
