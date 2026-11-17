# Clean BR23/BD19 controller baseline

The working branch is `btctrler/br23-bd19-vanilla`, based directly on
`btctrler-decompiled` at `44d0073`, with `main` merged in. Its checkout is
`/work/cyclone2-fw-btctrler-vanilla`. The application integration remains
preserved on `btctrler/bd19-support` at `7cae22b`; do not merge that branch
here, because doing so would import later application changes.

“Clean” means the historical BR23 decompilation baseline. That baseline
already contains configuration specialization, assertion policy and some
recovery-era guards. This migration does not assert perfect SDK equivalence
or silently undo those choices. A stricter vanilla audit must compare those
choices with occurrence-specific original IR and keep its findings explicit.

## Imported recovery

All 74 accepted BD19 members are carried forward, supplying 908 public
functions and 67 globals through 79 source files. The retained archive still
contains 37 members and 1162 verified index entries. Original member hashes,
occurrence identities and ownership groups are preserved; paths are adapted
to this branch's `src/platform/jieli/btctrler/` layout. Original reports and provenance remain
in the commit bodies on `btctrler/bd19-support`; imported review notes and
revisit IDs are historical evidence, not claims of app-image validation here.

All 123 original root-level BR23 `.c` files are byte-identical to `44d0073`.
`sdk_br23.h` is byte-identical to its original `sdk.h`; a selector chooses the
independently generated BD19 header. Glue adds only the BD19 IRQ declaration
selection. None of the later app role-switch, scheduler-timeout, LE-version
response or receive-hook fixes were imported into BR23.

Fifteen unchanged sources are shared directly. Forty-eight formerly shared
imports are temporarily under `bd19/shared/`, alongside 16 existing dedicated
BD19 sources. This isolates added definitions, helper refactoring and target
conditionals from the BR23 baseline. It is a migration boundary, not a new
claim that these algorithms differ between chips. Consolidate them after
checking both original IRs and compiled boundaries; do not redecompile them.
Four imports required removing post-baseline BR23 source/test branches during
the initial three-way transplant (`link_layer`, `ll_slave`, `lmp_hci_pairing`,
`thread`). The final BD19 production IR still matches the previous recovery.

Consolidated afterwards: the 46 `bd19/shared/` sources with a BR23 counterpart
replaced it in `src/platform/jieli/btctrler/`; the two without one moved to `bd19/`. Checked
by unchanged BD19 objects (function bodies and globals) and a byte-identical
BR23 firmware image (same-length BUILD_ID). In BR23 objects they add
BD19-checked definitions the BR23 decompilation had left out (unlinked there),
and rearrange a few bodies without changing behaviour.

`migration-provenance.json` records source hashes and their original paths.
The comparison checked 123 BR23 objects / 2109 function bodies against fresh
objects built from `44d0073`, and 79 BD19 objects / 1198 bodies plus global
initializers and type declarations against the accepted `7cae22b` build.
Function comparisons use `tools.btctrler.compare.module(...)["canonical"]`;
global comparison ignores debug attachment IDs. Every comparison agrees.
This verifies preservation through migration; it does not replace the prior
member reviews or prove whole-program equivalence. Reports are also stored
under `/work/analysis/btctrler-vanilla-migration/` in this environment.

## Merged with main

`main` is merged in (its `7e90217` squash of `44d0073` is recorded as merged).
The controller lives at `src/platform/jieli/btctrler/`; `tools/btctrler/vanilla.mk`
uses `src/make/jieli.mk`'s chip toolchains. main's app still links the SDK
`btctrler.a` for BD19 (Nova 2 Lite dongle). main's own controller changes
are under `BTCTRLERMOD` (glue.h): on for BR23, off for BD19, whose objects stay
as audited until those changes are tested on its hardware.

## Dependencies and validation

`external/` holds main's submodules. In a worktree, point make at the main
checkout's copies instead of fetching them; `tools/controller.py` reads
`JIELI_SDK` from the environment (vanilla.mk exports it):

```
E=/work/cyclone2-firmware/external
V="JIELI_SDK=$E/jieli-fw-AC63_BT_SDK BTSTACK=$E/bluekitchen-btstack TELINK_SDK=$E/telink-tc_ble_simple_sdk/b80_ble_sdk"
PATH=/work/bin:$PATH make -j32 $V controller-objects controller-test \
  controller-layouts controller-tools-test controller-audit test
PATH=/work/bin:$PATH make $V controller-compare
PATH=/work/bin:$PATH python3 tools/btctrler/check_uecc.py
```

`controller-test` runs the BD19 builds of the controller tests; `test` (main's
host tests) runs the BR23 builds. No firmware with the BD19 controller has
been flashed or tested on hardware.

## Next work

To run our controller on BD19 in the app: drop `btctrler.a` from
`JIELI_BD19_ARCHIVES`, add the bd19.json sources to the BD19 images, and test
on the dongle. Then decide per `BTCTRLERMOD` change whether BD19 takes it.
Further consolidation means the dedicated `bd19/` sources whose BR23
counterparts are close (see the reuse census); keep the BR23 image unchanged.
