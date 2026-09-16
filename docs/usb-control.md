# USB control protocol

The firmware exposes one vendor-defined HID interface whenever USB is
connected. It uses unnumbered 64-byte input and output reports and is intended
to grow into the controller's configuration transport.

The first implemented command requests UBOOT1.00:

```sh
out/c2ctl uboot
```

An explicit hidraw path may be supplied if automatic discovery is ambiguous:

```sh
out/c2ctl uboot /dev/hidraw3
```

The 64-byte payload starts with `C2CFG`, protocol version 1, and two sentinel
bytes (`a5 5a`). Byte 8 is the command and byte 9 is its complement. Command
`01` requests UBOOT1.00; the acknowledgement uses command `81`. Its response
data contains a 16-byte snapshot of MaskROM's NVRAM block at `0x10800`.
Bytes 4 through 11 contain the USB boot token
`75 73 62 ed 63 81 67 ef`.

After acknowledgement the firmware delays briefly, forces a host-visible USB
detach, selects MaskROM USB boot state 2, and deliberately expires the
watchdog. The watchdog reset is important: the BR23 software-reset paths tested
during development either lost the NVRAM token or returned to the application,
while the watchdog reliably entered UBOOT1.00. If VBUS disappears during the
detach interval, the firmware waits rather than resetting into an unavailable
USB loader.

Command `02` reports the profile-selection reset mailbox followed by the same
16-byte NVRAM snapshot. It is primarily diagnostic.
