# Settings (`config.*`) and state (`status.*`)

Everything the pad remembers is a setting under `config.`: the report
period, what each slot pretends to be, each slot's bond with its host, the
stick and trigger calibration.  What it is doing right now is under
`status.`, read-only.  Both are described in code, one entry per leaf:
`src/config_schema.c` and `src/status.c`.  The design is
`plan/08-config.md`.

## On the console

    config                              every setting; "# default X" marks a changed one,
                                        "not written" one that is not in flash yet
    config.profile.1                    one subtree
    config.profile.1.type = xbox        set (quotes optional); says when it takes effect
    help config.profile.1.type          type, choices or range, default, help text
    config.report_ms --help             the same
    unset config.profile.1              back to the defaults (`unset config`: all but autocommit)
    export [PATH]                       only what differs from the defaults, keys included,
                                        as lines `import` takes
    import  ...lines...  end            all accepted, or nothing changes (`abort` gives up)
    pending                             changed and not in flash yet
    commit                              write it now
    factory-reset [--no-commit]         every setting back to its default
    status[.PATH]                       e.g. status.radio, status.profile.0.address

Tab completes commands, paths, and after `=` the choices.

A change never restarts anything.  `takes effect now` settings apply at
once; `takes effect on the next connect or pair` ones (a slot's type, its
bond, the identity) wait for the slot to connect again -
`status.radio.stale` says the running slot is out of date.  Nothing needs a
reboot today, though a setting may be marked so.

## When settings reach flash

* `config.autocommit = true` (the default): whatever changed is written
  100 ms after the last change, as long as the radio is quiet - off,
  connected, or paging with no link up, and in that state for half a second.
  Pairing, connecting with a link up, a profile switch and the Switch 1 wake
  hold writes back (the resets in `docs/reboot-hunting.md` were a syscfg
  access inside the SSP exchange and during a profile start).  The flash
  write itself runs with interrupts masked.
* `config.autocommit = false`: **nothing is written**, not a pairing and not
  this setting either.  `commit` writes everything (at once if the radio is
  quiet, otherwise as soon as it is).  There is no revert: reboot, and the
  pad comes back with what flash holds - which includes autocommit, so a
  committed `false` stays `false`.
* `factory-reset` puts every setting back, autocommit included, and writes it
  at once.  `factory-reset --no-commit` is `config.autocommit = false`
  followed by `unset config`: `commit` keeps it, a reboot undoes it.
* A pad powering itself off writes what is pending first (autocommit only).

This replaces the old `store ram|flash|revert`: `store ram` is
`config.autocommit = false`, `store flash` is `commit` (plus autocommit on),
`store revert` is a reboot.

## Bonds

One bond per profile slot, `config.profile.N.bond.*`:

| leaf | what |
|---|---|
| `local` | the pad's address when the bond was made |
| `peer` | the Classic host the slot pages on `connect N` and wakes on `wake N` |
| `link_key`, `key_type` | the Classic link key, as the stack keeps it |
| `le_peer`, `ltk` | the Switch 2 console and its LE long-term key |

BTstack's link-key database (`src/bt/btstack_jl_sdk.c`) is a view of the
running slot's bond: a key only ever goes to that slot's peer.  A pairing
fills the bond and records the address it was made with; `pair N` clears
it.  `export config.profile.N` therefore holds everything a reconnect needs,
and `import` puts it back on any pad.  `tools/bond_capture.py` and
`tools/bond_load.py` do that round trip through files (they also read the
old `BOND local=.. peer=..` files in `/work/cyclone2-bonds/`).

The controller library's own pairing records in retention memory
(`bondstore`, `clearkeys`) are separate controller state, as before.

## Addresses

A slot uses its bond's `local` address when it has one.  Otherwise the
address is derived - nothing stored - from a seed, the slot, its type and
`config.profile.N.generation`, and forced to a locally administered unicast
address.  So a slot used for another kind of pad looks like another device,
and `pair N --fresh` (which bumps the generation) gives the slot an
identity no host has a stale key for.  `status.profile.N.address` shows
each slot's.

The seed is the SPI flash chip's factory unique ID
(`status.config.flash_id`, from the SDK's `get_norflash_uuid()`), unless
`config.identity.seed` is set.  A flash without an ID gets a random seed
drawn once and stored in `identity.seed`.  The BTIF copy of the address
(`CFG_BT_MAC_ADDR`) is no longer written.

## The flash format

The settings live in four kinds of syscfg (VM) item:

| item | holds |
|---|---|
| 48 | PREFS: autocommit, report period, identity, each slot's type/rumble/generation |
| 49 | CAL: stick and trigger calibration |
| 34 + N | BOND: slot N's bond |

so a pairing rewrites only its slot's item (about 60 bytes).  An item is the
byte `0xC1` then records `key | length | value`, `key` a varint of
`uid << 3 | index`.  **Only values that differ from the default are stored**,
which is what makes the store safe to evolve:

* a new setting has no record, so it reads as its default and costs nothing;
* a changed default reaches every pad that never set the value;
* a record with an unknown uid, a wrong length or an invalid value is skipped
  (`status.config.dropped_records` counts them), so older firmware reads
  newer flash;
* a uid is never reused - a setting whose meaning or type changes gets a new
  one, and the old one is listed as retired in `config_schema.c`.

`make config-test` runs the engine against an in-memory syscfg.

The VM itself sits right after the code limit (`CODE_LIMIT` in the
Makefile, now `0x50000`; `docs/flash-map.md`), so moving the code limit
starts every pad from defaults.
