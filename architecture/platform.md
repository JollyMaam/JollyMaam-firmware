# Platform

Everything that depends on the Jieli BR23 SDK, its OS, or the Cyclone 2 board
is in three files.  The rest of the firmware sees only the `plat_` functions
below.

| File | Contains |
|---|---|
| `platform_jieli.c` | `app_main`, `board_init`, tasks, the 1 ms clock, timers, storage, USB CDC, the pin table, button/stick/trigger sampling, I2C, the LED chain, the motors, power-off and wake, the loader entry, the boot chords |
| `platform_jieli_sdk.c` | constants and hooks the SDK libraries require, which the firmware itself never reads |
| `btstack_hci_jieli.c` | BTstack on the Jieli controller (see [bluetooth.md](bluetooth.md)) |

## The `plat_` interface

Kept to what the firmware actually uses, not a general HAL.

```c
/* time and threads */
u32  plat_ms(void);
u32  plat_us(void);                         /* wraps */
void plat_after(void (*fn)(void *), void *arg, u32 ms);   /* runs on the control thread */
void plat_lock(void);  void plat_unlock(void);            /* short critical sections */

/* storage: one key per settings record */
int  plat_store_read(u8 key, void *buf, u16 len);
int  plat_store_write(u8 key, const void *buf, u16 len);
const u8 *plat_unique_id(void);             /* flash UID, NULL if none */
void plat_random(u8 *buf, u16 len);

/* board */
void plat_input(struct in_raw *raw);
int  plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len);
int  plat_i2c_write(u8 dev, u8 reg, const u8 *buf, u8 len);
void plat_leds(const struct led_frame *f);
void plat_motors(u8 left, u8 right);
u8   plat_boot_chord(u8 *slot);             /* Home and keys held at power-on */
u16  plat_battery_mv(void);  int plat_charging(void);

/* USB: mass storage, no medium; vendor commands go to usb_command() */
int  plat_usb_present(void);

/* power */
void plat_power_off(void);                  /* wakes on Home or USB */
void plat_reboot(void);
void plat_reboot_loader(void);
void plat_radio_power(enum plat_tx_power p, int edr3, u8 in_flight);
```

Calls into the firmware:

```c
void ctl_start(void);    /* once, after the platform is up */
void ctl_poll(void);     /* every 1 ms, on the control thread */
void imu_task(void);     /* body of the imu thread */
```

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

Nothing else.  The imitation of the vendor host API that `bt/` has today
(`user_hid_*`, `edr_hid_*`, `bt_comm_*`, `bt_wait_phone_connect_control`,
`btstack_init`, the BLE stubs) is called only by our own code and goes away.

The SDK `.c` files compiled from `sdk-br23.mk` are cut down to those that end
up linked, and the `sdkconfig/*.h` headers to those they include.

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
* Threads are named by role (control, bluetooth, imu), and the cross-thread
  rules are latches and one newest-wins slot, which also hold on a single
  run loop.
* Timing is in milliseconds from `plat_ms()`, never OS ticks.
* The "hot" section attribute (run from RAM) is a macro the platform
  defines; elsewhere it is empty.
* Storage is three opaque records, so any key-value flash store will do.
