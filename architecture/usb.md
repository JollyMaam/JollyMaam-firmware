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
still works as a fallback, as it does today.

## Commands to the running firmware

The running firmware handles one vendor opcode, `0xF5` (to be checked as free
against the loader's set), with a sub-command in byte 1.  Everything is
little-endian.  Data goes in the SCSI data phase; the status is the CSW.

| Sub | Name | Data | Status |
|---|---|---|---|
| `0x00` | IDENTIFY | in: magic `C2PD`, protocol version, firmware version and build ID, flash UID, Bluetooth address | now |
| `0x01` | STATUS | in: state, slot, profile, battery, charging, calibration required, link quality | now |
| `0x02` | LOG | out: from sequence number; in: next sequence number and text | now |
| `0x10` | REBOOT | – | now |
| `0x11` | REBOOT_LOADER | – (any loader opcode does the same) | now |
| `0x20` | SETTINGS_READ | record, offset → bytes | later |
| `0x21` | SETTINGS_WRITE | record, offset, bytes | later |

The other SCSI commands return what makes operating systems leave the device
alone: INQUIRY answers, TEST UNIT READY says "no medium", READ CAPACITY
fails.  Nothing is mounted.

## Logs

`util_log(fmt, ...)` (in `utils.c`) appends a line to a RAM ring buffer of a
few kilobytes.  Each line has a sequence number.  `LOG` returns lines from a
given sequence number on, so a tool can follow the log by polling, and can
tell when lines were overwritten before it read them.  The buffer can live in
the SRAM section that survives a soft reset, so the lines before a crash or
reboot can still be read afterwards.

## Interface

```c
void usb_init(void);
void usb_poll(void);          /* from ctl_poll */
int  usb_present(void);       /* a host is sending frames */
```

The SDK's mass-storage class driver and `private_scsi_cmd()` hook are
platform code; `usb.c` gets each vendor command as
`usb_command(const u8 cdb[16], u8 *data, u32 len)` and returns a status.

## Tools

The command set above is the contract.  Two clients use it:

* a PC command-line tool (`padtool`): `flash`, `backup`, `restore`, `log -f`,
  `status`, `reboot`.  This replaces `tools/flash.sh`, `padbus.py` and
  `pad_tui.py`.
* an Android app, later, for users without a PC.

Development (and the agent's hardware session) moves to `padtool log -f` and
`padtool flash`.  Build `usb.c` and `padtool` before the CDC console is
removed, so there is never a build with no way to read logs or reach the
loader.
