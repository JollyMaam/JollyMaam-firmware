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
4. The tool writes the image and verifies it by reading back, as
   `tools/flash.sh` does now.
5. The tool resets the pad.  The firmware starts with empty bonds and default
   settings and calibration.

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
| `0x01` | STATUS | in (16): state, slot, USB powered, low battery, u16 battery mV, u32 uptime ms | done |
| `0x02` | LOG | CDB bytes 2–5: from sequence number; in: u32 first sequence returned, u32 length, text | done |
| `0x03` | STATS | CDB byte 2 bit 0: reset after reading; in: u16 version, u16 size, `struct ctl_stats` as in memory | done |
| `0x10` | REBOOT | – | done |
| `0x11` | REBOOT_LOADER | – (any loader opcode does the same) | done |
| `0x20` | SETTINGS_READ | record, offset → bytes | later |
| `0x21` | SETTINGS_WRITE | record, offset, bytes | later |

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
u32 usb_command(const u8 cdb[16], u8 *data, u32 max);          /* from the platform's SCSI hook */
```

`usb_poll()` runs on every pass of the loop (`EV_USB` wakes it).  It lets the
platform service a parked command (`plat_usb_service()`), which calls
`usb_command()` on the control thread; that answers it and returns at most one request (`REQ_REBOOT`,
`REQ_REBOOT_LOADER`, later others); the controller decides what to do with
it, as with gestures.  `status` is what the controller chooses to report
(state, slot, battery, calibration required, ...); `usb.c` keeps no copy of
the pad's state.  `IDENTIFY` comes from constants and `plat_unique_id()`,
`LOG` from the ring buffer in `utils.c`.

The SDK's mass-storage driver runs outside the loop.  Its
`private_scsi_cmd()` hook (platform code) parks each vendor command until the
next `usb_poll()` takes it, and raises `EV_USB` to wake the loop; the host simply waits
for the status.  Whether a host is present (`plat_usb_present()`) is the
platform's, read by the controller for sleep decisions.

## Tools

The command set above is the contract.  Two clients use it:

* `tools/padtool.py` (Linux, SG_IO): `find`, `identify`, `status`,
  `log [-f]`, `reboot`, `loader`, `flash IMAGE [--backup]`.  Flashing goes
  through the loader with the Jieli `jluboottool`.  Still to come: `restore`
  (a backup is a whole flash image, so it is `flash` of the backup), Windows.
* an Android app, later, for users without a PC.
