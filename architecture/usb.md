# USB: updates, logs, settings

There is no serial console.  Over USB the pad is a **mass-storage device with
no medium**, and everything a tool does goes through vendor SCSI commands on
that interface.  That is the same transport the chip's ROM loader (UBOOT1.00)
uses, so one tool and one transport cover:

* upgrading a stock pad,
* updating this firmware,
* reading logs and status,
* later, reading and changing settings.

`usb.c` (prefix `usb_`) implements the device side.

## Why mass storage

* **The ROM loader speaks it.**  UBOOT1.00 enumerates as a mass-storage device
  and takes its commands as vendor SCSI CDBs (`0xFB` write flash, `0xFD` read
  flash, `0xFC` other: erase, RAM write, jump, ...).  Any flashing tool must
  speak this, for stock pads at least.
* **The SDK already expects a running app to speak it too.**  The SDK's
  mass-storage device calls `private_scsi_cmd()` for every command it does not
  know (`apps/common/device/usb/device/msd_upgrade.c`); its stock handler
  answers any loader opcode by rebooting into the loader.  So a tool can find
  the pad, running or not, and always end up talking to the loader to flash.
* **Phones and PCs reach it without drivers:**
  * Android: the app claims the interface through the USB host API and does
    bulk-only transport itself.  No root, no driver.
  * Linux: SCSI generic (`/dev/sg*`, `SG_IO`).
  * Windows: SCSI pass-through on the disk device (to check: whether this
    needs administrator rights).
  * macOS: the kernel owns mass-storage devices; not supported at first.

## Getting a stock pad onto this firmware

1. The user holds Home+Start and plugs the pad in.  The stock firmware checks
   Home (PB11), Start (PA14) and USB present (PC3) at boot and enters the ROM
   loader (recovered from the stock image, see `RE_NOTES.md`).
2. The tool finds the device by its UBOOT1.00 inquiry string.
3. The tool reads the whole flash and keeps it as the user's backup: that is
   the way back to stock.
4. The tool writes the image and verifies it by reading back
   (`jlnewfw flash`).
5. The tool resets the pad.  The firmware starts with empty bonds and default
   settings and calibration.

## Gamepad or dongle

The GameSir dongle has the same chip and the same ROM loader, and in the
loader both answer "BR23 UBOOT1.00".  An image says what it is for in its
flash header: the PID, 16 plain bytes at offset 16 (`GS_C2_ADC_DEVICE` for
the pad, which our build sets; `GS_C2_Dongle`).  Before writing, `jlnewfw
flash` reads the target's header through the loader (at 0, 0x1000 or
0x10000, following `EOFFSET`) and refuses an image with another PID, or none
there (a blank flash, our builds before the PID was set), unless
`--force-pid`; and an image encrypted for another chip key unless
`--force-key` (`flashpack` packs for the chip's key).

A dongle does not boot from 0: its `isd_config` says `EOFFSET` 16 (4 KiB
units), and GameSir's tool puts the image at 0x10000, points the app area's
PRCT and VM entries at real flash addresses, and copies the first 4 KiB to 0
and 0x1000 (read back from a dongle it upgraded).  `jlnewfw unpack` of a
distributed image (`donglefw_*.bin`) and `jlnewfw flashpack` with `--mark
0x0fff --copy 0:0:0x1000 --copy 0:0x1000:0x1000 --copy 0x1000:0x2000:0x7000
--keep 0x60fe0:32` write it the same way (docs/flash-map.md).

## Updating this firmware

The same as above, except step 1: the tool sends the "reboot to loader"
command (or any loader opcode) to the running pad.  Home+Start at plug-in
still works as a fallback.

## Commands to the running firmware

The running firmware handles one vendor opcode, `0xF5` (to be checked as free
against the loader's set), with a sub-command in byte 1.  Everything is
little-endian.  Data goes in the SCSI data phase; the status is the CSW.

| Sub | Name | Data | Status |
|---|---|---|---|
| `0x00` | IDENTIFY | in (40): magic `C2PD`, u16 protocol, build ID (16 chars at 8), flash UID (16 at 24) | done |
| `0x01` | STATUS | in (16): state, slot, USB powered, low battery, u16 battery mV, u32 uptime ms, ephemeral (12), settings items collected (13) | done |
| `0x02` | LOG | CDB bytes 2–5: from sequence number; in: u32 first sequence returned, u32 length, text | done |
| `0x03` | STATS | CDB byte 2 bit 0: reset after reading; in: u16 version, u16 size, `struct ctl_stats` as in memory | done |
| `0x10` | REBOOT | – | done |
| `0x11` | REBOOT_LOADER | – (any loader opcode does the same) | done |
| `0x13` | CONNECT | CDB byte 2: slot (`0xfe` the last): start its session, as a Home connect does | done |
| `0x14` | PROFILE | CDB byte 2: 1 start at the rate in bytes 3-4 (samples/s), 2 stop, 0 read from the byte offset in bytes 3-6; in: header (version, rate, bucket shifts, the flash/RAM/ROM ranges and bucket counts, samples, outside, saturated, the last 8 outside addresses), then u16 counts per bucket, flash, RAM, ROM | done |
| `0x16` | TRACE | CDB byte 2: 1 start (clears), 2 stop, 0 read from the byte offset in bytes 3–6; in: u32 markers written, then 1024 entries `{u32 µs, u16 argument, u8 marker, u8 –}` (`util_trace()`, pad.h) | done |
| `0x15` | PEEK | CDB bytes 2–5: address; in: 32-bit words read from it (registers, RAM) | done |
| `0x12` | RELINK | – : while connected, take the session down and connect the same slot again (the host is awake; nothing is woken) | done |
| `0x20` | FIELD | argument: field number; in: u8 version, target kind (0 slot, 1 palette, 2 calibration), id, type, size, –, u16 max, then name, help, enum names (NUL-terminated), defaults (one per slot for slot fields, fields up to 4 bytes); all zero past the last | done |
| `0x21` | READ | CDB byte 2: target (0-7 slot, 8 palette, 9 calibration); in: u8 version, target, count, then per field id, set, len, value | done |
| `0x22` | WRITE | out: items `{target, id, len, value}`, collected; `len` 0: default; id 0: the whole target | done |
| `0x26` | RESULT | in: the last WRITE: u8 count, then a code per item (0 ok, 1 unknown, 2 bad length, 3 out of range, 4 full) | done |
| `0x23` | COMMIT | – : apply what was collected, in order; save unless ephemeral | done |
| `0x24` | DROP | – : throw away what was collected | done |
| `0x25` | EPHEMERAL | – : nothing is written to flash until the next boot | done |

The other SCSI commands return what makes operating systems leave the device
alone: INQUIRY answers, TEST UNIT READY says "no medium", READ CAPACITY
fails.  Nothing is mounted.

## Logs

`util_log(fmt, ...)` (in `utils.c`) appends a line to a RAM ring buffer of a
few kilobytes.  Each line has a sequence number and the time since boot
(`plat_ms()`), which `LOG` puts in front of the text as `seconds.ms`.  `LOG` returns lines from a
given sequence number on, so a tool can follow the log by polling, and can
tell when lines were overwritten before it read them.  The buffer can live in
the SRAM section that survives a soft reset, so the lines before a crash or
reboot can still be read afterwards.

## Interface

```c
struct ctl_request usb_poll(const struct usb_status *status);   /* every pass of the loop */
u32 usb_command(const u8 cdb[16], u8 *data, u32 len, int from_host);   /* the platform's SCSI hook */
```

`usb_poll()` runs on every pass of the loop (`EV_USB` wakes it).  It lets the
platform service a parked command (`plat_usb_service()`), which calls
`usb_command()` on the control thread; that answers it and returns at most one request (`REQ_REBOOT`,
`REQ_REBOOT_LOADER`, later others); the controller decides what to do with
it, as with gestures.  `status` is what the controller chooses to report
(state, slot, battery, calibration required, ...); `usb.c` keeps no copy of
the pad's state.  `IDENTIFY` comes from constants and `plat_unique_id()`,
`LOG` from the ring buffer in `utils.c`, the settings commands from
`settings.c` ([settings.md](settings.md)).  For a host-to-pad command the
platform reads the data first and passes it with `from_host` set.

The SDK's mass-storage driver runs outside the loop.  Its
`private_scsi_cmd()` hook (platform code) parks each vendor command until the
next `usb_poll()` takes it, and raises `EV_USB` to wake the loop; the host simply waits
for the status.  Whether a host is present (`plat_usb_present()`) is the
platform's, read by the controller for sleep decisions.

## Tools

The command set above is the contract.  Two clients use it:

* `tools/padtool.py` (Linux, SG_IO): `find`, `identify`, `status`,
  `log [-f]`, `stats`, `reboot`, `loader`, settings.  Flashing is
  `tools/jlnewfw.c`'s, through the ROM loader (a backup is a whole flash
  image: `jlnewfw flash` of the backup restores it).  Still to come: Windows.
* an Android app, later, for users without a PC.
