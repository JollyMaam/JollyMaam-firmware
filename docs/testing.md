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
3. exercises the commands that used to write flash (`report`, `newaddr`,
   `pair`, `connect`, `disconnect`) over the console,
4. asks you to pair the pad with a real console,
5. reads the whole flash back,
6. compares everything from the end of the application image onwards,
7. and, if anything up there moved, writes the original dump back and fails.

The compare starts at the end of the image because the image is what step 2
deliberately rewrote; everything above it is the persistent area (VM/syscfg,
BTIF, key/mac) that a RAM-only build must never touch.  A difference there
means something leaked into flash, and the restore is what keeps the pad from
being left with a half-written record.

    make itest                          # full run, prompts for the pairing
    make itest ITEST_FLAGS=--skip-pair  # machinery only, no console needed

With everything RAM-only the whole run should be flash-neutral, so there is
nothing to avoid any more: `newaddr`, `pair N fresh` and `unkey` change the
address in RAM for this boot only.  A run that passes leaves the pad on the
RAM-only build; dumps are kept under `/work/cyclone2-flash-backups/`.

## `tools/fuzz_links.py` — random connect / pair / disconnect against a console

Hunts the intermittent resets by driving the pad through padbus with random
`pair 0`, `connect 0` and `disconnect` commands at random intervals (0.3 s to
20 s, some short enough to cut a pairing or connection off halfway).

Setup:

1. padbus is running (`tools/padbus.py serve ...`); nothing else should hold
   control while the fuzzer runs (it takes control as `fuzz`).
2. The pad runs a build with markers: `CYCLONE2_PATCH_BTCTLER` set to a
   `"markers..."` mode in `src/build_config.h` (the LMP markers are what make
   a dump readable).
3. The Switch is **on and in Change Grip/Order**, so `pair 0` gets a fresh
   pairing and `connect 0` is a pad-initiated reconnect.  Nobody needs to
   touch the pad or the console during the run.

Run:

    tools/fuzz_links.py --minutes 10            # random seed, printed first
    tools/fuzz_links.py --minutes 10 --seed N   # replay a sequence

It turns the pad's debug log on, prints `RESET after action N: ...` whenever
padbus reports a reset, and at the end the number of actions and resets.
Each reset is written to `/work/nslog/fuzz-resets.txt` with the last twelve
actions (with timestamps) and a dump of the last two boots' marker rings,
taken straight after the reset so the new boot cannot overwrite the dying
one.  The fuzzer leaves the pad disconnected.

`tools/ringdump.py [--boots N] [--tail M]` is the dump on its own: it reads
the whole 2000-entry ring with `peek` (about 40 s) and prints it grouped by
boot, where `status` shows only the newest 96 markers.  It needs
`build/app.map` from the build that is on the pad.

Before flashing, `disconnect` first: entering the loader with `uboot` while
the pad is connected has twice left it off USB until a replug (2026-09-25).
