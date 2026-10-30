This is an open source gamepad firmware.

It supports the following products:
  - Gamesir Cyclone 2 (TODO: add link to gamesir page that details on how to install)
  - Gamesir Nova 2 Lite (TODO: same)
  - 8BitDo Ultimate 2C Wireless (TODO: add 8bitdo link on how to install)

Q: Did a human write this README? Yes.

Q: Did I vibe code this project? Yes. (TODO: add HUMANS.md for my philosophy/blog about developing this stuff with AI)

Q: Did I expect to vibe code this when starting out? No.

Q: Why did you create this? The Cyclone 2 has a bunch of user experience features that annoyed me. Kids and guests would somehow keep entering turbo mode on accident. It only supported one possible configuration of profiles. I like open source. My keyboard runs QMK, my gamepad should run an open source firmware too!

Q: Who are you? A linux geek and professional embedded linux engineer for 10+ years.

Main features that have been added:
  - Nintendo Switch 2 Pro controller support. This includes waking the console, the back paddle buttons, the C button, and motion controls (when available).
  - DualSense controller support.
  - Up to 8 different connection slots. Connecting with Home+ABXY or Home+UpDownLeftRight switch to a different slot. Each slot can be independently configured to be any supported controller type. For example, you can have 8 different switch2 connections that connect to 8 different consoles, etc...
  - Up to 8 different remap slots. A remap slot is independent from a connection slot, e.g. it can be applied on top of any connection. Each remap slot can map any input to any output. Additionally there can be up to 16 rules that apply things like "if key is held for 200ms, enable turbo modifier for button B".
  - Show the "player number" LEDs when provided by the host so you know which pad corresponds to which player.
  - Configuration and firmware upgrades through USB or bluetooth (through a python-based text and graphical interface, a web-based interface, or android app).
  - Support multiple controllers connecting through a single dongle.

Note: Not all the gamepads support all the same features. For example, the 8BitDo Ultimate 2C only has 16KiB of RAM so it only supports 4 connection and remap slots and 8 rules per remap slot. Neither the Nova 2 Lite or Ultimate 2C Wireless support motion controls.

The top-level Nix flake pins the build environment in `flake.lock`:

```
nix develop                       # native C/Python tools
nix build .#linux                  # standalone Linux updater, core/platform archives
nix build .#windows                # Windows cross-build, core/platform archives
nix develop .#android              # Android SDK, NDK, JDK and build tools
nix develop .#emulator             # Android emulator and system image too
nix flake check                    # core, host and launcher regression checks
```

`nix develop .#static` supplies the musl static toolchain, and
`nix develop .#windows` supplies the Windows cross toolchain. Nix expressions
under `nix/` are implementation modules of this flake, not independent channel-
based environments. Current development environments target x86_64 Linux;
Android native builds produce separate target-ABI libraries.
