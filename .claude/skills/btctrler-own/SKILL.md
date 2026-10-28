---
name: btctrler-own
description: Port an assigned btctrler archive member to shared BR23/BD19 owned C, auditing both SDK IRs and preserving current app fixes. Use for object-sized controller ownership assignments.
---

An assignment names the integration commit, original member and occurrence,
and source files you own. `/work/cyclone2-fw-btctrler-vanilla` is reserved for the
coordinator on `btctrler/br23-bd19-vanilla`. Never use it as a worker checkout,
switch its branch, or run builds/tests there. The primary checkout is also
shared with unrelated work; do not edit it or run your checks there.

Inspect `git worktree list` and use a distinct worker path/branch based on the
assigned commit. If one was not supplied, create it with `git worktree add -b
btctrler/bd19-MEMBER-own /work/cyclone2-fw-btctrler-vanilla-MEMBER COMMIT` (substitute
actual names). Check the worktree root and current branch before editing.
Prepare dependencies and run all builds/tests in that worker worktree; missing
submodules do not justify switching to the coordinator or primary checkout.
Never edit another worker's files or the coordinator-owned manifest, headers,
glue and build rules. One member can span several C files. Do not merge,
push, flash or access hardware. Use `PATH=/work/bin:$PATH`.

Read `tools/btctrler/README.md` for the current commands and ownership schema.
The original IR is the authority; `tools/btctrler/irc.py` is a reading aid.
Your task is to adapt existing C, not repeat the BR23 decompilation. Compare
the assigned BD19 member, its BR23 counterpart, and the current C. Preserve
existing BR23 behavior and recorded app fixes. BD19 should first reproduce
its SDK behavior; identify app fixes that need a separate decision.

Start with `build/controller-ir/reuse.json` from `make controller-compare`.
For `reuse_candidate` functions already supplied by C, reuse that code: check
its current app changes, target compilation and ownership boundary rather
than translating or repeating a line-by-line IR audit. For matching instructions
with changed boundaries, focus on the reported types, globals and declarations.
Different instructions can be loop rotation/inlining or changed field indices;
establish the actual difference before writing a second implementation. The
census is conservative triage, not automatic whole-member acceptance. Check
local helpers and target-specific callees when accepting the source file.
Use `git log --follow -- SOURCE` and consult the archived object history when
needed; the platform-directory split hides older history from a plain path
log. Matching original SDK bodies do not establish that app-modified C still
matches either SDK.

For a whole-member replacement account for EVERY external function and global,
including currently unlinked definitions. Preserve sections, alignment,
initializers, volatility, aliases, calling conventions, MMIO addresses, IRQ
attributes, wide unaligned copies, integer widths and side-effect order.
Types with equal sizes can have different fields. Map GEPs by actual IR byte
layout, not debug field ordinal. Do not infer a value from the wrong local
enum; recovered colliding names are qualified. Retain SDK quirks.

BD19 has two `tws_pair.c.o` occurrences. Use the occurrence-specific inventory
and hash; never flatten archive extraction by filename. Check external
references, address-taken callbacks, assembly aliases and inlined copies when
assessing ownership. Do not delete a function merely because the current
firmware's LTO drops it.

Prefer common C for verified common semantics, a small compile-time selection
for small differences, and separate target implementations for substantial
hardware changes. Add focused host checks when a new behavioral difference
needs one; derive expected values from IR, not the implementation. Run the
existing member tests and compile the changed source with both target compilers.
If the assignment needs a coordinator-owned change, report the exact dependency;
do not work around it by using a BR23 layout/address on BD19.

Commit a reviewable checkpoint on your branch. Put the complete recovery
report in the commit message body, not only in the agent chat or an ephemeral
log. A subject-only commit is insufficient. Include:

- SDK member occurrence/hash and proposed manifest entry.
- Every external definition provided and globals/sections moved.
- BR23 behavior preserved; BD19 differences and SDK quirks.
- Tests and their expected-value provenance; exact commands/results.
- Uncertainties, unsupported dependencies, and findings worth revisiting.

Findings worth revisiting include candidate BR23/BD19 consolidation,
BR23-only application fixes that BD19 may need, SDK defects/undefined behavior,
no-op features and unresolved ABI or hardware questions. For each finding name
the function, evidence (SDK occurrence/IR operation or prior commit), target
scope and a concrete follow-up. Distinguish verified behavior from a hypothesis;
explain why a conditional is needed now and what would justify removing it.
Consult `src/btctrler/revisit-bd19.md` to avoid rediscovery.
Unless assigned that shared file, include proposed additions in your commit
body so the coordinator can update it without worker edit conflicts.

Do not claim hardware validation. After committing, the agent chat should only
identify the branch/commit, completion or blockers, and any immediate coordinator
action. The durable report belongs in git. Stop at the assigned boundary so the
coordinator can review against IR and integrate the batch. Keep your clean
worker checkout available until that review finishes. The coordinator removes
the worktree and branch after integration; do not create permanent review
branches or delete another worker's checkout.

This branch descends from `btctrler-decompiled` at `44d0073`. Previous BD19
reports originate on `btctrler/bd19-support` through `7cae22b`; consult their
commit bodies, but do not merge that app branch. Migration provenance is in
`tools/btctrler/vanilla-migration.md`. No hardware actions in this workflow.

For this controller-only migration validate with `make -j32 controller-objects
controller-test test controller-layouts controller-tools-test controller-audit`
and `make controller-compare`. Four-product app targets are on the preserved
application branch, not this branch. Existing baseline C is byte-identical to
44d0073; `bd19/shared/` holds imported recoveries pending proven sharing.
Do not promise strict SDK equivalence: the BR23 baseline includes earlier
recovery choices/guards recorded in revisit.md. Audit those separately.
