---
name: btctrler-review
description: Batch-review btctrler/<member> branches produced by other agents with the btctrler-own skill — check each against the SDK IR, fix or reject, merge onto rewrite, flash and test on hardware. Use when asked to review, merge or test agents' btctrler work.
---

# Batch review of btctrler branches

Workers follow `.claude/skills/btctrler-own/SKILL.md` and leave one commit
per member on branches `btctrler/<member>`.  You are the gate: nothing
reaches `rewrite` or the pad without passing this.

## 1. Collect

```
cd /work/cyclone2-firmware
git branch --list 'btctrler/*' 'worktree-agent-*'
git worktree list
```
For each branch: `git log --format='%h %s' rewrite..BRANCH` (expect one
commit) and `git show --stat BRANCH`.  A branch touching files outside
`src/btctrler/<member>.c`, `tools/btctrler/{recover,archive}.py` and
`tools/build_btctrler.py` needs a reason.

Make sure `build/controller-ir/sdk/` exists in the main checkout
(`make controller-ir`), since every check below compares against it.

## 2. Review each branch (in its worktree)

Read the commit's **Review notes**, "Unsure" first.  Then:

- **Functions:** read every owned function against
  `build/controller-ir/sdk/<member>.c.o.ll` (`tools/btctrler/irc.py` for
  pseudo-C, `.ll` as authority).  Check signedness, widths, wrap
  constants, argument order at the call sites, early returns, and the order of side
  effects.
- **Globals and tables:** compare bytes programmatically, not by eye.  Parse
  each `@name = ... c"..."` initializer from the IR and the C array, and
  compare them.  Check every global's `section "..."` and `const` against the C
  (a worker once claimed "plain const as in the SDK" when the SDK had a
  section).
- **Struct accesses:** any GEP on an SDK struct maps to the right member by
  byte offset (bitfield merging); `_Static_assert(offsetof)` present where
  it matters.
- **Leftovers:** every function left in the member really has no caller
  (`grep -l '@name(' build/controller-ir/retained/*.ll`, `grep -rn src/`).
- **Asserts and allocation:** the paths match the SDK (malloc vs lmp_malloc, free
  pairing, assert → `btctrler_assert_failed`).
- **Tests:** the expected values come from the IR semantics or a published
  source, not from running the new code.  Edge cases covered.
- **Tools:** run `make controller-ir` then
  `python3 tools/btctrler/crosscheck.py <functions>`; workers often skip
  this.
- **Behaviour changes:** any intentional deviation from the SDK is rejected
  unless the user decides otherwise.

Verdict per branch: **merge**, **merge with fixes** (you fix it and note
it in the commit), or **send back** (a SendMessage to the worker if it is still
reachable, or a note to the user).

## 3. Merge

On `rewrite` in the main checkout, cherry-pick the approved commits.
Conflicts are almost always the `PIPELINE`, `DROPPED`, `archive.py` import
lines and adjacent `recover.py` tables: keep both sides.  Renumber
the titles in sequence (`btctrler #N: ...`; see `git log`).  Amend messages to
record reviewer fixes, and keep both Co-Authored-By lines (worker and yours).
Then `make -j32` and `make test` (exit 0) on the merged tree.

## 4. Hardware test (the user has allowed PC tests; no rumble)

Flash and check the pad's clock (some boots run slow):
```
export PATH=/work/bin:$PATH
make flash
# then compare padtool status uptime over 4 s: it must advance ~4 s
```
Pick tests that run the new code; at minimum, on the PC (hostbt server,
see /work/cyclone2-bluetooth-benchmarks.md §5):
- `padtool connect B`, then `hostrx.py -t 30`: rate in line with the notes,
  no stuck buttons (decode the report's button bytes from the hostbt log).
- Encrypted reconnect (Link Key Request → Encryption Enabled in the hostbt log).
- For pairing/security members: a fresh pairing on slot X under
  `padtool ephemeral`, reconnect, then `bluetoothctl remove` on the PC
  and `padtool reboot` to drop it.
- Switch 2 (slot A) only if the user is there: connecting wakes the console.
Never wake the console or test rumble without the user.  Restore any setting
you change (prefer `padtool ephemeral` + reboot).

If something fails: bisect by building and flashing commits from the main
checkout (`git checkout <hash> && make flash`; builds take seconds and are
reproducible there, not in other worktrees, where path strings shift the
layout), then revert or fix only the first bad commit.

## 5. Clean up and report

Remove merged worktrees and branches (`git worktree remove`, `git branch -D`).
Report to the user, leading with the verdicts: per member, what was owned,
what the review found (SDK bugs worth knowing, worker mistakes), the
hardware results, and anything untested.  Update the memory status line in
`btctrler-assimilation-goal.md`.
