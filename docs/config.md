# Settings (`config`) and state (`status`)

Everything the pad remembers is a setting under `config`: the report
period, what each slot pretends to be, each slot's bond with its host, the
stick and trigger calibration.  What it is doing right now is under
`status`, read-only.  Both are described in code, one entry per leaf:
`src/config_schema.c` and `src/status.c`.  The design is
`plan/08-config.md`.

## On the console

One grammar in every namespace: the word after it is one of its verbs
(`get`, `reset`, ...), and anything else is a path.

    config                              help: the verbs and the groups
    config profile.B                    a setting, or everything under it
    config profile.B.type=xbox          set (spaces around = optional); says when it applies
    config profile.B.type help          what it is, its choices or range, its default
    config get [PATH]                   every setting; "# default X" marks a changed one,
                                        "not written" one that is not in flash yet
    config get --changed [PATH]         only what differs from the defaults, keys included,
                                        as plain lines that set it again
    config reset PATH                   PATH back to its default
    config reset --all [--no-commit]    every setting back to its default, written at once
                                        (--no-commit: not written; config commit keeps it)
    config pending                      changed and not in flash yet
    config commit                       write it now
    status radio                        what the pad is doing (status get, status help)

The groups are `global` (autocommit, report_ms), `identity`, `profile.A`..`Y`
and `cal`.  After `config` or `status`, a word that is not a verb is a path,
so `config profile` and `config profile.` both list the profile group.
Every line `get` prints is a command: `config get --changed` output
pasted back into the console restores those settings (a `# ...` comment is
ignored), which is all "import" is.  Every command and verb also takes
`help`, `-h` or `--help`, and `help WORDS` is `WORDS help`.  Tab completes
verbs and paths; a finished path gets its `=` on the next Tab, and the Tab
after that lists the choices.

The profile slots are named after the Home+button chord that selects them -
A, B, X, Y - and so are `connect`, `pair` and `debug wake`'s arguments.  The
sticks are `left`/`right`, the triggers `l2`/`r2`.  The numbers (0..3, 0..1)
work too, in any case, so `profile.2.type` and `profile.x.type`
are the same setting; listings and exports use the names.

A change never restarts anything.  `takes effect now` settings apply at
once; `takes effect on the next connect or pair` ones (a slot's type, its
address, its bond, the identity) wait for the slot to connect again -
`status radio.stale` says the running slot is out of date.  Nothing needs a
reboot today, though a setting may be marked so.

## When settings reach flash

* `global.autocommit=true` (the default): whatever changed is written
  100 ms after the last change, as long as the radio is quiet - off,
  connected, or paging with no link up, and in that state for half a second.
  Pairing, connecting with a link up, a profile switch and the Switch 1 wake
  hold writes back (the resets in `docs/reboot-hunting.md` were a syscfg
  access inside the SSP exchange and during a profile start).  The flash
  write itself runs with interrupts masked.
* `global.autocommit=false`: **nothing is written**, not a pairing and not
  this setting either.  `config commit` writes everything (at once if the radio is
  quiet, otherwise as soon as it is).  There is no revert: reboot, and the
  pad comes back with what flash holds - which includes autocommit, so a
  committed `false` stays `false`.
* `config reset --all` puts every setting back, autocommit included, and writes it
  at once.  `config reset --all --no-commit` turns autocommit off and resets the
  rest: `config commit` keeps it, a reboot undoes it.
* A pad powering itself off writes what is pending first (autocommit only).

This replaces the old `store ram|flash|revert`: `store ram` is
`config global.autocommit=false`, `store flash` is `config commit` (plus autocommit on),
`store revert` is a reboot.

## Bonds

One bond per profile slot, `profile.A.bond.*` (and B, X, Y):

| leaf | what |
|---|---|
| `peer` | the Classic host the slot pages on `connect A` and wakes on `debug wake A` |
| `link_key`, `key_type` | the Classic link key, as the stack keeps it |
| `le_peer`, `ltk` | the Switch 2 console and its LE long-term key |

BTstack's link-key database (`src/bt/btstack_jl_sdk.c`) is a view of the
running slot's bond: a key only ever goes to that slot's peer.  A pairing
fills the bond and pins the slot's address (below); `pair A` clears the keys.
`config get --changed profile.A` therefore holds everything a reconnect needs -
address included - and pasting it into another pad's console puts it there.
`tools/bond_capture.py` and `tools/bond_load.py` do that round trip through
files (they also read the old `BOND local=.. peer=..` files in
`/work/cyclone2-bonds/`).

The controller library's own pairing records in retention memory
(`debug bondstore`, `debug clearkeys`) are separate controller state, as
before.

## Addresses

`profile.A.address` (and B, X, Y) is the pad's Bluetooth address in
that slot.  Unset, it is derived - nothing stored - from a seed, the slot and
its type, forced to a locally administered unicast address, so a slot used
for another kind of pad looks like another device; `config` shows the derived
value with `# derived`.  The seed is the SPI flash chip's factory unique ID
(`status config.flash_id`, from the SDK's `get_norflash_uuid()`), unless
`identity.seed` is set; a flash without an ID gets a random seed drawn
once and stored in `identity.seed`.

A pairing pins the address it was made with into the setting.  `pair A
--fresh` moves it on: the upper three octets (the "vendor" half, with the
locally administered bit) stay, the lower three become a double hash of the
current address, and the result is stored.  So there is no counter - each
address leads to the same next one - and a host with a stale key for the old
address sees a pad it has never met.  `config reset profile.A.address` goes
back to the derived one; `config profile.A.address` shows the one in use,
derived or not.  The BTIF copy of the address (`CFG_BT_MAC_ADDR`) is not written.

## The flash format

The settings live in four kinds of syscfg (VM) item:

| item | holds |
|---|---|
| 48 | PREFS: autocommit, report period, identity, each slot's type and rumble |
| 49 | CAL: stick and trigger calibration |
| 34 + N | BOND: slot N's address and bond (A = 0 ... Y = 3) |

so a pairing rewrites only its slot's item (about 60 bytes).  An item is the
byte `0xC1` then records `key | length | value`, `key` a varint of
`uid << 3 | index`.  **Only values that differ from the default are stored**,
which is what makes the store safe to evolve:

* a new setting has no record, so it reads as its default and costs nothing;
* a changed default reaches every pad that never set the value;
* a record with an unknown uid, a wrong length or an invalid value is skipped
  (`status config.dropped_records` counts them), so older firmware reads
  newer flash;
* a uid is never reused - a setting whose meaning or type changes gets a new
  one, and the old one is listed as retired in `config_schema.c`.

`make config-test` runs the engine against an in-memory syscfg.

The VM itself sits right after the code limit (`CODE_LIMIT` in the
Makefile, now `0x50000`; `docs/flash-map.md`), so moving the code limit
starts every pad from defaults.
