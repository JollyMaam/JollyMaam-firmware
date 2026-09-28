#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "profile_switch_wake.h"
#include "profile_switch_wake_radio.h"

struct config config;
static u32 now;
static int opened, transmitting, opens, closes, starts;
static int open_error, transmit_error;

u32 sys_timer_get_ms(void) { return now; }
void cyclone2_usb_debug(const char *format, ...) { (void)format; }

int jieli_btctrler_open(const uint8_t *address)
{
    (void)address;
    assert(!opened);
    opens++;
    opened = !open_error;
    return open_error;
}

void jieli_btctrler_close(void)
{
    assert(!transmitting);
    closes++;
    opened = 0;
}

int wake_radio_start(const struct wake_radio_config *radio,
                     const u8 *body, u16 size)
{
    assert(opened && !transmitting);
    assert(size == 339 && body);
    assert(!memcmp(radio->address, config.profile[0].bond.peer, 6));
    starts++;
    transmitting = !transmit_error;
    return transmit_error;
}

void wake_radio_stop(void) { transmitting = 0; }

static void reset(void)
{
    wake_stop();
    memset(&config, 0, sizeof(config));
    config.profile[0].bond.peer[0] = 1;
    now = 0;
    opened = transmitting = opens = closes = starts = 0;
    open_error = transmit_error = 0;
}

int main(void)
{
    struct wake_status status;
    reset();
    assert(wake_start(0, 79, 8) == -2);
    assert(wake_start(0, 0, 0) == -2);
    assert(wake_start(0, 0, 31) == -2);
    assert(wake_start(1, 0, 8) == -1);
    assert(opens == 0);

    assert(wake_start(0, 0, 8) == 0);
    assert(wake_start(0, 0, 8) == -2);
    assert(wake_step(249) == 0 && starts == 1);
    assert(wake_step(250) == 0 && starts == 2);
    assert(wake_step(8000) == 1 && !transmitting);
    wake_stop();
    wake_stop();
    assert(!opened && opens == 1 && closes == 1);
    wake_status_get(&status);
    assert(!status.active && status.elapsed_ms == 8000);

    reset();
    assert(wake_start(0, 0, 8) == 0);
    wake_stop();
    assert(!opened && !transmitting && closes == 1);
    assert(wake_step(100) == 1);
    assert(wake_start(0, 0, 8) == 0);
    wake_stop();
    assert(opens == 2 && closes == 2);

    reset();
    open_error = -7;
    assert(wake_start(0, 0, 8) < 0);
    assert(!opened && !wake_active() && starts == 0 && closes == 1);
    wake_stop();
    assert(closes == 1);

    reset();
    transmit_error = -4;
    assert(wake_start(0, 0, 8) < 0);
    assert(!opened && !transmitting && !wake_active() && closes == 1);

    reset();
    assert(wake_start(0, 0, 8) == 0);
    transmit_error = -4;
    assert(wake_step(250) == 1);
    wake_stop();
    assert(!opened && !transmitting && closes == 1);
    wake_status_get(&status);
    assert(status.last_error == -3);

    reset();
    now = UINT32_MAX - 500;
    assert(wake_start(0, 0, 1) == 0);
    assert(wake_step(now + 999) == 0);
    assert(wake_step(now + 1000) == 1);
    wake_stop();
    assert(!opened && closes == 1);
    puts("wake session: all checks passed");
    return 0;
}
