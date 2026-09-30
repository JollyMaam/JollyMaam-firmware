/* Host check for the Home gestures (src/ux.c): the table as it ships, driven
 * a millisecond at a time with the controller's actions stubbed out. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "calibration.h"
#include "controller.h"
#include "usb.h"
#include "ux.h"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

/* What the actions asked of the controller, in order. */
static char calls[256];
static u8 last_slot = CYCLONE2_UX_NO_SLOT;

static void record(const char *format, int value)
{
    size_t used = strlen(calls);

    snprintf(calls + used, sizeof(calls) - used, format, value);
}

int cyclone2_connect_request(u8 slot) { record("connect %d;", slot); return 1; }
int cyclone2_pair_request(u8 slot) { record("pair %d;", slot); return 1; }
int cyclone2_disconnect_request(void) { record("disconnect;", 0); return 1; }
void cyclone2_sleep_request(void) { record("sleep;", 0); }
u8 cyclone2_last_slot(void) { return last_slot; }
int cyclone2_calibration_start(void) { record("calibrate;", 0); return 0; }
void cyclone2_usb_debug(const char *format, ...) { (void)format; }

static u32 now;
static enum cyclone2_controller_kind kind;
static struct cyclone2_input_report pad;
static struct cyclone2_input_report host;

static void run(u32 ms)
{
    while (ms--) {
        now++;
        cyclone2_ux_poll(now, &pad, kind);
        host = pad;
        cyclone2_ux_filter(&host, now);
    }
}

static void press(u16 buttons) { pad.buttons |= buttons; run(1); }
static void release(u16 buttons) { pad.buttons &= (u16)~buttons; run(1); }

static void reset(enum cyclone2_controller_kind k)
{
    kind = k;
    memset(&pad, 0, sizeof(pad));
    pad.hat = CYCLONE2_HAT_NEUTRAL;
    run(10);
    calls[0] = 0;
}

#define HOME CYCLONE2_REPORT_BUTTON_HOME
#define SELECT CYCLONE2_REPORT_BUTTON_SELECT
#define A CYCLONE2_REPORT_BUTTON_A
#define B CYCLONE2_REPORT_BUTTON_B

int main(void)
{
    /* Offline, a Home tap connects the slot last used... */
    reset(CYCLONE2_CONTROLLER_IDLE);
    last_slot = 2U;
    press(HOME); run(80); release(HOME);
    CHECK(!strcmp(calls, "connect 2;"));
    /* ...and with none, does nothing. */
    reset(CYCLONE2_CONTROLLER_DISCONNECTED);
    last_slot = CYCLONE2_UX_NO_SLOT;
    press(HOME); run(80); release(HOME);
    CHECK(!strcmp(calls, ""));

    /* Not connected, Home+B connects at once; held on, it pairs. */
    reset(CYCLONE2_CONTROLLER_IDLE);
    press(HOME); press(B);
    CHECK(!strcmp(calls, "connect 1;"));
    run(4990);
    CHECK(!strcmp(calls, "connect 1;"));
    run(20);
    CHECK(!strcmp(calls, "connect 1;pair 1;"));
    release(HOME); release(B); run(100);
    CHECK(!strcmp(calls, "connect 1;pair 1;"));

    /* The d-pad picks slots too: left is slot 6. */
    reset(CYCLONE2_CONTROLLER_IDLE);
    pad.hat = CYCLONE2_HAT_LEFT; press(HOME);
    CHECK(!strcmp(calls, "connect 6;"));

    /* Connected, Home+B must be held to switch. */
    reset(CYCLONE2_CONTROLLER_CONNECTED);
    press(HOME); press(B); run(1900); release(B); release(HOME);
    CHECK(!strcmp(calls, ""));
    reset(CYCLONE2_CONTROLLER_CONNECTED);
    press(B); press(HOME); run(1990);
    CHECK(!strcmp(calls, ""));
    CHECK(!(host.buttons & (HOME | B)));
    run(20);
    CHECK(!strcmp(calls, "connect 1;"));

    /* Connected, a quick Home tap reaches the host after it is let go. */
    reset(CYCLONE2_CONTROLLER_CONNECTED);
    press(HOME); run(50);
    CHECK(!(host.buttons & HOME));
    release(HOME);
    CHECK(host.buttons & HOME);
    run(70);
    CHECK(!(host.buttons & HOME));
    CHECK(!strcmp(calls, ""));

    /* Held past the grace time it goes live, and chords stop counting. */
    reset(CYCLONE2_CONTROLLER_CONNECTED);
    press(HOME); run(200);
    CHECK(host.buttons & HOME);
    press(B); run(2500);
    CHECK(host.buttons & B);
    release(B); release(HOME); run(100);
    CHECK(!strcmp(calls, ""));

    /* Home+Select for two seconds disconnects, in any state. */
    reset(CYCLONE2_CONTROLLER_CONNECTED);
    press(SELECT); press(HOME); run(1900);
    CHECK(!strcmp(calls, ""));
    CHECK(!(host.buttons & (HOME | SELECT)));
    run(200);
    CHECK(!strcmp(calls, "disconnect;"));
    release(HOME);
    CHECK(!(host.buttons & SELECT));
    release(SELECT);
    reset(CYCLONE2_CONTROLLER_CONNECTING);
    press(HOME); press(SELECT); run(1900); release(SELECT); release(HOME);
    CHECK(!strcmp(calls, ""));

    /* Home alone for five seconds sleeps, and no tap follows. */
    reset(CYCLONE2_CONTROLLER_IDLE);
    last_slot = 0U;
    press(HOME); run(5010); release(HOME);
    CHECK(!strcmp(calls, "sleep;"));

    /* A chord that changes is dropped until all is let go. */
    reset(CYCLONE2_CONTROLLER_CONNECTED);
    press(HOME); press(B); run(500); press(A); run(6000);
    CHECK(!strcmp(calls, ""));
    release(A); release(B); run(100); release(HOME);
    CHECK(!strcmp(calls, ""));

    /* Blocked: what is held starts nothing. */
    reset(CYCLONE2_CONTROLLER_IDLE);
    pad.buttons = HOME | A;
    cyclone2_ux_block();
    run(6000);
    release(A); release(HOME);
    CHECK(!strcmp(calls, ""));

    /* Boot: a waking press already let go still counts. */
    reset(CYCLONE2_CONTROLLER_IDLE);
    last_slot = 3U;
    cyclone2_ux_boot(1, CYCLONE2_UX_NO_SLOT, &pad, kind);
    CHECK(!strcmp(calls, "connect 3;"));
    reset(CYCLONE2_CONTROLLER_IDLE);
    cyclone2_ux_boot(1, 1U, &pad, kind);
    CHECK(!strcmp(calls, "connect 1;"));
    /* Still held: the polls take it, as a tap on release. */
    reset(CYCLONE2_CONTROLLER_IDLE);
    pad.buttons = HOME;
    cyclone2_ux_boot(1, CYCLONE2_UX_NO_SLOT, &pad, kind);
    run(100);
    CHECK(!strcmp(calls, ""));
    release(HOME);
    CHECK(!strcmp(calls, "connect 3;"));

    if (failures) return 1;
    printf("ux: all checks passed\n");
    return 0;
}
