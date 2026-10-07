# Review scripts for owned btctrler members

Run from the repository root after `make -j32 && make controller-ir`.
They compare the owned C (compiled object IR in `build/src/btctrler/*.o`)
with the pristine SDK bodies in `build/controller-ir/sdk/*.ll`.

| Script | Use |
|---|---|
| `volseq.py MEMBER build/src/btctrler/FILE.o [FUNC...]` | per function: ordered volatile accesses (MMIO address or struct field path) and calls, SDK vs owned. Differences from not-yet-inlined helpers and assert paths are expected. |
| `mmio.py SDK.ll OBJ.o FUNC...` | ordered MMIO register accesses and calls only. |
| `callcmp.py MEMBER...` | call multisets and MMIO sequences per function (worktree-relative build). |
| `wideaudit.py` | every owned function: under-aligned i16/i32/i64 loads/stores in the SDK body the owned object lacks (the one-byte-copy trap). |
| `globals.py SDK_IR_DIR MEMBER...` | each moved global: section, const-ness vs the owned object. |
| `leftovers.py MEMBER...` | SDK definitions not owned: any still referenced by retained IR or src/? |
| `resolve.py FILE...` | resolve cherry-pick conflicts in the PIPELINE/DROPPED/OWNED tables by keeping both sides (check DROPPED still closes). |
| `btnwatch.py SECONDS` | PC test: decode Switch Pro 0x30 button bytes and rate from the hostbt log. |
