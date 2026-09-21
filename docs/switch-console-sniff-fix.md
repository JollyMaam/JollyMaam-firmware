# Making the native Switch profile survive on a real console

Handoff from the `/work/jl-esp32` port.  It answered a question this project
shares: with the native Switch 1 HID profile, a real console pairs, opens the
HID control and interrupt channels, receives our 0x30 input reports, sends
nothing at all, and detaches the ACL with reason `0x13` about 300 ms after
`HID_OPEN`.

## The cause

Sniff mode.  The console sends `LMP_SNIFF_REQ` as soon as the HID channels are
open, and it wants a 24 slot (15 ms) interval.  The controller answers with
whatever `sniff_negotiation_hook()` puts in `*T_sniff` / `*attemp` /
`*timeout`, and this firmware leaves those at the library default by passing
`cfg.sniff_param = NULL`, so the answer was 800/100/4/1 slots - a 500 ms
interval.  No console will accept that.  It retries the negotiation three
times and then simply leaves:

    HID OPEN
    LMP_SNIFF_REQ     rx: D=0x16 T=0x18 A=0x1 TO=0x0     console wants 15 ms
    negotiation : 0x3, 1
    tx conn:          D=0x16 T=0x320 A=0x4 TO=0x0        we answer 500 ms
    form hook T:0x320, A:0x4, TO=0x1
    ... three times ...
    LMP_DETACH: 19

Nothing about the HID conversation is at fault.  The console never sends a
single HIDP byte, because it never gets as far as believing the link is set up.

## What to change

Two changes, both needed.  The first is in this repository.

### 1. Give the application real sniff parameters

`src/bt/app_comm_edr.c`, `edr_default_sniff_param`:

```c
 static const edr_sniff_par_t edr_default_sniff_param = {
     .sniff_mode = SNIFF_MODE_DEF,
     .cnt_time = 1,
-    .max_interval_slots = 800,
-    .min_interval_slots = 100,
-    .attempt_slots = 4,
-    .timeout_slots = 1,
+    .max_interval_slots = 24,
+    .min_interval_slots = 24,
+    .attempt_slots = 1,
+    .timeout_slots = 0,
     .check_timer_period = 1000,
 };
```

Better still, pass a `sniff_param` in the `edr_init_cfg_t` instead of relying
on the default, the way the SDK's own HID examples do
(`apps/hid/examples/*/app_*.c`, which use the short-interval group -
16/16/2/1 slots).  A Pro Controller is a HID device: it needs a short sniff
interval.  The library's 800 slot default is for low-power peripherals and is
not usable against a console.

Also add, next to the existing sniff setup in `btstack_edr_start_after_init()`:

```c
 lmp_set_sniff_establish_by_remote(1);
```

Do **not** add `lmp_set_sniff_disable()`.  It is the neighbouring call in the
SDK's own example, commented out for good reason: it is the opposite of what a
HID device wants.

### 2. Stop the controller library vetoing sniff

`lmp_esco_sniff_check()` in `cpu/br23/liba/btctrler.a` (`lmp.c.o`) has three
ways to answer "no" to an incoming `LMP_SNIFF_REQ`:

```c
if (conn->esco_conn != NULL)                    return 0;
if (lmp_get_esco_conn_statu() == 1)             return 0;
if (sniff_establish_by_remote == 0)             return 0;
if (is_call_now() == 0)                         return 1;   /* allowed */
return 0;
```

On a HID link with no eSCO there is nothing to veto, but this firmware hits
one of them anyway.  `lmp_set_sniff_establish_by_remote(1)` above only clears
the third, and it was **not sufficient** on its own: with it set and the
parameters fixed, 13 sessions still produced no subcommand.  Force the
function itself:

```llvm
define zeroext i8 @lmp_esco_sniff_check(...) {
entry:
  ret i8 1
}
```

`/work/jl-esp32/tools/patch_btstack.py --btctrler` does exactly this, plus the
trace retargeting described below, and writes `sdk-libs/btctrler.a`.  That
project's `Makefile` substitutes the patched archive for the vendor one, so
the SDK tree is never modified.  The script is self-contained and can be
copied over.

## How to see any of this

The controller library's HCI/LMP logging is compiled in and unreachable:
`apps/common/debug/debug.c` compiles `log_print()` to an empty stub unless
`CONFIG_DEBUG_ENABLE` is set, and `app_config.h` sets `LIB_DEBUG` to 0, so
every `CONFIG_DEBUG_LIB(x)` gate is zero.  Two things turn it on:

1. Set `LIB_DEBUG` to 1 so the tags are non-zero.
2. Retarget `log_print` (and `printf`, `puts`, `printf_buf`) in `lmp.c.o`,
   `lmp_hci.c.o`, `bredr_link.c.o`, `hci_controller.c.o`, `ll_events.c.o` and
   `link_layer.c.o` at a function that prints to your console, because the
   stock one goes nowhere.

The archive members are LLVM bitcode, so the toolchain's own clang can
disassemble them to IR, and `llvm-ar` can put the reassembled object back.
`/work/jl-esp32/tools/patch_btstack.py` is a working implementation.  With it
on, the console prints:

    CTRL2 : [Info]: [LMP]LMP_SNIFF_REQ
    CTRL2 : [Info]: [LMP]form hook T:0x18, A:0x1, TO:0x0

That single line is what turned this from guesswork into a five minute fix.

## What is *not* the problem

Each of these was measured and eliminated, so nobody needs to look at them
again:

* **L2CAP QoS.**  Adding the `ServiceType=Guaranteed` option a real Pro
  Controller sends is harmless and the console accepts it, but it changes
  nothing.
* **Encryption.**  The link is encrypted before the HID channels open, and the
  console is happy with it.
* **The report stream.**  A build that stays completely silent on the
  interrupt channel behaves identically, so the console is not reacting to
  what we send.
* **Identity.**  The HID report descriptor, the SDP record and the virtual SPI
  regions all match both the stock firmware and a working ESP32 emulator.
  The descriptor in `data/nintendo_switch_pro_hid.bin` is byte-identical to
  the one served.
* **The console never reads our SDP record.**  It opens PSM 0x0011 and 0x0013
  directly, in every capture, including with a brand new Bluetooth address.
  Do not expect to see an SDP connection to debug.
* **Debug logging load.**  A quiet build behaves exactly like a heavily
  instrumented one.

## Reference material

* `/work/jl-esp32` - the port, working, with the fix.  The commits that matter
  are `0e405dc` (the fix) and `4467d62` (the controller trace).
* `/work/jl-esp32-logs/controller/l2cap/` - captures.  `lmp-sniff-
  negotiation-fail.log` is the failure, `console-working-after-sniff-fix.log`
  is the success.
* `/shared/github.com-601-NS1-Wakeup-Demo` - an ESP32 classic-BT NS1 Pro
  Controller emulator that does work.  Its comment about power-mode changes
  colliding with the console's link control is the same problem seen from the
  other side, and its HID examples' short sniff intervals are the values to
  imitate.
* `/shared/github.com-TeamJCD-JoyConDroid` - Android Pro Controller emulator;
  its `pro_controller_eeprom.bin` matches what `swpro_spi.c` already serves.

## Still open

Buttons registered once on the console and then appeared dead, and the sticks
report 1-3% off centre (`18 88 80` / `28 88 8c` instead of `00 08 80`).  Both
are input-path issues in the port, not link issues, and are being looked at
separately.
