---
name: btctrler-own
description: Translate one member of the SDK's btctrler.a (LLVM bitcode) into owned C under src/btctrler/ and drop it from the archive, ready for batch review. Use when assigned a btctrler member ("own bredr_slot_timer.c", "take btcontroller_heap").
---

# Own one btctrler member

You turn one SDK archive member (e.g. `bredr_utils.c.o`) into owned C that
does exactly what the SDK did, prove it with host tests, and commit it on
your own branch.  A reviewer checks every commit against the SDK IR and
tests it on hardware, in batches.  Faithful beats clever: an "improvement"
that changes behaviour is a defect here.

## Hard rules

- **No hardware.**  Do not run `make flash`, `tools/padtool.py`,
  `/work/cyclone2-esp32/hostbt/*`, or touch `/dev/bwrap`.  A person uses the
  pad.  Host builds and host tests only.
- **Your own worktree and branch.**  Never edit the main checkout.  Start with:
  ```
  cd /work/cyclone2-firmware
  git worktree add ../cyclone2-fw-<member> -b btctrler/<member> rewrite
  cd ../cyclone2-fw-<member>
  ```
  (`<member>` without `.c.o`, e.g. `bredr_utils`.)  If your harness already
  gave you an isolated worktree, use it and name the branch the same way.
- One member per branch.  Do not touch other members' code, upstream
  BTstack, `docs/` or `architecture/`.
- Keep SDK behaviour, including bugs and odd constants.  If you think the SDK
  is wrong, keep it and say so in a comment and in the commit's review notes.
- Use `PATH=/work/bin:$PATH` for `make`.  You run in a bwrap container; do not
  try to break out.

## How the pipeline works (read before starting)

- `tools/build_btctrler.py` builds `build/sdk-libs/btctrler.a` from the
  pristine SDK archive.  `PIPELINE` maps a member to transforms in
  `tools/btctrler/recover.py`; `DROPPED` members are left out of the archive.
- A transform calls `own_all(text, TABLE)` (`tools/btctrler/ir.py`): `TABLE`
  maps each function you replace to the SHA-256 of its SDK body text.  The
  build fails if a body differs, which is how we know the C replaces the code
  that was reviewed.  The definition becomes a declaration, so the linker
  uses your C.
- Then `leftovers(text, 'member.c', {...})` asserts exactly which SDK
  definitions remain: only functions with no caller anywhere (often
  `cpu_reset`, a local assert helper).  Anything with a caller gets
  translated.
- `tools/btctrler/archive.py` collects every `*_OWNED` table into `OWNED`;
  add yours.
- **Model to copy:** `src/btctrler/lmp_utils.c`, `LMP_UTILS_OWNED` and the
  `lmp_utils` transform in `recover.py`, its `PIPELINE`/`DROPPED` entries.
  Smaller recent examples: `bredr_utils.c`, `authentication.c`.

## Steps

1. `make -j32 && make controller-ir` in your worktree.  This writes the SDK
   IR to `build/controller-ir/sdk/<member>.c.o.ll` (what you translate) and
   the retained archive's IR to `build/controller-ir/retained/` (the callers).
2. List the member's definitions and globals:
   `grep -E '^(define|@)' build/controller-ir/sdk/<member>.c.o.ll`.
   For each function, find its callers: `grep -l '@name(' build/controller-ir/retained/*.ll`
   and `grep -rn 'name(' src/`.  No caller anywhere means it may stay as a leftover.
3. Read each body.  `python3 tools/btctrler/irc.py FILE.ll func1 func2` prints
   pseudo-C; the `.ll` text is the authority when they disagree.
4. Write `src/btctrler/<member>.c`:
   - Include `"btctrler/glue.h"`.  Use SDK names and the types in
     `src/btctrler/sdk.h` (generated from debug info).  Prototypes must match
     sdk.h exactly; owned C builds with `-Werror=implicit-function-declaration`.
   - Move the member's globals too (the member is dropped).  Keep each
     global's `section "..."` with `__attribute__((section("...")))`, its
     `const`-ness and its initial value.  Check every global's section; do
     not assume.
   - Start the file with a short comment: what the code is for and which
     SDK quirks are kept.  Match the comment density of sibling files.
   - Asserts: the SDK's `ASSERT` pattern (`config_asser` → printf +
     `cpu_assert_debug`, else reset) is `btctrler_assert_failed(what, value)`.
5. Fingerprints: `python3 -c "import sys; sys.path.insert(0,'tools'); from btctrler.ir import fingerprints; fingerprints('build/controller-ir/sdk/<member>.c.o.ll', ['f1','f2'])"`
   prints table lines.  Add `<MEMBER>_OWNED`, the transform with
   `leftovers`, the `PIPELINE` and `DROPPED` entries, and the `archive.py`
   import.
6. Host test: an `#ifdef UNITTEST` block at the end of the file (`make test`
   builds every `src/btctrler/*.c` that has one; see siblings for `test.h`,
   `CHECK`, `TEST_DONE`).  Expected values must come from the IR's
   semantics worked out by hand, or from a published source (e.g. Bluetooth
   Core spec sample data).  Never adjust code to make a test pass; never
   invent expected values.  Cover edge cases (wrap-around, boundaries, empty,
   full).
7. `make -j32` (the firmware must link) and `make test` (must exit 0).
8. `python3 tools/btctrler/crosscheck.py f1 f2 ...` compares calls and struct
   fields between your C and the SDK bodies (needs `make controller-ir`).
   Explain every difference in your review notes.  It cannot see arithmetic
   or raw byte offsets; that is what the tests are for.
9. Commit on your branch (format below).  Do not merge, rebase or push.

## Pitfalls that have caused real bugs

- **Struct field order:** LLVM merges bitfields, so a GEP field index in the
  IR is not the member's position in the debug-info struct.  Map by the IR
  type layout and byte offsets; add `_Static_assert(offsetof(...) == N)` for
  any field you rely on.  (A PDU once got written 4 bytes early because of this.)
- **Argument order and meaning:** confirm at the call sites, not from names.
- **Signed vs unsigned** compares (`icmp slt` vs `ult`), integer widths,
  wrap masks, `nsw` arithmetic.  Translate exactly.
- **Static helpers with private calling conventions** (`fastcc`): translate
  them as `static` functions; `own_all` rewrites the calls.
- **Inlined copies** of your functions in other members stay SDK; that is fine.
- Code in interrupts or radio timing paths: do not add logging, loops or
  allocation that the SDK did not have.

## Commit format

```
btctrler: own <member>; drop the SDK member

<What the code does, two or three lines.>

Review notes:
- Translated: f1, f2, ... ; leftovers: cpu_reset (no caller) ...
- Globals moved: name (section, const?) ...
- SDK quirks kept: ...
- Unsure: <every place where you were not certain what the IR means>
- Findings: <SDK bugs, odd behaviour or security-relevant code worth a
  later look, each with where and how you know; "none" if none>
- Tests: <what is covered, where expected values come from>
- crosscheck: <differences and why>
- make test: <last line>; make -j32: <last line>

Co-Authored-By: <your model> <noreply@anthropic.com or your vendor's>
```

The reviewer reads the "Unsure" list first; an honest list there saves a
hardware debugging session.  Then stop and report the branch name.
