# Nonblocking stick acquisition

Own `src/input.c`, `src/input.h` and optional `docs/input-acquisition.md`.
Do not edit controller.c or Bluetooth files. This can be prepared alongside
USB tooling but is integrated only after the USB baseline has been tested.

The existing UART receive loop can spin 200000 times collecting 32 bytes. Use
the BR23 SDK UART driver/DMA ring if it supports the proven UART2 RX PA3 route
without commandeering PA2 (d-pad up), and preserves the measured divisor 24.
Otherwise use a short receive interrupt with a fixed ring. Verify register and
IRQ semantics against local SDK sources, not guessed constants. The consumer
must never wait for bytes. Retain sliding-window CRC parsing across batches.
Count dropped bytes/CRC failures, timestamp valid axes, and expose a compact
statistics getter. Define overflow recovery so it cannot silently parse stale
partial frames. No logging/BT calls from ISR, no dynamic allocation in our code.

Do not change stick scaling, trigger calibration, GPIO maps, or report format.
If the evidence is insufficient for a reliable IRQ/DMA implementation, report
the specific missing fact and prepare a reviewable proposal rather than guessing.
Document any cache/DMA alignment and ring ownership requirements. Compile the
affected object and hand off to the coordinator for flashing and live testing.
