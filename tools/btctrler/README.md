# Controller ownership tools

The first accepted batch and its validation are recorded in
[review-notes.md](review-notes.md). Deferred consolidation, SDK quirks and
application-fix candidates are tracked in
[revisit-bd19.md](../../src/btctrler/revisit-bd19.md).
New ownership and review commits must carry the complete report in the commit
body; agent chat should identify the commit and immediate next action.

The canonical clean-controller integration checkout is `/work/cyclone2-fw-btctrler-vanilla` on
`btctrler/br23-bd19-vanilla`. Workers use separate paths/branches; they never switch
that checkout or run their builds there. The primary `/work/cyclone2-firmware`
checkout belongs to other ongoing work.

Review a completed worker in its existing worktree, commit corrections/report
there, and merge that branch from the canonical integration checkout. After
combined validation, remove the integrated worker/review worktree and branch.
Keep pending work and dirty changes until reviewed or explicitly preserved;
there is no permanent branch or checkout per review. See the ownership/review
skills for status, ancestry and nested dependency checks.

BR23 compiles the existing C controller directly. BD19 migrates whole SDK
members to reviewed C through `bd19.json`; unaccepted members stay in a
retained SDK archive. SDK and toolchain dependencies are read-only inputs under `external/`; see
[vanilla-migration.md](vanilla-migration.md) for setup and baseline provenance.

```
PATH=/work/bin:$PATH make controller-ir controller-layouts controller-tools-test
PATH=/work/bin:$PATH make controller-compare
PATH=/work/bin:$PATH make -j32 controller-objects controller-test
PATH=/work/bin:$PATH make controller-audit
PATH=/work/bin:$PATH make test
python3 tools/btctrler/check_uecc.py  # BD19 curve specialization vs Python integers
python3 tools/btctrler/check_law_codecs.py  # Exhaustive fixed-width codec arithmetic
python3 tools/btctrler/check_cte_algorithm.py --clang /path/to/host/clang  # SDK LLVM differential math checks
```

Original IR and inventories: `build/controller-ir/{br23,bd19}/`.
Files use `OCCURRENCE--MEMBER.ll`; BD19 has TWO `tws_pair.c.o` members.
Inspection pins archive hashes and keys its decode cache by target, decoder,
scanner, and member content. `irc.py FILE.ll FUNCTION...` prints pseudo-C;
read the original IR whenever it differs.

Before assigning more decompilation, read `build/controller-ir/reuse.md` and
the function-level `reuse.json`. The census matches original functions by
member occurrence/name, then unique symbol when a function moved. It renames
SSA values/basic blocks, resolves attribute IDs and omits debug/analysis
metadata and target code-generation attributes. It preserves instruction
order, operand identity, widths, arithmetic flags, volatility, call ABI and
sections, and checks referenced type layouts, transitive global initializers,
and imported declarations. Compiled BR23 exports locate existing C, including
assembly-name aliases. Rebuild the selected board before running the CLI.

`reuse_candidate` means preserve the existing C and review its app changes
and acceptance boundary; no second decompilation is needed. Matching bodies
with changed boundaries need a targeted layout/data/signature audit. Different
IR may still be the same C after loop rotation or inlining. Shared C using
named fields can compile correctly against different chip layouts; hardcoded
offsets/sizes, changed bitfields and hardware operations require review.
The census does not prove whole-program equivalence or automatically change
the ownership manifest. In particular, check local helpers/callee selections
when accepting a whole source file.

Each accepted `bd19.json` entry contains `member`, one-based `occurrence`,
original member `sha256`, and `sources` (repository-relative C paths).
Only those sources compile for BD19. Entries sharing a source form one
ownership group; the source compiles once and its exports must exactly match
the group's combined SDK exports. Duplicate SDK exports in such a group must
have matching checked bodies/dependencies, rather than silently picking one.
The builder drops exactly those member occurrences, reindexes the archive,
and verifies all retained names, payload hashes, and order. `controller-audit`
requires every original external function/global to be supplied by the
corresponding compiled owned source(s)/group, with no additional external symbols.
The audit also preserves SDK llvm.used/llvm.compiler.used retention roots;
compiler-only retention cannot replace linker retention. Symbol coverage is a
boundary check, not semantic equivalence or ABI proof.
The separately compiled `bd19/assert.c` supplies the existing owned assertion
policy, including the recovered HCI controller. It remains a support unit
outside original-member ownership; do not duplicate it in a member source.

Prioritize remaining work using the selected dongle's linked image as well as
the reuse census. Target `nm -S --defined-only build/BOARD/app.elf` can match
original definition names (allowing `.llvm.*` suffixes). Exclude names shared
by several original members, such as `cpu_reset` and `list_empty`, from member
attribution. This is lower-bound triage: inlining, aliases, renamed/private
symbols and unrelated local names prevent exact coverage or runtime-use claims.
Member counts give equal weight to tiny helpers and large radio/LMP members.

The BR23 recovered header is preserved as `sdk_br23.h`; `sdk.h` selects the
chip header. Regenerate BD19 types after inspecting the pinned archive:

```
PATH=/work/bin:$PATH python3 tools/btctrler/sdktypes.py \
  --cc external/toolchains/jieli-linux-toolchains-20250324.1/q32s/bin/clang \
  --target q32s src/btctrler/sdk_bd19.h \
  build/controller-ir/bd19/*.ll
```

The generator validates offsets/sizes with the actual target compiler. It
qualifies repeated enum constants rather than silently discarding conflicting
values; local conflicting struct layouts retain source-qualified names.
Generated layout success does not establish runtime behavior.

App changes since the original BR23 decompilation include AFH fixes,
LL_VERSION_IND handling, role-switch dispatch/recovery, page-timeout fixes,
cooperative scheduler initialization, sniff timing, framing checks, and
`btctrlermod_` timing/QoS hooks. Preserve them on BR23. Audit `git log` and
the affected source before porting each member; record BD19 decisions in its
review notes. The original decompilation's object history and fingerprints
are on `btctrler-decompiled`, not the implementation branch's build rules.
