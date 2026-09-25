# ESP32 wake-burst listener

A controller-only ESP-IDF app for an ESP32 (tested: ESP32-D0WDQ6 rev 1.0,
ESP-IDF 6.1) that listens for the pad's Switch 1 wake bursts.  It starts an HCI
CSB receive for the console address and then keeps rewriting the receive
control structure the way NS1-Wakeup-Demo's `bb_csb_rx.c` does: wake-up
parity, fixed channel, LT_ADDR 0, whitening off.  The hardware stamps the
clock and bit offset at every access-code correlation (`+0x52..+0x56`); each
change is printed as `WL SYNC`.  Payloads are not captured (the CSB receive
buffer is 20 bytes and only DM1 is allowed, so a DH5 burst is never stored).

The target address and parity are constants in `main/main.c` (`TARGET`,
`WAKE_PARITY34`, `STD_PARITY34`); compute them for another console with the
pad's own `src/wake/wakeup_code.c`.  Channel is `s_channel` (default 0).

    nix develop /shared/esp-idf-v6.1-shell.drv --command bash -c \
      'cd tools/esp32-wakelisten && idf.py set-target esp32 && idf.py build && \
       idf.py -p /dev/bwrap/ttyUSB-esp0 flash'
    nix develop /shared/esp-idf-v6.1-shell.drv --command \
      python tools/esp32-wakelisten/mon.py 60 reset     # 60 s, resets the board

Result 2026-09-25 (docs/ns1-wakeup.md): with the wake parity, 0 correlations
before and after a 20 s `wake 0 0 20` and 22 during it; with the standard
parity, 0 during the same burst.
