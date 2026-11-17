# Clean BR23/BD19 controller baseline

The working branch is `btctrler/br23-bd19-vanilla`, based directly on
`btctrler-decompiled` at `44d0073`. Its checkout is
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
to this branch's `src/btctrler/` layout. Original reports and provenance remain
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
replaced it in `src/btctrler/`; the two without one moved to `bd19/`. Checked
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

## Dependencies and validation

Read-only SDK/toolchain inputs are located at:

- `external/jieli-fw-AC63_BT_SDK` (pinned BR23/BD19 archive hashes checked).
- `external/toolchains/jieli-linux-toolchains-20250324.1`.
- `external/bluekitchen-btstack` (only needed for the historical app build).

`external/` is ignored. This checkout currently uses symlinks to the existing
integration's dependencies. Fresh worktrees can link these same read-only
inputs or provision independent copies. Never edit them or change refs there.

Run from this worktree:

```
PATH=/work/bin:$PATH make -j32 controller-objects controller-test test \
  controller-layouts controller-tools-test controller-audit
PATH=/work/bin:$PATH make controller-compare
PATH=/work/bin:$PATH python3 tools/btctrler/check_uecc.py
```

Validation passes: both target compilers, both host suites (193 `: ok` reports
and 12 legacy reports), both layouts, 31 tooling tests, 73 ownership groups,
retained archive content/order/index verification, cross-chip census and 740
independent curve checks. The historical BR23 inspector is preserved as
`legacy_archive.py` while the new occurrence-aware archive tooling handles
BD19. Ownership/review skills now target this worktree and require complete
reports in commit bodies and removal of completed worker worktrees/branches.

This branch provides controller builds/audits, not the newer four-product
application build. No firmware flashing or hardware validation was performed.

## Next work

Continue the 37 retained BD19 members from this clean branch, one worker at a
time when requested. Use the rebuilt reuse census before assigning a member.
Further consolidation means the dedicated `bd19/` sources whose BR23
counterparts are close (see the reuse census); keep the BR23 image unchanged.
Track stricter SDK-purity changes separately from migration and source sharing.
