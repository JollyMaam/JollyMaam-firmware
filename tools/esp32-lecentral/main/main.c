/*
 * LE central test (ESP32, controller only): connect to the pad, exchange MTU,
 * read the GATT database by type, enable the response CCC and send a Switch 2
 * command, printing every ACL packet that comes back.  Target address below.
 */
#include <stdio.h>
#include <string.h>
#include "esp_bt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Pad slot 3: fa:09:3a:f3:ed:e1, public */
static const uint8_t PEER[6] = {0xe1, 0xed, 0xf3, 0x3a, 0x09, 0xfa};

static SemaphoreHandle_t s_done;
static volatile uint16_t s_handle = 0xffff;
static volatile uint32_t s_acl;

static void ready(void) {}

static int recv(uint8_t *d, uint16_t len)
{
    if (d[0] == 0x04) {
        if (d[1] == 0x0e || d[1] == 0x0f) xSemaphoreGive(s_done);
        else if (d[1] == 0x3e && (d[3] == 0x01 || d[3] == 0x0a)) {
            printf("C connect status=%u handle=%04x interval=%u\n", d[4], d[5] | d[6] << 8,
                   d[15] | d[16] << 8);
            if (d[4] == 0) s_handle = d[5] | (d[6] << 8);
        } else if (d[1] == 0x05) {
            printf("C disconnected reason=%02x\n", d[6]);
            s_handle = 0xffff;
        } else if (d[1] == 0x3e && d[3] == 0x0c) {
            printf("C PHY update status=%u tx=%u rx=%u\n", d[4], d[7], d[8]);
        } else if (d[1] != 0x13) {
            printf("C event %02x len=%u\n", d[1], len);
        }
    } else if (d[0] == 0x02) {
        s_acl++;
        printf("C ACL <");
        for (int i = 1; i < len && i < 40; i++) printf(" %02x", d[i]);
        printf("\n");
    }
    return 0;
}

static const esp_vhci_host_callback_t s_cb = {ready, recv};

static void cmd(uint16_t op, const uint8_t *p, uint8_t n)
{
    uint8_t b[64] = {0x01, (uint8_t)op, (uint8_t)(op >> 8), n};
    memcpy(b + 4, p, n);
    while (!esp_vhci_host_check_send_available()) vTaskDelay(1);
    esp_vhci_host_send_packet(b, 4 + n);
    xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
}

static void att(const uint8_t *pdu, uint8_t n)
{
    uint8_t b[64];
    uint16_t h = s_handle;
    b[0] = 0x02; b[1] = h & 0xff; b[2] = (h >> 8) & 0x0f;
    b[3] = n + 4; b[4] = 0;
    b[5] = n; b[6] = 0; b[7] = 0x04; b[8] = 0x00;
    memcpy(b + 9, pdu, n);
    while (!esp_vhci_host_check_send_available()) vTaskDelay(1);
    printf("C ACL >");
    for (int i = 5; i < 9 + n; i++) printf(" %02x", b[i]);
    printf("\n");
    esp_vhci_host_send_packet(b, 9 + n);
}

void app_main(void)
{
    s_done = xSemaphoreCreateBinary();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    esp_vhci_host_register_callback(&s_cb);
    cmd(0x0c03, NULL, 0);
    uint8_t mask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};
    cmd(0x0c01, mask, 8);
    uint8_t lemask[8] = {0xff, 0x0f, 0, 0, 0, 0, 0, 0};
    cmd(0x2001, lemask, 8);
    /* LE Create Connection: scan 60/60 ms, no whitelist, public peer,
     * interval 12..12 (15 ms), latency 0, timeout 200 (2 s). */
    uint8_t cc[25] = {0x60, 0x00, 0x60, 0x00, 0x00, 0x00};
    memcpy(cc + 6, PEER, 6);
    uint8_t *q = cc + 12;
    *q++ = 0x00;                               /* own address public */
    *q++ = 12; *q++ = 0; *q++ = 12; *q++ = 0;   /* interval */
    *q++ = 0; *q++ = 0;                        /* latency */
    *q++ = 200; *q++ = 0;                      /* timeout */
    *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0;    /* CE length */
    /* Restrict the data channels to 0..12, the map the Switch 2 console
     * uses (CONNECT_IND in the Pro Controller 2 capture: 1fff000000). */
    uint8_t chmap_all[5] = {0xff, 0xff, 0xff, 0xff, 0x1f};
    cmd(0x2014, chmap_all, 5);
    printf("C channel map: all\n");
    printf("C connecting\n");
    cmd(0x200d, cc, 25);
    for (int i = 0; i < 100 && s_handle == 0xffff; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_handle == 0xffff) { printf("C no connection\n"); return; }
    vTaskDelay(pdMS_TO_TICKS(500));
    /* LE Set PHY: prefer 2M both ways, as the Switch 2 console does. */
    uint8_t phy[7] = {s_handle & 0xff, s_handle >> 8, 0x00, 0x02, 0x02, 0x00, 0x00};
    cmd(0x2032, phy, 7);
    printf("C set PHY 2M requested\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint8_t mtu[] = {0x02, 0x00, 0x02};                  /* Exchange MTU 512 */
    att(mtu, sizeof(mtu));
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* Now update the map mid-connection: the controller sends the pad an
     * LL_CHANNEL_MAP_IND, as the Switch 2 console does. */
    uint8_t chmap[5] = {0xff, 0x1f, 0x00, 0x00, 0x00};
    cmd(0x2014, chmap, 5);
    printf("C channel map -> 0..12 (LL_CHANNEL_MAP_IND)\n");
    vTaskDelay(pdMS_TO_TICKS(2000));
    const uint8_t rbt[] = {0x08, 0x01, 0x00, 0xff, 0xff, 0x03, 0x28}; /* Read By Type chars */
    att(rbt, sizeof(rbt));
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint8_t ccc[] = {0x12, 0x1f, 0x00, 0x01, 0x00};      /* Write CCC 0x001f = 1 */
    att(ccc, sizeof(ccc));
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* Command 0x10/0x01 (firmware info) on 0x0016, no vibration prefix. */
    const uint8_t cmd10[] = {0x52, 0x16, 0x00, 0x10, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
    att(cmd10, sizeof(cmd10));
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("C done acl_in=%lu\n", (unsigned long)s_acl);
}
