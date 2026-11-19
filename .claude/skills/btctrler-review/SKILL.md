---
name: btctrler-review
description: Review and integrate BR23/BD19 controller ownership commits against original SDK IR, target layouts, and compiled symbol boundaries.
---

The canonical integration worktree is `/work/cyclone2-fw-btctrler-vanilla`, always on
`btctrler/br23-bd19-vanilla`. Do not move that branch into a review checkout or
switch the canonical checkout onto a worker branch. Confirm roots, branches
and status with `git worktree list` before changing anything.

Review a finished worker's commits in its existing isolated worktree; reuse
that branch for corrections once the worker has stopped, instead of creating
a branch/worktree for every review. Read the full commit body and uncertainties
first (use session logs only to recover older missing reports), then compare
every changed function/global with occurrence-specific BD19 IR, BR23 IR and
current BR23 behavior. Follow the sibling `btctrler-own/SKILL.md`; passing
host tests or a call-set match is not semantic/ABI proof. Keep coordinator
manifest/header/glue changes in the reviewed batch, not in an active worker's
files. Use a temporary review worktree only when isolation actually requires
one, and remove it after integration too.

Use the cross-chip census to avoid repeating the BR23 recovery. Matching
instructions and checked boundaries justify reusing the already-reviewed C;
review current app changes and compilation/ownership rather than decompiling
it again. Concentrate detailed IR review on boundary differences and changed
instructions. Equal C field names can compile to different target offsets;
raw byte offsets, allocation sizes and bit operations need explicit checks.

Check every initializer byte programmatically, plus sections and alignment.
Check widths, signedness, wrap behavior, under-aligned multi-byte copies,
callback lifetime, repeated loads/calls, reset/IRQ behavior and assembly-name
aliases. Preserve BR23 behavior at the clean 44d0073 baseline, including its recorded recovery limitations; do not import later app fixes. Record any BD19 application
of those fixes separately from faithful SDK ownership.

Use `tools/btctrler/README.md` for integration commands. Add manifest entries
only after code review. Whole-member replacement requires all external
definitions; `make controller-audit` checks compiled ownership against the
original member and that every SDK member stays owned (no archive). Verify both target
layouts, affected firmware builds, and relevant tests. Explain firmware image
differences where BR23 behavior should remain unchanged.

Commit reviewable checkpoints on the worker/review branch with the full report.
Merge the reviewed batch into `btctrler/br23-bd19-vanilla` from the canonical
integration worktree. Prefer merging the actual worker branch (including
review corrections) to cherry-picking, so ancestry proves it is integrated.
Validate the combined result there. Leave the primary checkout and active
workers untouched. Hardware tests require a separate
authorized stage and user-operated physical actions where needed; this skill
does not itself authorize flashing, host/console connections, or rumble.

Write the review report in the review/acceptance commit message body: input
commits, SDK member occurrences/hashes, exact exports/globals/sections,
corrections and evidence, preserved BR23 behavior, BD19 SDK quirks, validation
commands/results, image comparison, remaining uncertainty and revisits. Use a
body file for multiline git messages. A chat report does not replace this.
Update `src/platform/jieli/btctrler/revisit-bd19.md` with new consolidation
candidates, deferred app fixes, likely SDK bugs and unresolved behavior. Link
each entry to source/functions and durable commit/IR evidence; label inherited
BR23 observations that have not been confirmed on BD19. Do not leave corrected
translation errors listed as active SDK bugs. Mark resolved entries with the
resolution commit instead of silently deleting their history. Acceptance is
not complete until the reviewed changes are merged into the integration branch,
combined validation passes, and cleanup is finished.

After integration, remove the completed worker and temporary review worktrees
with `git worktree remove PATH`, invoked from the canonical checkout, then
remove their branches with `git branch -d BRANCH`. Check for tracked/untracked
changes and confirm each tip is an ancestor of integration first. Inspect
nested dependency repositories too. A clean submodule-only refusal can require
`--force`; it is not permission to erase changes. If a cherry-pick or later
correction obscures ancestry, establish the replacement mapping and preserve
unique commits before deleting refs. Keep pending/unreviewed worktrees and
record their status; do not discard them to make a cleanup look complete.
Recheck that the canonical worktree is clean on `btctrler/br23-bd19-vanilla`, and
that the primary checkout is untouched. The final chat names the integration
commit and any remaining pending work; keep it concise.

This branch descends from `btctrler-decompiled` at `44d0073`. Previous BD19
reports originate on `btctrler/bd19-support` through `7cae22b`; consult their
commit bodies, but do not merge that app branch. Migration provenance is in
`tools/btctrler/vanilla-migration.md`. No hardware actions in this workflow.

For this controller-only migration validate with `make -j32 controller-objects
controller-test test controller-layouts controller-tools-test controller-audit`
and `make controller-compare`. main is merged in; worktrees need the make
variables in tools/btctrler/vanilla-migration.md. A source in `src/platform/jieli/btctrler/` compiles for
both chips (`CONFIG_CPU_BD19` selects chip differences); `bd19/` holds BD19-only
sources. A shared-source change must leave the BR23 firmware image unchanged
unless the change is meant for BR23 too.
Do not promise strict SDK equivalence: the BR23 baseline includes earlier
recovery choices/guards recorded in revisit.md. Audit those separately.
