# Platform

Everything that depends on the Jieli BR23 SDK, its OS, or the Cyclone 2 board
is in three files.  The rest of the firmware sees only the `plat_` functions
below.

| File | Contains |
|---|---|
| `platform_jieli.c` | `app_main`, `board_init`, tasks, `plat_wait()` and its event sources, storage, the USB mass-storage device, the pin table, button/stick/trigger sampling, I2C on its interrupt, the LED chain, the motors, power-off and wake, the loader entry, the wake cause and buttons held at boot |
| `platform_jieli_sdk.c` | constants and hooks the SDK libraries require, which the firmware itself never reads |
| `btstack_hci_jieli.c` | BTstack on the Jieli controller (see [bluetooth.md](bluetooth.md)) |

## The `plat_` interface

Kept to what the firmware actually uses, not a general HAL.

```c
enum { EV_BUTTONS = 1, EV_USB = 2, EV_BT = 4, EV_IMU = 8, EV_DEADLINE = 0x80 };

enum { BOOT_POWER_ON, BOOT_HOME, BOOT_USB, BOOT_RESET };
struct plat_boot {
    u8 cause;
    u8 hat_held;
    u16 buttons_held;
};

u32 plat_ms(void);
u32 plat_us(void);                          /* wraps */
void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len);   /* starts; EV_IMU when done */
void plat_i2c_write(u8 dev, u8 reg, u8 value);
int plat_i2c_done(void);                    /* 1 done, 0 busy, -1 failed */
u32 plat_wait(u32 deadline_ms);
void plat_wake(u32 events);                 /* from another thread or an interrupt */
void plat_delay_ms(u32 ms);                 /* blocking; set-up only */
void plat_bt_thread(void (*body)(void));
void plat_bt_wait(u32 ms);                  /* the bluetooth thread sleeps until woken */
void plat_bt_wake(void);
void plat_lock(void);
void plat_unlock(void);
void plat_boot_info(struct plat_boot *out);
void plat_input(struct in_raw *raw);
void plat_leds(const struct led_frame *f);
void plat_motors(u8 left, u8 right);
int plat_usb_present(void);
void plat_usb_service(void);
u16 plat_battery_mv(void);
const u8 *plat_unique_id(void);
int plat_store_read(u16 key, void *buf, u16 len);
int plat_store_write(u16 key, const void *buf, u16 len);
u8 plat_retained_slot(void);
void plat_retain_slot(u8 slot);
void plat_power_off(void);
void plat_reboot(void);
void plat_reboot_loader(void);
```

Called by the platform: `ctl_main()` (pad.h, controller), once, on the control
thread after the board is up; it never returns.  `usb_command()` answers a
parked vendor command from the loop, through `usb_poll()`.

## The SDK contract

Measured from the linker's resolution table: what the Jieli archives need
from the application.  All of it goes in `platform_jieli_sdk.c` (constants)
or `platform_jieli.c` (functions).

| Symbol | Needed by |
|---|---|
| `app_main`, `board_init`, `task_info_table`, `eSystemConfirmStopStatus` | system.a |
| `private_scsi_cmd` | the SDK's mass-storage device (`msd.c`) |
| `vm_max_size_config`, `support_vm_data_keep`, `is_pwm_led_on`, `btif_table` | cpu.a, system.a |
| `user_send_cmd_prepare`, `sniff_negotiation_hook`, `config_update_mode` | btctrler.a |
| `btstack_assert_failed` | upstream BTstack |
| `config_btctler_*`, `CONFIG_*` (~50) | btctrler.a |
| `log_tag_const_*` (~110) | every archive |
| `config_asser`, `config_printf_time`, `config_system_info`, `CONFIG_CPU_UNMASK_IRQ_ENABLE`, `clock_sys_src_use_lrc_hw` | system.a, cpu.a |

Nothing else: the old imitation of the vendor host API is gone.  The SDK
`.c` files compiled are the `SDK_USED` list in the `Makefile`.

## Porting to another chip

Take a custom pad on an nRF52/nRF54 as the example.

| Part | Stays | Rewritten |
|---|---|---|
| controller, settings_ux, leds, input*, settings, utils | ✓ | |
| `usb.c` command handling | ✓ | the mass-storage class driver underneath |
| profiles | ✓ | |
| `profile_switch_wake.c` code generation | ✓ | the raw-radio burst (or leave Switch 1 wake out) |
| `bt.c` | ✓ with BTstack's nRF port | or reimplement the `bt_` interface on the vendor stack |
| `btstack_hci_jieli.c` | | BTstack's transport for that controller |
| `platform_jieli*.c` | | `platform_nrf.c`: the `plat_` list above |

Things kept in mind so this stays true, without adding layers now:

* No module outside the platform files includes an SDK or OS header.
* Threads are named by role (control, bluetooth), and the cross-thread
  traffic is two queues and one newest-wins slot inside `bt.c`, which also work on a single
  run loop.
* Timing is in milliseconds from `plat_ms()`, never OS ticks.
* The "hot" section attribute (run from RAM) is a macro the platform
  defines; elsewhere it is empty.
* Storage is a few opaque records (a bond per slot, calibration), so any
  key-value flash store will do.

## Sampling profiler

`plat_prof_start(hz, zoom, shift)` runs timer 2 at priority 6, above the
controller's BLE interrupts (2), the stick UART (3) and the tick (1).  Each
interrupt reads RETI, the address it will return to, and counts it in a
bucket: flash code (`text_code_begin`..`text_code_end`), RAM code
(`_data_code_begin`..`_data_code_end`) and the mask ROM (0x110000, 32 KB),
4096/512/512 u16 buckets of 64 bytes or more; a zoom puts the flash buckets
over a narrower window.  The period is jittered (up to 1023 counts of the
24 MHz clock) so samples do not lock to 5 ms connection events.  Code that
runs with interrupts off is counted where they come back on
(`local_irq_enable`).  `padtool profile` (USB PROFILE, usb.md) maps buckets
to functions through the ELF's DWARF (pyelftools), per function, file or
part (`--by`).

The CPU's sleep shows as samples returning to the start of `wdt_clear`,
attributed to it and its neighbours in `msd.c` (`check_disk_status`,
`msd_mcu2usb`).

