# Image tool (`jlnewfw`)

The project's tool for the BR23 image format
([../jieli/re-application-image.md](../jieli/re-application-image.md)), built
for each board. Commands:

| Command | Does |
|---|---|
| `list` | show the JLFS entries of an image |
| `unpack` | write an image's files and an `isd_config.ini` |
| `pack` | build an image from files and an `isd_config.ini` |
| `flash` | write an image through the ROM loader |
| `flashpack` | `pack`, then `flash`, using the connected chip's key |
| `backup` | read the whole flash through the ROM loader |
| `loader` | ask running firmware to enter the ROM loader, then wait for the loader holding `--pid` |

Options used by the build:

- `--flash-size` and `--code-limit`: fix the layout so reserved regions do not
  move when code size changes.
- `--loader`: the SDK's `br23loader.bin`.
- `-c`: `isd_config.ini`. `-u`: `uboot.boot`.
- `--enter-loader --wait 30`: ask running firmware to enter the ROM loader,
  then wait for it. Firmware that shows itself as a disk gets a loader
  command. Stock firmware that shows itself only as HID gets the output
  report its board pages give, if `jlnewfw` lists it (the Nova 2 Lite:
  [../gamesir-nova-lite-2/](../gamesir-nova-lite-2/README.md)). On Linux the
  loader is followed by the USB port it comes back on.
- `-k`: re-encrypt for the target chip's key. `flashpack` does this by default.
- `--force-pid`: allow a write to a device whose PID differs (a blank device or a
  deliberate change).

`flash` writes only to a device whose flash holds the image's PID (the
Cyclone 2 PIDs are in [../gamesir-cyclone2/flash-map.md](../gamesir-cyclone2/flash-map.md)).

Unpack followed by pack, with `--at 0`, reproduces this project's build byte
for byte.
