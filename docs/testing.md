# Testing on the pad

Two targets, both of which want the controller plugged in.  Neither one needs
a host Bluetooth adapter; the console commands are enough for everything
except a real pairing.

For what the tests are actually checking against, see `docs/flash-map.md` and
`docs/memory-map.md`.

## `make test` — firmware stress test

`tools/stress.py` drives the running firmware over its USB console and checks
that it survives the transitions that keep breaking it:

* profile switches across all four slots and repeated switches into one slot,
* the pairing -> disconnected -> connecting cycle,
* rapid bursts that land a switch mid-transition,
* a dwell past the 30 s connect timeout,
* the console commands that reach the bond store while the radio is stopped.

"Survives" means the console keeps answering, nothing fails that should
succeed, and the chip's preserved boot counter (`status` -> `boots=`) never
moves.  The boot counter is the reset detector; the console greeting is not
used, because the firmware re-sends it whenever the host's CDC line state
flaps, which reports reboots that did not happen.

    make test                       # default run
    make test TEST_ROUNDS=6         # more repetitions
    make test TEST_FLAGS=--quick    # skip the 33 s dwell

## `make itest` — flash-level integration test

`tools/itest.sh` answers a different question: *does the RAM-only build
(`BOND_STORE_RAM=1`, see `docs/bond-store.md`) really keep its hands off
flash?*  It:

1. dumps the whole flash,
2. builds and flashes the RAM-only image,
3. asks you to pair the pad with a real console,
4. reads the whole flash back,
5. compares everything from the end of the application image onwards,
6. and, if anything up there moved, writes the original dump back and fails.

The compare starts at the end of the image because the image is what step 2
deliberately rewrote; everything above it is the persistent area (VM/syscfg,
BTIF, key/mac) that a RAM-only build must never touch.  A difference there
means a bond leaked into flash, and the restore is what keeps the pad from
being left with a half-written record.

    make itest                          # full run, prompts for the pairing
    make itest ITEST_FLAGS=--skip-pair  # machinery only, no console needed

Pair normally: do **not** use `pair N fresh` or `unkey`, which are meant to
change the pad's own Bluetooth address in syscfg and so do write flash.  A run
that passes leaves the pad on the RAM-only build; dumps are kept under
`/work/cyclone2-flash-backups/`.
