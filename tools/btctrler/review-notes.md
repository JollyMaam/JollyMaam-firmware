# Imported BD19 recovery history

The historical notes below originate from `btctrler/bd19-support` through
`7cae22b`. Their application-image validation and old source paths describe
that branch. This branch starts at `44d0073`; current migration evidence and
commands are in [vanilla-migration.md](vanilla-migration.md). Original per-object
commit bodies remain reachable through the preserved origin branch.

# Initial BD19 ownership batch

Integration base: main 2b5050f. Work is isolated on btctrler/bd19-support;
the primary checkout and hardware were not changed. Two gpt-6-luna workers
at medium effort supplied object-specific checkpoints; the integration review
checked them against both pinned SDK archives.

## Accepted boundaries

The exact member occurrences, hashes and source assignments are in bd19.json.
This batch accepts seven complete LLVM members:

| Member | External functions | External globals |
| --- | ---: | ---: |
| bredr_utils.c.o | 4 | 0 |
| btcontroller_heap.c.o | 11 | 3 |
| nv_memory.c.o | 4 | 0 |
| sys_timer.c.o | 8 | 0 |
| lmp_utils.c.o | 10 | 0 |
| hci_ll_cmds.c.o | 7 | 1 |
| hci_ll_5_cmds.c.o | 1 | 1 |

All 45 exported functions and 5 globals match compiled ownership inventories.
The retained archive has 104 of the original 111 members; both distinct
tws_pair.c.o occurrences remain intact. The original symbol index has 2140
entries and the retained index has 2090. Every remaining symbol resolves to
the same member payload as before.

BR23 keeps its current implementation and placement. BD19 clock/parity helpers
use their original Classic code sections. Complete BD19 heap and LMP utility
exports include entry points that the BR23 recovery omitted as unused.

## Review findings

- The initial heap translation reread config_asser after allocation callbacks.
  The SDK retains its entry-time value. The accepted code snapshots it, and
  a host test changes the policy inside the allocation stub to exercise this.
- BD19 heap pool initialization/release order, mode-bit handling, alignment,
  null-free guards, diagnostic/reset paths and type-0 allocation's infinite
  failure loop are preserved. The nonreturning loop is reviewed in IR rather
  than executed by the host checks.
- BD19 enhanced DTM helpers have recovered struct-pointer declarations while
  the HCI command member calls them with raw command bytes. Source-local
  assembly aliases preserve the actual caller ABI. A focused check verifies
  the original pointer and length reach both handlers.
- Existing weak key hooks have inconsistent caller/callee signatures in the
  SDK: the HCI member calls the P-256 hook without its declared argument and
  passes command size as the DH hook's second argument. Existing raw-ABI
  aliases retain the SDK call shapes. The weak hooks are currently no-ops;
  implementing those hooks later requires reviewing these boundaries.
- Both SDKs' HCI command initializer and registration match. Compiling the
  original and owned BD19 member to native objects produced identical
  208-byte .ble_hci_data contents. The registration initializer also matches
  textually, including callback targets. SDK undef padding becomes zeroed
  explicit padding in recovered C. ext_le_param's initializer/section/alignment
  match the original IR exactly.
- The BLE 5 handler has different optimized loop shapes between SDK targets.
  Review checked zero-count advertising enable/disable, duplicate/unknown
  handle validation, operation ordering and status carry, scan allocation
  and size copies, create-connection PHY counting, and partial default-PHY
  mutations. The common C preserves these semantics. Changed CFG shape is
  not itself evidence of a behavioral difference.
- Unchecked HCI size copies, the vendor debug-copy overflow, shared string
  buffers, wrapped 16-bit byte-store positions, overlap-sensitive flips and
  SDK clock arithmetic quirks remain as the SDK implements them.

## Validation

Reproducible commands are in README.md. For this batch:

- Both target compiler layout checks and seven tooling regression tests pass.
- `make -j32 all` builds every product, including all four Jieli targets.
  `make -j32 test` exits successfully
  with 551 successful test-program reports across the product variants.
- Every accepted source was compiled and its focused host checks run with
  both BR23 and BD19 headers. The BD19 board test target now selects only
  accepted controller tests and defines CONFIG_CPU_BD19 explicitly.
- At fixed BUILD_ID=2b5050f, all three BR23 app.bin images are byte-identical
  to the integration starting point. The BD19 app.bin is 197048 bytes versus
  197228 before migration, reflecting recompilation of live SDK code. Size
  reduction is not a performance or runtime-correctness claim.
- No flashing, Bluetooth connection, console wake or runtime hardware test.

## Remaining work

This is an incremental controller migration, not complete BD19 ownership.
The other 104 archive members, including native version metadata and the
chip-specific RF/baseband implementation, remain SDK code. Prioritize next
assignments using the actual dongle map/dependency closure. Complex Classic,
radio, low-power and TWS code needs target-specific layout/MMIO/timing review.

Existing BR23 app fixes remain authoritative on BR23. Audit each member's
source history before sharing it and record any deliberate application of
an app fix to BD19 separately from faithful SDK translation. Consolidate the
temporary BD19 assertion boundary when the HCI controller member is accepted.

## Reuse-first census and second batch

The whole-archive census matches 942 definitions after cosmetic naming and
target code-generation differences are removed, with matching checked
signature/type/data/declaration boundaries. Of these, 487 public functions
are already supplied by compiled BR23 C. Another 329 definitions have matching
instructions with boundaries requiring review. The remaining groups are 1000
different instruction bodies and 356 unmatched/ambiguous definitions; neither
group implies that a fresh decompilation is needed. Different optimization,
field indices, or member organization can account for differences.

The census is triage rather than equivalence proof. It does not compare callee
implementations recursively or accept code automatically. It groups repeated
type/data/interface differences so a boundary review can be reused for many
callers. Worker/reviewer skills now prioritize existing C and changed regions.

The enhanced DTM caller's production chip conditional is removed: both targets
use the same raw-byte assembly alias. The generated headers retain their SDK
callee declarations. Both selected-header HCI tests and all four Jieli builds
pass; the three BR23 images remain byte-identical at fixed BUILD_ID=2b5050f.

Five complete matching members were then accepted with NO C source changes:
ll_keys.c.o, ll_pc.c.o, ll_bis_master.c.o, ll_data_extension.c.o and
ll_wifi_detect.c.o. Their source histories have no later app changes. Both
targets' existing focused checks pass; compiled external definitions match
each member. Private data-length/Wi-Fi tables and state initializers were
compared programmatically with SDK IR, including sections and alignment.
These add 19 public functions and no public globals. The manifest now accepts
12 members (64 external functions and 5 globals), leaving 99 SDK members.
The retained symbol index has 2071 entries, with its original member mappings
verified. Thirteen tooling regression tests pass. No hardware validation.

## Shared LE, crypto and radio batch: 37 accepted members

The manifest now replaces 37 original members with 303 unique public functions
and 33 globals, leaving 74 SDK members and 1801 retained symbol-index entries.
The two misc hardware members share one C implementation: their duplicate
exports have matching normalized bodies and checked dependencies/declarations.
Ownership groups compile shared sources once and audit their combined exports.

Most LE members reuse existing C against the selected chip header. Named fields
account for changed ble_ext_rx and controller layouts. Periodic advertising
needs BD19's 16-bit timeout multiplication, including wrap (6554 becomes 4 ms),
and its original assertion/callout ordering. BR23 behavior remains unchanged.
The existing BR23 LL_VERSION_IND application fix remains BR23-only; BD19 uses
the original SDK no-procedure-active gate. Focused tests cover both gates.

AES, lbuf and LL slave missing public APIs are now supplied, including dormant
diagnostics. AES diagnostic vector orientation on hardware is not established;
SDK constants, comparison quirks and terminal loops are preserved. TWS BLE's
original unresolved monitor imports remain unresolved in dormant discarded
paths; this does not establish support for the optional monitor feature.

AGC selects BD19 tables in shared C. RF has a separate BD19 implementation
because its register operations, save buffers and layout differ substantially.
Review preserved cached assertion policy and pointer reload boundaries. The
q32s compiler's RF bitfield bytes were checked against SDK masks. The deep-sleep
registration retains its SDK llvm.used requirement. The compiled-symbol audit
now also rejects weakened linker/compiler retention roots.

Validation: all product builds completed at BUILD_ID=2b5050f; all three BR23
app.bin images remain byte-identical to the frozen starting baseline. The BD19
image is 196908 bytes (starting SDK image 197228). The complete host suite
produced 580 successful program reports with no reported failures. Both target
layout checks, all 16 tooling regression tests and all 37 member/group audits
pass. No hardware validation was performed.

## BD19 interrupt status helper

interrupt.c.o is now owned (38 accepted members, 73 retained). Its only public
function irq_get_status reads one volatile 32-bit word at
0x1be100 + 4 * (index >> 3), tests bit ((index << 2) & 28), and returns 0/1
as an unsigned byte. The recovered header says bool, so the implementation
uses an assembly-name alias to preserve the SDK i8 return ABI. BR23's SDK
uses the same selection at 0x10f100; the BR23 source set is unchanged.
The q32s compiled IR was checked for the exact address, shifts, mask, volatile
load and zero-extended i8 result; the dongle build and all 38 ownership audits
pass. No hardware validation.

## DMA pool/queue member

hw_dma.c.o supplies 20 public functions and no public globals. Twelve missing
BD19 entry points now cover debug/remain/reallocation/peek and LE RX wrappers.
The existing eight functions reuse common C; the three different SDK bodies
are assertion lowering and queue CFG/loop differences. Pool/queue types and
imported declaration boundaries match. No app changes exist in this source's
post-recovery history. The private pool state starts zero, as in the SDK.

BD19 allocation reloads its selected pool after a returning assertion/reset
hook, matching SDK IR; the existing BR23 path is preserved. Peek masks IRQs
even when empty and does not unlink. LE allocation zeroes exactly the requested
16-bit size only on success; it bypasses the general DMA assertion path.
Focused checks cover those distinctions, including a returning failure hook
that replaces the pool. Both target builds and focused tests pass; the BR23
pad image remains identical to baseline. All 39 ownership audits pass.
No hardware validation.

## BIS and remaining service boundaries

ll_bis.c.o now supplies all 17 public functions. Existing bookkeeping/data
path functions are shared; six previously omitted lifecycle/test/address
helpers are restored. Review corrected the access-address helper's bit-23
contribution and preserved the SDK close routine's surprising traversal of
the BIG list with BIS casts. Tests cover both bit contributions and the
original close side effects. RX test payload selection is one byte at the
test-union offset, as in the IR. Original code sections are preserved.
The all-product build and full 582-report host suite pass at this stage,
and all three BR23 app images remain identical to baseline.

The next H4 migration must group its source with hci_controller.c.o or split
the translation unit: hci_transport.c supplies exports from both originals,
and currently omits dormant SDK send APIs. Device-manager command sources
also overlap lmp_hci_cmd_parser.c.o and contain app framing checks and a
remote-version diagnostic no-op. Those members remain SDK-owned on BD19;
matching original cross-chip IR alone does not establish equivalence to
current app-modified C. ll_common_iso is partial, and the BIS/CIS slave and
codec members need additional source recovery before whole-member acceptance.

## Checkpoint: 42 accepted BD19 members

The linked-list and standalone event-mask members add 13 public functions
without changing BR23 production code. List duplicate/iterator/removal cases
are covered by durable tests. The event-mask copy uses an alias-safe unaligned
64-bit load before a two-byte-aligned 64-bit store, matching SDK IR even when
the input overlaps hci_param. Focused tests cover overlap and byte boundaries.

The manifest now accepts 42 members, supplying 354 unique public functions
and 33 globals; 69 original SDK members remain. The retained symbol index has
1750 entries, checked against exact original member occurrences. All 42
member/group export and retention audits pass. Both target layout checks and
16 tooling regressions pass. Every product builds; the complete host suite
reports 584 successful test programs. At fixed BUILD_ID=2b5050f all three BR23
app.bin files remain byte-identical to baseline. The BD19 image is 196840
bytes; neither image size nor host checks establish radio/runtime correctness.
No hardware validation was performed.

## Extended scan ownership

ll_ext_scan.c.o reuses the existing complete source (seven public functions,
one callback-table global). The changed ble_ext_rx fields compile through
named accesses against the BD19 header; no second parser/report implementation
is needed. The handler initializer, .ble_ll_const section and alignment match
the original IR. Used packet/report layouts pass target compiler assertions.

BD19 now preserves the helper's original i8 return ABI through a symbol alias
instead of the recovered bool/i1 definition. It also retains the pre-call RX
status bit across radio header callbacks, as the original IR does. New cases
mutate that status in both directions during a callback. BR23 production
behavior is unchanged and the pad image remains identical to baseline.

The existing host capture array had room for eight radio resolutions although
the periodic-sync cases perform more; enlarging it and checking bounds removes
a latent test-harness overflow. Focused tests pass with both chip headers and
under AddressSanitizer (leak checking disabled for the existing allocation-based
harness). Both target builds and all 43 member ownership audits pass.
No hardware validation.

## LE message pools, utilities and HCI wrappers: 46 accepted members

ll_msg.c.o has thirteen public functions and no owned globals or special
sections. New BD19 C covers both 512-byte queue pools, allocation, dispatch,
and ISO adapters. It preserves returning assertion/reset hooks, fresh queue
loads after those hooks and callbacks, one cached debug flag per drain, fresh
state reads after logging, u16 message-ID truncation to u8, and the unchecked
state dispatch except for special value 255. Allocation widens u16 before
adding the four-byte prefix (65535 becomes 65539). ISO send reads rets even
when logging is disabled, then logs/pushes/resumes in SDK order. Peer review,
focused tests and AddressSanitizer checks pass. BR23 does not compile this file.

btcontroller_util.c.o now supplies 44 public functions plus the public UUID
base. Existing helpers are shared, with BD19-scoped additions for omitted
endian/UUID/parser/CRC32 APIs and SDK diagnostic no-ops. Both string buffers,
CRC8/CRC32 tables and UUID bytes/alignments match compiled SDK IR exactly.
Parsing commits its temporary address only on success and permits the SDK's
three separators. Review caught an inherited store16 recovery mismatch: the
SDK increments the position as u16, while existing BR23 C uses promoted int.
BD19 now wraps position 65535 to zero, with a boundary test; BR23 retains its
existing behavior. New copies use the original memory intrinsics.

hci_ll.c.o reuses the existing three functions and registration, adding the
48 omitted wrappers, g_le_name and private state for BD19. All 51 functions
use .ble_hci_code; registration retention and data sections/alignments match.
Every initializer was compared programmatically, allowing only the generated
name change for the identical one-byte configuration type. Review corrected
advertising-data packing to byte offset 1, made event-mask copying alias-safe
with align1 load/align4 store (including overlapping input), and restored the
address-type reload after MAC callbacks. Packed six-byte connection reads
compile to i48 loads without over-reading; odd-offset stores retain their
SDK alignments. Tests cover raw expected words/bytes and callback mutation.
SDK unsafe length copies, shared buffers and uninitialized short scan-response
bytes remain SDK behavior; no new input validation was applied in this port.

At this checkpoint 46 members supply 469 unique public functions and 37 globals;
65 SDK members remain, with 1631 retained index entries verified against exact
original members. All compiled export/retention audits, both target layout
checks and 16 tooling regression tests pass. All products build and the full
host suite reports 588 successful test programs. Focused AddressSanitizer
checks pass for extended scan, message queues, utilities and HCI wrappers.
At BUILD_ID=2b5050f all three BR23 app.bin images remain byte-identical to
baseline. The BD19 image is 196868 bytes. No hardware validation was performed.

## Classic power, link timing and extended advertising: 50 members

BD19 bredr_low_power.c.o has a separate implementation because the register
handshake, half-slot counter restoration and callback table differ from BR23.
The table is six pointers (24 bytes), .classic_rf_const, align4; its complete
initializer matches SDK IR. All public functions and five callbacks use
.classic_rf_code. The model checks the exact volatile 32-bit read/write order,
strict control/busy/ready poll conditions, saved phase before the fine-counter
call, low/high clock reads before phase classification, wrapping addition,
and division/remainder by 1250. It preserves returning assertion/reset paths,
the final control-register read and the SDK wake-up character after timer IRQ
processing. Release clears the handle after the callback. Peer review caught
the initially omitted public hold-bit operations; corrected i8 RMWs and a
0xa4 -> 0xa5 -> 0xa4 test verify sleep requests are blocked then allowed.

link_task.c.o reuses all 31 functions with BD19 idle timing: idle_reset's
strict threshold is four, its lead is four and callback period is four; idle
resume uses left-4 or period-10 with period four. BR23 retains its eight-slot
lead and period-14 path. The strict left>12 gate and left+32 run budget stay
shared. A BD19 alias preserves the SDK idle-detection i8 return rather than
the recovered bool/i1 definition; BR23 keeps its existing ABI. The callback
pair in idle_task_ops, its alignment and private initializers match SDK data
(the compiled callback-pair type is structurally identical to the SDK named
type). Added boundary cases exhausted the old eight-entry fake timer capture;
the harness now has checked capacity for its monotonically issued IDs.

ll_ext_adv.c.o now supplies all 16 functions and its handler global. The two
omitted exports are the always-zero periodic-enabled stub and ACAD message
registration at dispatch slot 8, whose handler only logs the three SDK cases.
BD19 caches primary HW across the first two radio callbacks and reloads it
before start. Both original SDKs cache it, but current BR23 C reloads; that
existing BR23 path is preserved. Mutation tests check both paths. BD19 uses
the SDK memory intrinsics. Handler initializer/section/alignment match, and
channel/CRC constants have identical bytes and alignment; their private vs
internal LLVM linkage is module-local in both representations.

tws_le_acc.c.o is accepted with no C changes. The three functions preserve
32-bit LFSR arithmetic, all scoring masks/transition windows, the unusual
unnormalized byte comparisons, four separate address loads and mandatory
first generator step. Existing vector tests pass with both chip headers.
No globals or special sections are introduced.

All 50 member export/retention audits pass, supplying 525 unique public
functions and 39 globals. The retained archive has 61 original members and
1573 verified symbol-index entries. Both target layouts and all 16 tool tests
pass. All products build, and the full host suite has 592 successful reports.
Focused AddressSanitizer checks pass for Classic low power, link tasks and
extended advertising (leak checks disabled for the existing advertising
allocation harness). With BUILD_ID=2b5050f the three BR23 app.bin images remain
byte-identical to baseline; BD19 is 196900 bytes. No hardware validation.

Future FCC acceptance needs a separate parameter-table review: BD19's
packet_table has six entries, whereas existing BR23 C has nine. It remains
SDK-owned on BD19.

## Linked-code priority: link layer, buffer pools and slot timer

A name-based inspection of the Nova 2 linked image was used to prioritize this
batch. Ambiguous names shared by multiple original members (cpu_reset,
list_empty, and similar helpers) were excluded. This identifies remaining
linked code, not exact byte ownership or runtime execution. Large LMP, radio,
analog and baseband members remain; small-member counts alone overstate
progress. The original cross-chip census still supports reuse of matching
logic, with layout/callee/app boundaries reviewed separately.

link_layer.c.o reuses the existing 69 functions and five globals. BD19 omits
the BR23 application-only ll_version_exchange_pending helper, keeping the
SDK ll_current_no_step response policy. Init/destroy power-off callbacks
present in original BR23 IR are absent in original BD19 IR and are selected
accordingly. The btctrlermod_air_rx anchor timestamp hook stays BR23-only;
BD19 anchor scheduling needs a separate deliberate app integration. BD19
memory operations use the original compiler intrinsics. All five public
initializers, sections and alignments match SDK IR exactly.

link_bulk.c.o reuses pool/queue logic and restores eight omitted APIs. BD19
RX headers are 24 bytes, TX headers 28; named offsets now distinguish them
through allocation, reserve prefixes, zeroing and free/accounting paths.
Common linkage/null initialization of all six public pool globals matches
SDK IR. Private link records have BD19's explicit align4, while BR23 retains
its unannotated declaration (its target compiler naturally chooses align4).
Heap receive pools, remaining-pool allocation, references, suspend waiting
and diagnostic failures retain the SDK paths. Used-size truncation, signed
share comparisons and wrapping BD19 size arithmetic were reviewed. BR23
keeps its prior invalid-ID guards and signed remaining-size expression.
A BD19-only builtin memset selection resolves the final peer-review finding.

bredr_slot_timer.c.o needs a separate BD19 hardware implementation: sixteen
half-slot timers, separate pending/enable/clear registers and per-timer fine
compares replace BR23's eight-timer combined control register. The sixteen
public functions, no public globals, sections and IRQ interrupt attribute
match original IR. Register ordering, direct untagged callbacks, exact
130-byte state clear, nested get/put IRQ handling, unconditional rets reads,
signed division/remainder, single slot wrap and u16 fine storage are checked
by the target compiler and a host register model. The SDK remain(16) and
65535-wrap quirks are preserved. Independent review found no blockers.

Validation: all products build; the complete host suite has 595 successful
reports. Both target layout checks, sixteen tool regressions, and all 53
export/retention audits pass. Focused AddressSanitizer checks pass for the
new timer, link layer and buffer management (leak checking disabled for the
existing allocation-based harnesses). At BUILD_ID=2b5050f the three BR23
app.bin images remain byte-identical to baseline. BD19 app.bin is 196560
bytes. The manifest supplies 648 unique public functions and 50 globals,
leaving 58 SDK members with 1439 verified symbol-index entries. No hardware
validation was performed.

## BD19 host ACL and LMP transmit: 54 accepted members

`lmp_rxtx.c.o` occurrence 1 (`81c88e4e...37c9922`) is supplied by
`lmp_acl_tx.c` and `lmp_control_tx.c`. The compiled pair defines exactly the
member's ten public functions and no public globals. Both sources remain clean
two-chip files: BR23 keeps its existing application null/error guards and
sign-extended capacity comparison, while the BD19 branch reproduces the SDK.

BD19's receive bulk has `payload_head` at 20 and `data` at 24; both chips place
the transmit `payload_head` at 24 and `data` at 28. The private receive
operations retain their IRQ-disabled 16-bit bitfield read/modify/write order.
The private transmit capacity check zero-extends the 16-bit length. BD19's
standard ACL path has no allocation-null guard and uses the SDK memory
intrinsic. LMP request/response allocate before `va_start`, preserve the
module gate and response/role flag ordering, and leave argument validity to
the SDK. These are faithful SDK choices, not application-fix ports.

Focused host checks add the 40000-byte private transmit boundary that
distinguishes zero- from sign-extension. Allocation-failure checks remain on
BR23, where the application safely returns; the BD19 failure path is reviewed
from IR instead of executed because the SDK continues into the failed
allocation. Both target compilers build both sources.

Both target layout checks, all 16 tooling regressions, all four product builds,
and all 54 member/group ownership audits pass. The complete host suite exits
successfully with 596 `: ok` program reports plus the legacy `tests passed`
reports. The BD19 app image is 196560 bytes. The retained archive has 57
original members and 1429 verified symbol-index entries. The manifest supplies
658 unique public functions and 50 globals.

The initial forced rebuilds of `2b5050f` and `a1021a5` produced matching
BR23 images that differed from the frozen artifacts (`d98408e7...`,
`1a87f4b0...`, `b1572c92...`). A subsequent ordinary fixed-ID build during
parent review reproduced all three original frozen images exactly
(`12a01fd2...`, `aaf7656d...`, `6aabf009...`). Both changed BR23 modules also
have identical canonical production IR and globals to the fresh a1021a5
reference for all three boards. The earlier forced-rebuild discrepancy's
cause is not isolated; the frozen artifacts were retained unchanged. No
hardware validation was performed.

Parent review corrected BD19 varargs cleanup order: allocation precedes
va_start, formatting precedes va_end, and transaction-bit updates and queue
handoff occur after va_end, as in the SDK. Compiled q32s IR verifies that
sequence through the private allocation/handoff helpers. The four Jieli
builds, focused tests with both headers, full host suite and all 54 ownership
and retention audits pass after the correction. The change stays within the
BD19 branch; the BR23 production paths remain unchanged.

## BD19 Classic HCI ownership: 55 accepted members

`lmp_hci.c.o` occurrence 1 (`e677dc6d...c52d9f3`) is supplied by
`lmp_hci_control.c`, `lmp_hci_pairing.c`, `lmp_hci_procedures.c` and
`lmp_hci_settings.c`. The combined object defines exactly its 60 public
functions and `gbredr_local_dev`, with no extra external symbols. Every public
function is in `.classic_lmp_code`; the global is in `.classic_lmp_data`,
align 4, with byte-for-byte identical initializer data to the SDK.

Existing reviewed C supplies the matching bodies. BD19 selects its SDK
behaviors rather than the BR23 application fixes:

- The private-version CRC writes both bytes of the 16-bit CRC. BR23 retains
  its existing low-byte behavior.
- Local names use the SDK `strlen`-then-assert copy, and supported-feature
  indexes are unchecked. BR23 retains its bounded name and feature handling.
- PIN replies read four unaligned 32-bit words even for short PINs. User
  passkey replies preserve the SDK's unusual variadic counts.
- Switch Role sets `core.role_switch` directly. The BR23 queued
  controller-task dispatch remains unchanged.
- Remote-version and encryption-refresh requests call their LMP senders with
  the SDK's unchecked connection pointer.

The BD_ADDR and AFH final-word reads, local-device defaults and all remaining
control/setting ABIs match the IR. Focused tests exercise the CRC byte,
unaligned key/PIN reads, keypress call shapes, role flag, null-sender calls
and bounded BR23 paths with both headers.

## BD19 controller task ownership: 56 accepted members

`btctrler_task.c.o` occurrence 1 (`52f6d471...97a4247`) is supplied by
`task_dispatch.c`, `task_lifecycle.c` and `task_queue.c`. The combined object
defines exactly its 17 public functions and `bt_task_thread`, `h4_config`
and `h4_transport`; all initializers, sections and alignments match. Public
functions use `.hci_controller_code`; the callback table uses
`.hci_controller_data`; transport/config pointers use `.hci_controller_bss`.

The SDK marks the task entry, task callbacks, queue-full counter and testbox
callback state internal. Because the owned member is split across source
files, BD19 keeps the production task entry with `task_create` in
`task_lifecycle.c` so it remains internal without exporting an extra symbol.
BR23 retains its pre-existing source split and linkage.

BD19 also retains the SDK's failure quirks: task-ready ignores the semaphore
creation result, task-exit ignores creation and post results, always waits,
frees and kills the task, and message builders continue after the SDK count
assertions. `bredr_link_event` has no four-word bound, negative counts are
passed through, and target-specific `cpu_irq_disabled` remains a declaration
boundary. BR23 keeps its safe count validation and failure returns.

Focused tests cover queue-full retry/hook ordering, IRQ and masked-IRQ stops,
self-thread link-event policy, callback tables, suspend/resume draining,
SDK-only count continuation, ignored lifecycle results and both target
headers. All target compilers and the full BR23 controller test target pass.

At this checkpoint 56 members supply 735 unique public functions and 54
globals. The retained archive has 55 original members and 1348 verified
symbol-index entries. All four products build, all 56 export/retention audits,
both target layouts and all 16 tooling regressions pass. The complete host
suite exits successfully with 600 `: ok` reports and 36 legacy `tests passed`
reports. BD19 `app.bin` is 196600 bytes.

The frozen BR23 artifact comparison remains unreproduced in this environment:
both forced clean and repeated ordinary fixed-ID builds of the clean baseline
commits produce `d98408e7...`, `1a87f4b0...` and `b1572c92...`, while the
stored files remain `12a01fd2...`, `aaf7656d...` and `6aabf009...`.
No hardware validation was performed.

## BD19 LE low-power and multilink scheduling: 58 accepted members

`le_low_power.c.o` (`c8b3ba26...`) supplies 11 public functions and
`ble_power_ops`. BD19 now has the six-field operation table, a no-op
`ble_power_on`, the private resume fine-count state, and the missing
`ble_enter_deepsleep`/`ble_exit_deepsleep` register save/restore paths.
All functions use `.ble_rf_code`; the operation table retains
`.ble_rf_const`, align 4. BR23 keeps its nine-field table and powered callback.
Tests cover the saved 92-byte SFR prefix, anchor capture/restore ordering,
deep-sleep fine-count restoration, and the six-field table.

`multilink_schedule.c.o` (`948409c2...`) reuses the existing scheduler
implementation: 19 public functions and `ble_slave_multlp_target`. The only
target change is BD19's three-field `lp_target` initializer with a null level
handler and the same idle callback; BR23 keeps its two-field initializer.
Existing tests cover signed/unsigned slot wrap, repeated hardware reads,
anchor/fine ordering, callback mutation, timer arithmetic and the SDK's
sentinel overread.

The combined manifest now accepts 58 members, supplies 765 unique public
functions and 56 globals, and leaves 53 retained members with 1316 verified
index entries. All four products build, both layouts and all 16 tool tests
pass, all 58 ownership audits pass, and the host suite exits successfully
with 602 `: ok` reports plus 36 legacy `tests passed` reports. BD19 `app.bin`
is 196620 bytes. No hardware validation was performed.

## BD19 device manager and cooperative threads: 60 accepted members

`device_manager.c.o` (`b19e444b...`) supplies 14 handlers and `hci_param`.
BR23's equivalent four command handlers remain owned by `hci_misc.c`; the
device-manager copies are compiled only for BD19, avoiding duplicate symbols.
The BD19 handlers preserve LINK_CONTROL disconnect/remote-version dispatch,
empty link-policy/status handlers, and testing-command acknowledgement rules.
All public functions are in `.hci_controller_code`.

`thread.c.o` (`66446ead...`) supplies 11 functions. Added the five dormant
message API stubs, retained controller-code sections, and preserved BR23's
minimum-timeout scheduler fix. BD19 keeps the SDK's undefined empty-list
timeout accumulator and leaves timed scheduling checks to non-empty cases.

The manifest supplies 790 unique public functions and 57 globals, with 51
retained members and 1290 verified index entries. All products, 60 ownership
audits, layouts and 16 tool tests pass. The host suite exits successfully with
604 `: ok` reports and 36 legacy `tests passed` reports. BD19 `app.bin` is
196676 bytes. No hardware validation was performed.
## Follow-up review of HCI and controller task ownership

Reviewed the two accepted members at `3d1281f` against their original BD19
IR in an isolated worktree. Corrected these BD19 details:

- Role-switch support accepts an `i8 zeroext` byte through an assembly-name
  alias; the shared SDK header's C `bool` would narrow the public definition.
- The HCI task argument-count assertion uses an unsigned comparison, including
  negative counts; it still continues if the assertion hook returns.
- Ready/exit reload the escaped semaphore message word after queue and pend
  callbacks. Exit snapshots module selection before allocation and callbacks.
- Connection-message construction ends its local variadic-list copy before
  posting, preserving the SDK's cleanup order.
- The private-version and local-name setters retain their conditional SDK
  logs. Under-aligned CRC and key/PIN accesses use alias-safe types.

Focused host cases cover nonzero role-switch bytes through 255, negative
message counts, callback mutation of the semaphore word at both boundaries,
module-selection mutation and both restored logging paths. Emitted BD19 IR
confirms the byte ABI, unsigned guard, module snapshot and pointer reloads.
The local-device initializer and types, transport/config zero initializers,
and callback table initializer, sections and alignments match the SDK.

All four Jieli products build at `BUILD_ID=2b5050f`; all 56 ownership audits,
both layouts and 16 controller tooling regressions pass. The complete host
suite passes with 600 `: ok` and 36 legacy `tests passed` reports. All edited
BR23 production modules have identical canonical functions and globals to
the review baseline. In this review run, all three BR23 `app.bin` files also
match the original frozen baselines byte for byte. This successful run does
not establish the cause of the earlier artifact discrepancy.

BD19 `app.bin` is 196604 bytes (previous accepted checkpoint: 196600), SHA-256
`db72507d8c22cae31c946524f7485aaadda279120dc95738b832c09427069d92`.
Ownership counts remain 56 members, 735 functions and 54 globals, with 55
retained SDK members and 1348 verified index entries. No hardware testing.
Validation logs are `/work/analysis/btctrler-bd19/review56-{builds,checks,tests}.log`.


## Follow-up review of LE low-power and multilink ownership

Reviewed the two accepted members at `c58c76e` against their occurrence-1
BD19 IR and the existing BR23 implementation. Multilink requires no further
source changes: its three-field retained low-power target, manager and lead-ID
initializers, sections and alignments match. Reviewed shared scheduler
arithmetic, callback reloads, hardware reads and the changed BLE field layouts.
The recovered atomic type has a different C name but the same checked shape.

Corrected four BD19 low-power behaviors:

- Deep-sleep exit restores the fourteen ordinary anchors first, then writes
  the coupled anchor 2/9 pair once per hardware instance. The earlier port
  wrote the pair twice inside the loop. Preserve the ordinary hardware-ID
  mask and the pair helper's byte-width hardware ID.
- Suspend probe sets bit 1 of `0x1cfc70` before updating deep-sleep control.
- Resume sleep/event bookkeeping uses `bt_get_fine_cnt(usec)` plus the saved
  power-off fine count, including unsigned wrap, rather than the input usec.
- Resume calls `bt_run_clk_sel(1)` after the final control write when
  `lp_bt_sniff_use_btosc` is enabled.

Host tests now trace every anchor data/control access and synchronization
point for two hardware instances, with distinct stored anchor values. They
also check compensation, wrap, event-count advancement with an input time
that differs from the compensated count, the wake bit and conditional clock
selection. Original and emitted BD19 IR were compared for both operation
tables, private-state initializers, relevant type shapes and register order.

This review branch also includes the preceding HCI/task review corrections
(original commit `8888353`, reapplied as `784c4de`), which had not yet reached
the integration branch. Combined validation builds all four Jieli products
with `BUILD_ID=2b5050f`. All 58 accepted members pass the 57 ownership-group
audits; both layouts and all 16 tooling tests pass. The complete host suite
passes with 602 `: ok` and 36 legacy `tests passed` reports. The two reviewed
BR23 modules retain identical canonical production functions and globals;
all three BR23 firmware images match the original frozen baselines exactly.

Combined BD19 `app.bin` is 196620 bytes, SHA-256
`cb51629f6cd71490c67805879527a351797e6e5306cdd1eb76efe3bfea126df5`.
The manifest remains at 58 members, 765 public functions and 56 globals,
with 53 retained SDK members and 1316 verified symbol-index entries.
Logs: `/work/analysis/btctrler-bd19/review58-builds.log` and
`/work/analysis/btctrler-bd19/review58-final-checks.log`. No hardware testing.


## Follow-up review of device manager and cooperative thread ownership

Reviewed both occurrence-1 members at `cd41996` against the original BD19 IR,
BR23 IR/history and emitted target code. The original `hci_param` initializer
matches exactly, including its section and alignment.

Corrected the BD19 device-manager behavior that still inherited BR23 changes:

- Baseband and information handlers ignore input lengths; extended-feature
  pages use the SDK pointer calculation and success status without rejection.
  BR23 retains its length/null checks and bounded page handling.
- The event-mask copy uses the original unaligned 64-bit load and aligned-2
  store, with alias-safe types. Reset snapshots module configuration before
  the Classic reset callback.
- Read Remote Version starts with status zero, uses only the ACL connection
  lookup, and returns the low byte of its sender result. Disconnect starts
  with status two, falls back to eSCO, and maps nonzero low-byte results to two.
- Restored conditional SDK diagnostic paths, including manufacturer/version
  reporting, address formatting, event-mask dumps and test-command logs.
  Page input is reloaded after the logging callback, as in the IR.

Thread definitions now use the original default text sections rather than
`.hci_controller_code`. The five empty message functions remain public. The
SDK's signed, undefined timeout accumulator affects nonempty timed lists as
well as empty lists; it is not a valid minimum calculation. BD19 host tests
exercise runnable scheduling, mutation/deletion, IRQ behavior and the message
stubs, and avoid executing that undefined timeout path. BR23's defined minimum
and empty-list behavior remain unchanged and tested. No BD19 timeout fix was
introduced; that would be a separate application change.

Focused device tests cover ignored lengths with valid storage, all byte page
values without dereferencing invalid pages, module snapshots, LE/ACL/eSCO
routing combinations, full-width sender-result truncation, consumed testing
commands and logging callback mutation. Both target compilers build the
sources. All four firmware products build at `BUILD_ID=2b5050f`. The full host
suite passes with 604 `: ok` and 36 legacy `tests passed` reports. All 60 members
pass the 59 ownership-group audits; both layouts and all 16 tooling tests pass.
Reviewed BR23 production functions/globals are canonically unchanged, and
all three BR23 `app.bin` files match the original frozen baselines byte for byte.

This isolated branch carries the prior HCI/task and LE low-power review fixes
as `0fc29e3` and `0b5449a`, respectively, in addition to this review. The
integration branch had not yet incorporated those changes. Combined BD19
`app.bin` is 196664 bytes, SHA-256
`fd80defde8aeccb48187b2ef916072844f11bf00b327cb52361685f9ef4c6654`.
Manifest totals remain 60 members, 790 public functions and 57 globals,
with 51 retained SDK members and 1290 verified symbol-index entries.
Logs: `/work/analysis/btctrler-bd19/review60-builds.log` and
`/work/analysis/btctrler-bd19/review60-final-checks.log`. No hardware testing.

## FCC/DUT target recovery decision

The next FCC/DUT assignments require BD19 hardware recovery, not merely
compiling the BR23 bodies against a different header. Original authority:
`build/controller-ir/bd19/01--bredr_fcc.c.o.ll` and
`build/controller-ir/bd19/01--bredr_dut.c.o.ll`. The reported missing BR23
interfaces are not permission blockers; use the following checked BD19
boundaries and recover their surrounding control flow from that IR.

- `radio_set_eninv` is not called by either BD19 member. Do not add a fake
  declaration or no-op shim. FCC analog setup instead has its own volatile
  register sequence, including low-byte clears at `0x1c001c`/`0x1c0108`, frame
  disable and power setup. Recover the complete sequence and its ordering.
- `bredr_freq_table_init(u8 *, u8)` is the correct existing BD19 declaration.
  FCC passes the address of `frame->freq_table[0][0]` (frame offset 252),
  followed by mode 0/1. Preserve this ABI and deliberate qualifier conversion;
  do not change the shared declaration to the BR23 one-argument signature.
- BD19 has no `ctrl_frame.rxfreq`/`txfreq`. Both members program fixed
  frequencies through volatile 32-bit MMIO at `0x1c011c`: clear, OR one
  encoded frequency, then separately read/OR the other encoding shifted left
  seven. Recover the field order separately for each member; FCC uses
  `tx_fre` in the low bits and `rx_fre` shifted seven. Preserve surrounding
  format-bit changes and reads.
  Do not invent frame fields or reuse BR23 register addresses/masks.
- The BD19 DUT member has no `rf` import or `production_test` branch. Recover
  its loopback copy/checker dispatch from `dut_rx_handler`'s inlined helper;
  do not add a field, hardcode a fictitious flag, or retain the BR23 branch.
- Encryption and packet-type paths call the already-declared
  `link_conn_set_encrypt(lc, rx, tx)` and `link_conn_set_ptt(lc, ptt)`.
  Recover these BD19 calls at their actual call sites instead of importing
  `lc_write_encry` or `lc_write_ptt` into the BD19 boundary.
- The receive clock-adjustment path calls `rxoffset_cal(frame, 1)` under its
  BD19 conditions. Recover that branch from IR rather than declaring
  `__rx_adjust_clkoffset` or treating it as a mechanical name alias.

Implementation decision: preserve BR23 behavior and use target-specific C
for the hardware-different bodies. The worker may add
`src/platform/jieli/btctrler/bd19/bredr_fcc.c` and
`src/platform/jieli/btctrler/bd19/bredr_dut.c` if that makes the boundary clearer;
reuse verified pure helpers/tables where useful. Proposed BD19 manifest
entries can select those target sources after whole-member review. No
coordinator header additions are currently required by the listed blockers.
This does not pre-accept either member; the worker must still recover every
export/global, audit all dependencies, compile with the BD19 target compiler,
and provide meaningful tests based on IR. Report genuinely new missing
interfaces with the exact BD19 declaration/call evidence. No hardware testing.

## BD19 FCC and DUT initial acceptance (withdrawn by follow-up review)

`bredr_dut.c.o` (`7a56461f...`) and `bredr_fcc.c.o` (`d8beb506...`) now use
separate BD19 sources under `bd19/`, preserving their two PRBS9 globals and
complete six-function and two-function public boundaries. BD19 uses the
existing two-argument frequency-table initializer, volatile frequency control
at `0x1c011c`, raw FCC low-byte/frame register sequencing, and the approved
`link_conn_set_encrypt`, `link_conn_set_ptt`, and `rxoffset_cal` call paths.
The obsolete BR23 `production_test` branch and frame frequency fields are not
used. The recovered `dut_frame_get_empty_txbuf_index` samples transmit state
under IRQ masking and selects two slots.

The manifest accepts 62 members, supplying 798 unique public functions and 59
globals, leaving 49 retained members with 1280 verified index entries. All
four products, layouts, 16 tool tests, 62 ownership audits and the host suite
pass; the suite reports 606 `: ok` plus 36 legacy `tests passed` reports.
BD19 `app.bin` is 196664 bytes. No hardware validation was performed.


## FCC/DUT semantic review: acceptance withdrawn

Review baseline: `0974935`, source recovery `2384602` (worker reported
`ae778ab`). The two members fail comparison with their original BD19 IR.
Removed ONLY `bredr_fcc.c.o` occurrence 1 and `bredr_dut.c.o` occurrence 1
from `bd19.json`, restoring both original SDK archive members. The draft
`bd19/bredr_fcc.c` and `bd19/bredr_dut.c` remain in the repository for rework;
they are not accepted BD19 production implementations. Prior successful
build/export checks and the preceding acceptance report do not establish
semantic equivalence.

Blocking findings, with line numbers in the rejected sources:

1. FCC analog setup, lines 104-157, retains BR23 control registers, masks and
   sequencing. SDK IR `set_fcc_default_info` reads/writes volatile **i32** at
   `0x1c001c` and `0x1c0108`, preserving their upper 24 bits. Draft lines
   112-113 instead issue **i8** zero stores. The SDK then disables the frame,
   sets power before carrier/frequency setup, and updates `frame->optcntl`.
   The draft retains `BB_REG_C`/`BB_REG_A` operations from BR23 and omits
   `bredr_bd_frame_disable`, `wind_align_set` and `windset` calls entirely.
2. FCC's SDK packet table has SIX bytes (`04 0b 0f 24 2a 2e`), not the nine
   bytes in draft line 49. Its `miscntl` accesses are i16; the draft narrows
   through `u8` at line 104 and destroys upper bits. Original init also uses
   `bredr_frame_set_irq_handler` and `link_conn_set_rx_bulk_in_irq`; neither
   call exists in the compiled draft. Recover the whole public init path.
3. DUT analog setup, lines 197-254, uses BR23 `0x1c00d0` bit 0 and
   `0x1c0000` bits 19/16. SDK `dut_cfg_analog` instead starts with a volatile
   i32 read/clear-bit-3/write at `0x1c0100`, uses that register's bits 3/2
   for carrier mode, and changes `frame->optcntl` bit 0 for scenarios.
   Fixed-frequency selection uses `frame->format` bit 7, not the draft's
   BR23 register bit 16. The SDK performs i16 `miscntl` accesses; draft line
   246 narrows to u8. Recover all scenario branches and teardown ordering.
4. DUT receive dispatch, line 538 onward, is a BR23 slot loop. The SDK calls
   `bredr_frame_get_rx_payload_head(frame, heads)` once, iterates its two
   decoded records until index 0xff, and uses the returned metadata.
   The draft omits that API and directly samples frame registers, calls
   `__write_reg_rxint`, and carries the BR23 rx_full loop. Original
   `soft_agc`, `bredr_link_set_afh` and frame IRQ-handler transitions are
   also absent from the compiled draft. This requires actual BD19 receive
   recovery, not a compatibility shim.
5. The SDK transmit path calls `dut_frame_get_empty_txbuf_index`; the draft
   defines that export but never uses it in `dut_tx_handler` (line 473).
   The helper itself combines two volatile RMWs into one and samples frame
   status AFTER its retry loop. SDK samples txtog/txbuf/link control fields
   INSIDE each attempt, before testing the latch. Both the helper's sequence
   and the internal caller must be recovered from BD19 IR.
6. The comprehensive BR23 fixtures are disabled by `#if 0`. Active BD19
   tests check four channel encodings (FCC) and packet/table samples (DUT).
   They exercise none of the differing hardware, receive or transmit paths.
   Passing those tests cannot validate either whole-member replacement.

The two PRBS9 global initializers, sections and alignments DO match the
original SDK byte for byte; the compiled public symbol boundaries also match.
These are reusable pieces, not justification for accepting the hardware code.
Decoded rejected objects and imported-call differences are preserved under
`/work/analysis/btctrler-bd19/review62/`. Missing direct SDK calls were checked
against raw IR; a call-list comparison alone was not used as equivalence proof.

Rework boundary: recover FCC first as its own checkpoint; validate complete
register/callback traces from raw BD19 IR, all six packet table entries and
16-bit field preservation. DUT can then reuse the verified tables and pure
helpers, but must recover its own analog, decoded receive and synchronized
transmit paths. Use meaningful BD19 fixtures; preserve the original BR23
sources. Do not re-add manifest entries until semantic review passes.

Fallback validation: all four Jieli products build at `BUILD_ID=2b5050f`.
Both layouts, all 16 tooling regressions, all 60 accepted members (59 audit
groups) and the full host suite pass (604 `: ok`, 36 legacy reports). The
retained archive now includes both rejected members again. Totals return to
60 members, 790 public functions, 57 globals, 51 retained members and 1290
verified index entries. All three BR23 frozen images match byte for byte.
BD19 `app.bin` is 196664 bytes and matches the previously reviewed 60-member
image, SHA-256 `fd80defde8aeccb48187b2ef916072844f11bf00b327cb52361685f9ef4c6654`.
Logs are `/work/analysis/btctrler-bd19/review62-sdk-fallback-{builds,checks}.log`.
No hardware testing.


## FCC recovery completed after withdrawal: 61 accepted members

Completed the worker's uncommitted FCC-only rework from
`/work/cyclone2-fw-bd19-fcc-dut` in a separate worktree, based on `97744d8`.
Re-reviewed both public functions and all private data against original BD19
`bredr_fcc.c.o` occurrence 1, SHA-256
`d8beb5063d8182f868aa4eff54ae42c9d163f5015239a5e154afee129754d1e8`.
The accepted source is `src/platform/jieli/btctrler/bd19/bredr_fcc.c`.
DUT remains withdrawn and supplied by its original SDK archive member.

The recovery preserves the six-byte packet table, two address/BCH constants,
all `.classic_rf_code`/`.classic_rf_const` sections and alignments, and exactly
two external function definitions with no public globals. Programmatic
initializer comparisons match all three private constants exactly.

Final review corrections beyond the worker's handoff:

- `miscntl` bit 3 depends on packet-table indices 3..5, not packet group >=3.
  All valid EDR entries have group 2. The i16 upper bits remain preserved.
- The final timing-register RMW is at `0x1c0008`, not `0x1c0048`.
- FCC fixed-frequency control uses `tx_fre` in the low bits and `rx_fre`
  shifted left seven. Both are read after window callbacks and separately
  ORed through volatile i32 MMIO. Unequal callback-updated fields are tested.
- Link setup reloads `bredr_link_v.fcc_lc` after TX allocation, before each
  RX-slot callback and before IRQ-handler registration. Allocation results
  remain unchecked where the SDK ignores them.
- Removed inherited BR23 logs: this SDK member has no log-tag dependency.
  Assertion file provenance and line numbers now match the BD19 object.
  Assertion/reset hooks retain fall-through rather than adding app guards.
- The fill uses the SDK's memory intrinsic. The compiled configuration has
  the original MMIO types, addresses, alignment and ordering across branches.

Replaced the obsolete BR23 fixture with actual BD19 tests. Literal traces
check every MMIO read/write and callback order for modulated/continuous data,
hopping/fixed channels and init mode 16. Tests cover both transmit/receive
format branches, all six packet indices, all 256 byte-valued channels,
upper-bit preservation, signed power arguments, callback-updated fields,
allocation/IRQ global-pointer mutation, ignored return values and assertion
fall-through. Frame-allocation fault tests stop in the hook before the SDK's
invalid dereference. AddressSanitizer and UndefinedBehaviorSanitizer pass
with recovery disabled; test payload checks address the backing allocation
rather than indexing the SDK's GNU zero-length array.

The compiled `bredr_fcc_set_param` canonical IR matches the SDK after ignoring
the build's `nobuiltin` call attribute. Remaining configuration IR differences
are pure calculation ordering and widened channel arithmetic (exhaustively
checked for the byte domain); init has equivalent assertion block ordering
and loop rotation. All state loads around external callbacks, volatile
accesses and public/imported ABIs were reviewed explicitly.

Validation at `BUILD_ID=2b5050f`: all four Jieli products build; both layouts,
16 tooling regressions and all 61 accepted members (60 ownership groups)
pass. The full host suite passes with 605 `: ok` and 36 legacy reports.
The manifest supplies 792 public functions and 57 globals; the retained
archive contains 50 original members and 1288 verified symbol-index entries.
All three BR23 frozen firmware baselines match byte for byte. BD19 remains
196664 bytes, SHA-256
`fd80defde8aeccb48187b2ef916072844f11bf00b327cb52361685f9ef4c6654`.
Artifacts/logs: `/work/analysis/btctrler-bd19/fcc-finish/` and
`/work/analysis/btctrler-bd19/fcc-finish-{builds,checks}.log`.
No flashing or hardware testing.
## BD19 `ll_cis.c.o` occurrence 1

At base `1854656`, reused `src/platform/jieli/btctrler/ll_cis.c`, the existing
BR23 implementation. Added the missing public CIG/CIS lifecycle, channel/test
mode, counter, and init/close routines from BD19 IR. Internal reset, message,
and established-event helpers remain local to the source. No globals move.
Proposed coordinator manifest entry:

```json
{
  "member": "ll_cis.c.o",
  "occurrence": 1,
  "sha256": "ea9b6e2b1d646522d717ea50d07e6509c1d6757b93eedb721738bc54d7e39e75",
  "sources": ["src/platform/jieli/btctrler/ll_cis.c"]
}
```

The source supplies all 28 BD19 external functions and no globals, matching
the member inventory. The added routines use the existing shared CIG/CIS
layouts; no BD19-only layout or hardware access was needed. Existing BR23
bookkeeping and duration code was left unchanged. Preserved SDK quirks include
the empty-group early return in `ll_cig_free`, the shared-list CIS-ID lookup,
handle exhaustion returning 102 after asserting, and unchecked lookup in
interleaved duration calculation. Channel setup remains the SDK's empty
function. The established-event helper follows the BD19 IR field order and
format string.

Validation:

- `PATH=/work/bin:$PATH make build/gamesir-cyclone-2-pad/src/platform/jieli/btctrler/ll_cis.o` — passed.
- `PATH=/work/bin:$PATH make build/gamesir-nova-2-lite-dongle/src/platform/jieli/btctrler/ll_cis.o` — passed with Q32S.
- `PATH=/work/bin:$PATH make build/host/test-gamesir-cyclone-2-pad/platform/jieli/btctrler/ll_cis && build/host/test-gamesir-cyclone-2-pad/platform/jieli/btctrler/ll_cis` — `btctrler_ll_cis: ok`; expected values follow BD19 IR for the recovered list/test-mode routines.
- `PATH=/work/bin:$PATH python3 tools/controller.py audit --manifest /tmp/bd19-ll-cis-manifest.json --board gamesir-nova-2-lite-dongle` — all external definitions match. The temporary manifest contained only the proposed entry above and was not committed.
- `PATH=/work/bin:$PATH make controller-compare` inspected 108 BR23 and 111 BD19 members, then stopped before the reuse census because the BTstack submodule is absent (`bluetooth_gatt.h` missing). Full firmware comparison/build was not established. No hardware testing.


## ll_cis review and acceptance: 62 accepted members

Reviewed worker commit `71ac297` against original BD19 `ll_cis.c.o`
occurrence 1 (`ea9b6e2b1d646522d717ea50d07e6509c1d6757b93eedb721738bc54d7e39e75`),
the BR23 member, the reuse census and emitted Q32S IR. Most SDK bodies are
shared; cross-chip differences are helper inlining/loop rotation rather than
hardware changes. Reused the existing C and reviewed the added lifecycle,
message and test-mode paths. The member supplies exactly 28 public functions
and no globals, all in `.ble_ll_code`; private message/event helpers retain
internal linkage. All CIG/CIS/ISO concrete layouts match recursively across
21 type pairs after resolving recovered typedef names. The SDK's opaque
`lbuff_head` pointee remains an external boundary, not a claimed layout proof.

Corrected two worker discrepancies:

- `ll_cis_init` preserves the existing allocation bitmap; the SDK does not
  clear it. List heads, limits/counts, master/slave callbacks and dispatch
  slot 12 initialization retain their original order.
- Failure-message handling reloads the handle after logging and the status
  after lookup. The previous worker code captured both before callbacks.
  Message fields now use an alias-safe two-byte-aligned packet type matching
  the IR accesses, and tests mutate the message at both callback boundaries.

The event-emitter call uses a BD19 assembly-name alias with the SDK's byte
return ABI; the generic header declares bool. Returned values are ignored.
BD19 zero-fill operations retain the memory intrinsic. BR23 keeps its prior
fill-call behavior. The new lifecycle functions are also equivalent to their
BR23 SDK counterparts; no BR23 application behavior changes were introduced.

Extended host tests exercise dispatch registration and untouched table slots,
bitmap preservation, master/slave callback order, all 16 established-event
arguments (including the SDK's repeated fields), logged message mutation,
failure destruction, success establishment with an ignored missing ACL link,
byte counter wrap after callback mutation, MIC/unknown messages and close
with both empty and populated groups. Existing list, allocation, lookup,
handle-exhaustion, test-mode/counter and duration tests remain in place. The
SDK's empty-CIG free quirk and unchecked event/lookup pointers are preserved.

All four Jieli products build at `BUILD_ID=2b5050f`; the full host suite passes
with 606 `: ok` and 36 legacy reports. Both layouts, 16 tooling regressions
and all 62 accepted members (61 ownership groups) pass. Totals are 820 public
functions, 57 globals, 49 retained SDK members and 1260 verified index entries.
DUT remains supplied by its original SDK member. All three BR23 frozen
firmware baselines match byte for byte. BD19 `app.bin` is 196684 bytes,
SHA-256 `18ddaba77059f67ed46e4640c659a9cf73671c84c311d0b61920fa283268268c`.
This is 20 bytes larger than the prior FCC checkpoint after replacing the CIS
member with the reviewed C. No hardware testing. Logs/artifacts are under
`/work/analysis/btctrler-bd19/ll-cis-review/` and
`/work/analysis/btctrler-bd19/ll-cis-review-{builds,checks}.log`.


## ll_common_iso review and acceptance: 63 accepted members

Reviewed worker `2df53ab` (cherry-picked as `354a765`) against BD19
`ll_common_iso.c.o`, occurrence 1, SHA-256
`b0d323c6a83a3966a733d5f1dc4a176ecd604678e5d4b9c780541e7ade8c0d6d`,
BR23 IR and current C. Accepted all 14 public functions and four public
globals. Lifecycle, command dispatch, ISOAL receive paths and the exported
SDK self-test were reviewed beyond the existing shared data/codec routines.
All exports retain `.ble_ll_code`; the private segment packer also retains
that section. The four zero initializers, sections and alignments match
programmatically, as does the private unsigned all-ones watermark. Recursive
type matching resolves 52 named type pairs. `le_parameter` differs only in
four explicit one-byte padding arrays replacing SDK byte padding fields;
field sizes, positions and the accessed event-mask/buffer fields agree.
The external opaque queue pointee remains an external boundary.

Corrected worker discrepancies before acceptance:

- Initialization writes the 16-bit `maxIsoPduLen` field, not `max_cis_number`.
  The wide event-mask stores use alias-safe types with the SDK alignments.
- Elapsed-time watermark comparison is unsigned, including timer wrap. Its
  initial UINT32_MAX value consequently remains unchanged in ordinary use.
- HCI commands dispatch on the low ten opcode bits and reply with the full
  opcode. Setup diagnostics snapshot decoded fields before log callbacks;
  unknown BIS directions return success as in the SDK. Reading test counters
  ignores helper return values. Ending a CIS test reads counters when RX is
  enabled, then clears RX and TX in the SDK order.
- Framed receive iteration subtracts from the current remaining byte count,
  allowing multiple segments in one PDU. Time-offset decoding deliberately
  continues to use the initial PDU payload, including later segments.
- Header packing reloads fields across log callbacks and overlapping output
  stores; segment packing reloads its header after logging. Aligned header
  word accesses and unaligned self-test packet stores use alias-safe types.

Focused tests now cover the corrected initialization, unsigned watermark and
wrap, full HCI opcodes, ignored counter-helper results, RX-only/TX-only test
end, invalid BIS directions, two completed framed segments in a single PDU,
header mutation at logging boundaries and output overlapping its header.
Existing lifecycle, queue dispatch, command, codec, fragmentation and receive
fixtures pass with both target headers. SDK undefined subheaders/counters,
unchecked framed sizes, null-PDU fragmentation and retained/leaked buffers
remain SDK quirks; no application fixes were applied to BD19.

All four products build at BUILD_ID=2b5050f. Full host tests exit zero with
607 `: ok` reports and 36 legacy reports; both layouts, the 16 tooling tests
and all 62 ownership groups pass. Totals: 63 accepted members, 834 unique
public functions, 61 globals, 48 retained SDK members and 1242 verified archive
index entries. All three frozen BR23 firmware baselines match byte for byte.
BD19 app.bin is 196664 bytes, SHA-256
`7437e8e2d703d467e4ec3d9e5284ebfafebb38f95fd4b963567181aeba33fc40`,
20 bytes smaller than the CIS checkpoint after replacing the ISO member and
relinking with LTO; this is semantic ownership, not a byte-identical BD19
rebuild claim. DUT remains supplied by the SDK. No hardware testing.
Review IR/diffs are in `/work/analysis/btctrler-bd19/ll-common-iso-review/`;
validation log: `/work/analysis/btctrler-bd19/iso-review-final-checks.log`.

## BIS slave acceptance: 64 accepted members

Reviewed `daf9867` against BD19 `ll_bis_slave.c.o` occurrence 1
(`fe8dd6566f0b857d7f7548280245b1b3933bcb79e7d0a4edf2fe3f939f4b86f1`).
The accepted implementation is `bd19/ll_bis_slave.c`, providing exactly four
public functions and no globals. The full recovery/review report is in the
acceptance commit body (`git log -- bd19/ll_bis_slave.c`, relative to the
controller directory). Follow-ups are in `revisit-bd19.md` (C13, B13–B15, R07).

All four builds at BUILD_ID=2b5050f, full host suite (608 `: ok`, 36 legacy
reports), both layouts, 16 tooling tests and all 63 ownership groups pass.
Focused ASan/UBSan checks pass. Totals: 64 members, 838 functions, 61 globals;
47 retained SDK members, 1238 verified index entries. All three BR23 frozen
images and the prior BD19 image match exactly (BD19 196664 bytes, SHA-256
`7437e8e2d703d467e4ec3d9e5284ebfafebb38f95fd4b963567181aeba33fc40`).
No hardware testing. Logs/IR: `/work/analysis/btctrler-bd19/bis-slave-review/`.

## CIS master/slave acceptance: 66 accepted members

Reviewed worker checkpoints `41b2680` and `671de02` against occurrence-1 BD19
IR and BR23 counterparts. Both target-specific sources now supply their exact
whole-member boundaries: master three public functions plus the 32-byte zero
pending global; slave four public functions with no globals. The complete
correction/evidence/validation report is in this acceptance commit's body;
follow-ups are tracked in `revisit-bd19.md`.

All four products at BUILD_ID=2b5050f, the full host suite (610 `: ok`, 36 legacy
reports), both layouts, 16 tooling tests and all 65 ownership groups pass.
Focused ASan/UBSan checks pass with halt-on-error enabled. Totals: 66 members,
845 functions and 62 globals; 45 retained SDK members, 1230 verified index
entries. All frozen BR23 images and the prior BD19 image match exactly:
BD19 196664 bytes, SHA-256
`7437e8e2d703d467e4ec3d9e5284ebfafebb38f95fd4b963567181aeba33fc40`.
No hardware testing. Artifacts: `/work/analysis/btctrler-bd19/cis-pair-review/`.

## BD19 analog BLE acceptance (worker 7d4a9e2)

Occurrence 1 `analog_ble.c.o`, SHA-256
`fe535e9a3d3c0ff4d2c19e929c04f0fdc5046f34d75d2feb1d06051051a16f51`,
is supplied by `bd19/analog_ble.c`: six public functions, no globals, all
in `.ble_rf_code`. Review corrected the callback-visible power-byte reload
and the BLE5 zero-extension (BLE4 sign-extends). Regression tests derive
these differences from the SDK load/store IR and mutate the byte in the
max-level callback. BR23 retains its separate source unchanged.
No-op AGC setters and byte-wrap/clamp behavior are faithful SDK recovery.
See the acceptance commit body for validation and remaining limitations.

## BD19 authentication acceptance (worker 3ffbb43)

Occurrence 1 `authentication.c.o`, SHA-256
`af15cd3efcc12f0e368c97ad4c323e328363865d79df60b62bf11fbaf787be2d`,
is supplied by shared `authentication.c` and its BD19-only curve include.
The exact boundary is four Classic E functions and five uECC selectors,
with no public globals. All five 176-byte curve descriptor initializers,
callback relocations, Classic tables, sections and alignment match SDK IR.
Seven curve helper bodies match normalized IR; remaining helpers differ
by equivalent loop placement or memset versus four zero stores.
`python3 tools/btctrler/check_uecc.py` checks all five reducers, curve
polynomials, Jacobian doubling and infinity handling against independent
Python integers (740 checks). This exercises the specialization callbacks
with a model backend, not device ROM. Existing Classic vectors pass on both
headers, including preserved key-reduction defects and allocator behavior.
BR23's source path is unchanged; common allocation diagnostics continue to
use the existing owned assertion policy. See the acceptance commit body.

Combined acceptance checkpoint: analog BLE `c7712ed`, authentication `3dfd4bd`.
At `BUILD_ID=2b5050f`, all four Jieli products, full host suite (612 `: ok`
reports plus 36 legacy reports), both layout checks, 16 tooling tests and
67 ownership-group audits pass. Manifest: 68 accepted members, 860 unique
public functions, 62 globals; 43 retained SDK members / 1215 verified index
entries. All three BR23 images exactly match their frozen baselines.
BD19 app is 196512 bytes, SHA-256
`c7faccac51474bdcf46251e863c97759cd99c50cf18bafa92017a4dade9fece8`.
The independent curve harness passes 740 checks. Both completed worktrees
and branches were removed after checking clean nested repositories and
ancestry. Only canonical integration and the untouched primary checkout remain.
No hardware validation performed.

## BD19 DUT acceptance (Luna worker 9140e3e)

Fresh occurrence-1 recovery of `bredr_dut.c.o`, SHA-256
`7a56461f6d412c1d53d42dbf41c6a1f5719bcf59235e4c180b46bd9a1d4a87ad`,
supplies six public functions and both PRBS9 arrays from `bd19/bredr_dut.c`.
The six packet/PRBS table initializers, sections and alignments match SDK IR;
the private ten-byte fill table also matches. All public code retains
`.classic_rf_code`, PRBS data `.classic_rf_const`.

Independent review corrected scenario optcntl writes, snapshot-before-poll
timing, clock-adjust predicate, unsniff/tws bitfield mappings, post-allocation
scenario reload, frequency/MMIO ordering and memory intrinsic boundaries.
Focused tests cover these and callback-driven slot/handle changes, null RX
enqueue, all packet rows and independently generated PRBS crossing a 511-byte
period. Expanded fixture passes ASan/UBSan. BR23 source is unchanged.
B27/B28 track confirmed SDK defects and ABI/hardware follow-ups; R04/H13
now distinguish accepted recovery from the withdrawn draft.
Full provenance, semantic review and validation are in commit bodies.

DUT combined checkpoint: acceptance `5f51b69`; canonical fixed-ID build/test,
both layouts, 16 tooling tests and all 68 ownership groups pass. Full host
suite: 613 `: ok` reports plus 36 legacy reports. Totals: 69 accepted members,
866 functions, 64 globals; 42 retained SDK members / 1207 verified index entries.
All four images are unchanged from `773ff09`, including BD19 at 196512 bytes
(`c7faccac51474bdcf46251e863c97759cd99c50cf18bafa92017a4dade9fece8`).
The worker and superseded draft worktrees/branches were removed after ancestry
and nested-repository checks. The authorized duplicate draft is preserved at
`/work/analysis/btctrler-bd19/dut-luna-review/superseded-draft/`.
No hardware validation.

## BD19 H4 UART hardware acceptance (worker 6e03c7a)

Occurrence 1 `h4_uart_hw.c.o`, SHA-256
`834544fb1d0a96ee6e02e3e4b292dee21807540d8390349a7b960bf0a762e859`,
is supplied by `bd19/h4_uart_hw.c`: four public functions, no public globals.
Weak pin override, init, private interrupt callback and close match normalized
SDK IR exactly; transmit differs only in loop rotation. Buffer/clock-string
initializers, sections and alignment match. Independent regular target compile
and ASan/UBSan fixture pass. BR23 sources are unchanged.
The draft's wrong register base was corrected before commitment; final MMIO
is literal 1974528 (`0x1e2100`) plus SDK offsets. Worker report prose inverted
the TX-ready condition: source/SDK wait while bit 15 is clear, until it is set.
C15/B29 record later consolidation and baud/poll/count-contract follow-ups.
Acceptance commit body carries the detailed review and validation report.

UART combined checkpoint: acceptance `23b5f74` passes canonical fixed-ID
four-product/full-host/layout/tooling/archive validation: 614 `: ok` reports,
36 legacy reports, 16 tooling tests and 69 ownership groups. Totals: 70
accepted members, 870 functions, 64 globals; 41 retained members / 1203
verified index entries. All four binaries match `d91cba0`. The clean worker
worktree and merged branch were removed after checking nested dependencies.
No hardware validation.

## BD19 audio codec acceptance (worker b4d0de8)

Occurrence 1 `audio_codec.c.o`, SHA-256
`602b140db1a0d100fbbf9e40fb4a262f145db89325f7b74f5e0615a783082d09`,
is supplied by `bd19/audio_codec.c`: nine functions and common
`palCodecScratch[400]` (1600 bytes, align 4), plus private two-stream storage.
Independent review replaced duplicated typedefs with existing SDK header
types, restored the SDK memset intrinsic, and corrected an extra diagnostic
comma. All nine globals/string initializers now match SDK IR exactly,
including common linkage and alignment. Five function bodies match canonical
IR; StreamIn/Out differ only in printf nounwind attributes; remaining lookup,
start and delay code differs in equivalent loop/branch shape.
Expanded ASan/UBSan fixture covers wrap, full capacity, stale reuse state,
exact mute slot writes and diagnostic reentry/reloads with a nonnull unused
ready callback. C16/B30/B31 record consolidation and deliberately retained
codec stubs/lifecycle quirks. BR23 production sources are unchanged.
The acceptance commit carries complete evidence and validation.

Audio combined checkpoint: acceptance `e9f8aa7` passes canonical fixed-ID
four-product/full-host/layout/tooling/archive validation: 615 `: ok` reports,
36 legacy reports, 16 tooling tests and 70 ownership groups. Totals: 71
accepted members, 879 functions, 65 globals; 40 retained members / 1193
verified index entries. All four binaries match `bc7d3c6`. The clean worker
worktree and merged branch were removed after nested-dependency checks.
No hardware validation.

## BD19 H4 UART interface-loop acceptance (worker e0df9fa)

Occurrence 1 `h4_uart_interface_loop.c.o`, SHA-256
`7445dd667db9f8183967f7772454bc5982318e1ef5205a438dc574867a6357c9`,
is supplied by `bd19/h4_uart_interface_loop.c`: ten functions, no public
globals. Byte-return `__data_empty` uses a local asm alias to avoid the
generated header's C-bool ABI. All five private state/table initializers,
callback relocations, sections and alignments match original IR; reachable
diagnostic literals match. The second-switch unknown-type diagnostic is
unreachable after initial validation and is removed by the current compiler.
Review restored logs, copy/length/callback reloads, config constness and
exact prefix clears. Ten functions match normalized IR exactly; the rest
differ in SSA/CFG, no-argument IRQ prototype casts, lifetime metadata or
constant-size memset length type. Focused sanitizer tests cover ring wraps,
IRQ/callback mutation, type 1/2/5 parsing, two-wire stale-count timeout refill,
thread lifecycle and both send modes. C17/B32–B34 retain follow-ups.
The acceptance commit body carries full evidence and validation.

UART-loop combined checkpoint: acceptance `2d8a690` passes canonical fixed-ID
four-product/full-host/layout/tooling/archive validation: 616 `: ok` reports,
36 legacy reports, 16 tooling tests and 71 ownership groups. Totals: 72
accepted members, 889 functions, 65 globals; 39 retained members / 1183
verified index entries. All three BR23 binaries are unchanged. BD19 is
196520 bytes (+8), SHA-256
`767aa68e4e4c8ee42b7865a995ac73b54736a60000b41562c6987dabefbfcce8`.
Disassembly shows `thread_delete` outlined (38 bytes), with `ll_destroy`
shrinking 94 to 70 bytes; UART transport remains absent from linked symbols.
The clean worker worktree and merged branch were removed after checking
nested dependencies and ancestry. No hardware validation.

## BD19 in-chip H4 acceptance (worker cab02d3)

Occurrence 1 `h4_controller_interface.c.o`, SHA-256
`3821c59f332d4bce78a7eea8ec27da14018c375e27099d5d003f216bb389d45a`,
is supplied by `bd19/h4_controller_interface.c`: public instance function
and const readiness-function pointer `__h4_can_send_packet_now`. All seven
function bodies match normalized SDK IR exactly. Both pointer/table
initializers, callback relocations, sections, alignment and all four private
strings match. Tests cover both log states, callbacks, narrowing, pointer
forwarding/mutation and ignored downstream return; sanitizer run passes.
BR23 combined `hci_transport.c` is unchanged. C18/B35 record future source
consolidation and explicit transport-contract decisions. Full report in the
acceptance commit includes integration validation.

In-chip H4 combined checkpoint: acceptance `0af2992` passes canonical fixed-ID
four-product/full-host/layout/tooling/archive validation: 617 `: ok` reports,
36 legacy reports, 16 tooling tests and 72 ownership groups. Totals: 73
accepted members, 890 functions, 66 globals; 38 retained members / 1181
verified index entries. All three BR23 binaries are unchanged. BD19 is
196552 bytes (+32), SHA-256
`c2247bdd50eeee979cfccbdbab525d5de5d0168354bb7dbd197d90e65ee13866`.
Disassembly shows the existing HCI packet handler inlined into the active
transport wrapper; all seven recovered function bodies match SDK IR.
The clean worker worktree and branch were removed after nested-dependency
and ancestry checks. No hardware validation.

## BD19 HCI controller acceptance (worker 843223a)

Occurrence 1 `hci_controller.c.o`, SHA-256
`c077ec3c2f015655f1fdf1ea2388828aebe6609d2ee32a004d6304037b73c0e8`,
is supplied by `bd19/hci_controller.c`: exactly 18 public functions and
mutable callback global `__h4_send_packet`. Private 48-byte state, 2560-byte
buffer, sections, alignments and packed 515-byte event layout were checked
against raw IR. Review corrected wrapping event wire lengths and the queued
allocation addition, retaining callback ordering, routing distinctions,
assertion continuation and SDK unchecked copies. Existing BR23 sources remain
unchanged. Shared assertion support deliberately retains the existing app
policy instead of SDK diagnostic text. C19/B36–B38 record consolidation and
robustness follow-ups; the worker and acceptance commit bodies carry details.

Worker fixed-ID validation passes four products, full host suite (618 `: ok`
reports plus 36 legacy reports), 16 tooling tests, both layouts and all 73
ownership groups. Totals: 74 accepted members, 908 functions, 67 globals;
37 retained members / 1162 verified index entries. Focused ASan/UBSan passes.
All three BR23 images are unchanged. BD19 is 196464 bytes (-88), SHA-256
`fa9e1f00eafe8ef4c52d4ef043032ffe6f623f4ed773a2c31b2c53e056660b10`.
Symbol comparison shows common HCI handlers now inlined into private
`bd19_hci_command_process` (590 bytes); old separate dispatcher/common
handler bodies total 670 bytes. Lifecycle/run-loop/owner calls also change
inlining. Production fixed const configuration permits mode caching; mutable
host fixtures additionally check the SDK callback reload boundaries.
No hardware validation.

HCI controller combined checkpoint: acceptance `68e3542` passes canonical
fixed-ID validation with the same four product hashes as worker validation:
618 `: ok` reports, 36 legacy reports, 16 tooling tests, both layouts and
73 ownership groups. Totals remain 74 accepted members / 908 functions /
67 globals; 37 retained members / 1162 index entries. BR23 images unchanged;
BD19 196464 bytes, SHA-256
`fa9e1f00eafe8ef4c52d4ef043032ffe6f623f4ed773a2c31b2c53e056660b10`.
After checking clean root/nested dependency status and merged ancestry, the
completed worker worktree and branch were removed. No pending workers or
hardware validation.

## Clean-branch BD19 Classic HCI parser acceptance

Worker `55e2b12` and coordinator baseband recovery supply occurrence 1
`lmp_hci_cmd_parser.c.o`, SHA-256
`97b7f09ba18f75bb9494e17d3ac4ecd0bf84c7ab005fc91e184e8c9900e127af`.
Two dedicated BD19 sources provide exactly seven public functions, no globals,
and `.classic_lmp_code` placement. BR23 baseline sources remain unchanged.
Review restored the original memory intrinsic, helper pointer/offset boundary,
QoS/clock saved handles, diagnostic gating/prototypes and explicit reader
order. All HCI diagnostic string bytes match the SDK. Three normalized bodies
match exactly; status differs only by phi incoming order. Control/policy CFG
and baseband assertions/voice/EIR loops were checked against raw IR.

Both focused sanitizer fixtures pass and exercise all opcode paths with both
logging states, partially initialized messages, callback mutation, EIR wrapping,
unaligned prefix clear and pointer/double varargs. Mock backing/recovery hooks
make returning-assert fixtures safe; production unsafe paths are preserved.
C20/B39–B41 record consolidation, malformed input, reply ABI and placeholder
behavior follow-ups. Acceptance commit body carries the full durable report.

Worker validation: `make -j32 controller-objects controller-test test
controller-layouts controller-tools-test controller-audit` passes: 195 `: ok`
reports, 12 legacy reports, 31 tooling tests, both layouts, 74 ownership groups.
Totals: 75 accepted members, 915 public functions, 67 globals; 36 retained
members / 1155 verified index entries. This is the clean controller branch;
no newer four-product image or hardware validation is claimed.

Classic HCI parser combined checkpoint: acceptance `35f0689` passes the
canonical controller suite with 195 `: ok` reports, 12 legacy reports,
31 tooling tests, both layouts and 74 ownership/retention audits. Totals:
75 members / 915 functions / 67 globals; 36 retained members / 1155 verified
index entries. All 123 original BR23 source files remain byte-identical to
`44d0073`. The clean worker checkout and merged branch were removed after
ancestry and read-only dependency-status checks. No pending worker or hardware
validation. The migration IR reader's joined header path was corrected in
`aec5c00`, with both target CLI smoke checks; this changes no controller code.

## Clean-branch BD19 A-law/μ-law codec acceptance

Worker `cc9237e` supplies occurrence 1 `au_law_codec.c.o`
(`9f9eaff73b762b21bd15003966b4375368405f2738e38416d4a2c442666926d5`)
and `esco_codec.c.o`
(`b6365601be9603886ce9cd9ab6c11d6c384277c5d00418c217404972c84824a8`).
Dedicated BD19 sources export two const callback tables and the selector;
eight codec callbacks remain private. Both four-pointer table initializers,
16 segment-table bytes, widths, alignment and default sections match SDK IR.
Review corrected the worker's μ-law decode sign and replaced the ineffective
combined selector fixture with a standalone stub fixture plus an independent
check linked against both actual codec tables. The sign mistake is a resolved
translation error, not an SDK defect.

`check_law_codecs.py` independently models the original fixed-width IR and
checks all 131072 encode cases (65536 samples per codec), all 512 decode cases,
maximum positive signed16 count, negative/zero no-access paths, init width,
selectors and guard sentinels. Focused ASan/UBSan fixtures pass. Four private
need/init bodies match normalized IR exactly; other differences are loop/CFG
shaping, equivalent masked shifts/truncation, selector return merging and
private-table linkage. C21/B42 retain sharing and SDK full-scale follow-ups.

Worker controller suite passes: 197 `: ok` reports, 12 legacy reports,
31 tooling tests, both target layouts and 76 ownership/retention audits.
Totals: 77 accepted members, 916 public functions, 69 globals; 34 retained
members / 1152 verified index entries. Original BR23 C is unchanged. No
application-image or audio hardware validation is claimed.

Law-codec combined checkpoint: acceptance `a3fce20` passes the canonical
controller suite: 197 `: ok` reports, 12 legacy reports, 31 tooling tests,
both layouts and 76 ownership/retention audits. Independent exhaustive
validation again passes all 131072 encode / 512 decode values plus ABI and
selector checks. Totals: 77 accepted members / 916 functions / 69 globals;
34 retained members / 1152 verified index entries. Original 123 BR23 C files
remain byte-identical to `44d0073`. Clean worker/dependency status and merged
ancestry were checked before deleting the completed worktree and branch.
No pending workers or audio hardware validation.

## Clean-branch BD19 TWS power balance acceptance

Occurrence 1 `tws_power_balance.c.o`, SHA-256
`eeb6614a795aba99e8479bbcf893c96db057ba470612aed895e14570625acb11`,
is supplied by dedicated `bd19/tws_power_balance.c`: two weak hooks, init and
one const callback table. Initial worker `2365fae` accidentally changed the
root BR23 file; coordinator amended it to `fccd25a`, moving the recovery to
the assigned BD19 path and restoring the root file before integration.
The full original checkpoint patch/report is archived under the review's
analysis directory. No BR23 source change remains in the reviewed branch.

Five normalized bodies match SDK IR after ignoring the compiler's nobuiltin
call attribute; slave TX differs only in the order of pure comparisons.
All three global initializers, sections and alignments match exactly. The
weak role predicate retains zeroext i8 through an asm alias despite the bool
header declaration. BD19 table attribute 1, IRQ TX sections, slave-only gate,
pre-hook remote capture and post-predicate counter reload are retained.

Focused ASan/UBSan fixtures and a separate TU with actual strong hook overrides
pass, including byte result 0x80, callback reentry, reset-before-hook ordering,
width wraps, disabled gate, pending flag retention and unchanged init state.
C22/B43 retain sharing, ABI and framing/lifecycle follow-ups; H25 is confirmed.
Worker suite passes: 198 `: ok` reports, 12 legacy reports, 31 tooling tests,
both layouts and 77 ownership/retention audits. Totals: 78 accepted members /
919 public functions / 70 globals; 33 retained members / 1148 index entries.
All original 123 BR23 C source files remain byte-identical to `44d0073`.
No application-image, battery calibration or TWS hardware validation claimed.

Power-balance combined checkpoint: acceptance `1fd79e4` passes canonical
controller validation with 198 `: ok` reports, 12 legacy reports, 31 tooling
tests, both layouts and all 77 ownership/retention audits. Totals: 78 accepted
members / 919 functions / 70 globals; 33 retained members / 1148 verified index
entries. Original BR23 source bytes remain unchanged. Corrected worker ancestry
and clean root/dependency status were checked before removing the completed
worktree and branch. No pending workers or TWS hardware validation.

## Clean-branch BD19 TWS CI-data acceptance

Worker `d5c3730` recovers occurrence 1 `tws_ci_data.c.o`, SHA-256
`c19a5053261a1aa0be1a52034022d2a0d08b66622b4c7a7cdc001fd95415c30c`.
Dedicated `bd19/tws_ci_data.c` supplies two public functions, five private
callbacks and one const channel table. The 100-byte private context and
28-byte table initializers, alignment, sections, callback relocations and
volatile byte accesses match SDK IR. The review maps 38 type shapes without
mismatch. Remaining body differences are equivalent context selection, phi
incoming order, no-argument IRQ prototypes and compiler call annotations.
No production semantic correction was required.

Coordinator extended focused tests for repeated open, ignored set/post errors,
NULL and external contexts, unsigned timeout 1, successful return despite IRQ
context mutation, both post/copy alias paths and reentrant flag updates.
A modeled timeout-event wake still returns the original requested length;
this confirms control flow, not actual OS delivery outcome. C23/B44–B45 retain
sharing, callback ABI, borrowed-pointer lifetime and framing follow-ups.
BR23's event callback returns void; BD19 returns int zero, so source sharing
requires a deliberate ABI choice rather than simply moving this file.

Worker full suite and ASan/UBSan pass: 199 `: ok` reports, 12 legacy reports,
31 tooling tests, both layouts and 78 ownership/retention audits. Totals:
79 accepted members / 921 functions / 71 globals; 32 retained members /
1145 verified index entries. Original 123 BR23 C files remain byte-identical
to `44d0073`. No application-image or TWS hardware validation claimed.

CI-data combined checkpoint: acceptance `8e8c024` passes canonical validation
with 199 `: ok` reports, 12 legacy reports, 31 tooling tests, both layouts and
78 ownership/retention audits. Totals: 79 members / 921 functions / 71 globals;
32 retained members / 1145 verified index entries. BR23 baseline C is unchanged.
Clean worker/dependency status and merged ancestry were checked before deleting
the completed worktree and branch. No pending workers or hardware validation.

## Clean-branch BD19 TWS LMP-sync acceptance

Reviewed worker `eaf2359`: `tws_lmp_sync.c.o` occurrence 1, SHA-256
`e87bad0ec260b7c9a29c64b823ce0ac36710e60ca3e6680fb75f8dc6c0fe5ded`.
Five public functions, three private callbacks and two 28-byte channel tables
are recovered. Both table initializers, relocations, sections and alignment
match the SDK exactly; 38 mapped type shapes agree. All eight function bodies
were compared with original IR. Remaining differences are equivalent boolean
branches, pure arithmetic scheduling, switch ordering, compiler call annotations
and an alignment assumption matching the SDK input domain.

Coordinator corrected external memcpy calls to SDK memory intrinsics and
restored the aligned u32 detach timestamp store. The host TX buffer is now
explicitly aligned. Additional checks cover full-byte eSCO preservation, negative
non-detach length, null connection, bit-zero selection and both no-op callbacks.
Retained SDK tws_pair.c.o occurrence 1 still supplies do_phone_link_detach;
BR23's private callback was not exposed. C24/B46–B47 record consolidation,
timer allocation/lifetime and unusual wire framing follow-ups.

Worker full controller suite and census pass: 200 `: ok` reports, 12 legacy
reports, 31 tooling tests, both layouts and all 79 ownership groups. ASan/UBSan
fixture passes. Totals: 80 accepted members, 926 functions, 73 globals;
31 retained members and 1138 verified index entries. All 123 original BR23 C
files remain byte-identical to 44d0073. Controller-only validation; no firmware
image or hardware validation claimed.

TWS LMP-sync combined checkpoint: acceptance `5d37541` passes canonical
controller validation and census with 200 `: ok` reports, 12 legacy reports,
31 tooling tests, both layouts and all 79 ownership groups. Clean worker and
dependency status plus merged ancestry were checked before deleting the worker
worktree and branch. Totals remain 80 accepted / 31 retained members, 926
functions, 73 globals and 1138 retained index entries. No hardware validation.

## Clean-branch BD19 TWS AFH acceptance

Reviewed worker `90772cc`, `tws_afh.c.o` occurrence 1, SHA-256
`bdb79b3563294c879c6542c24c6f63b1b413fcc3e57122d4815ee265d1a20ec0`.
`bd19/tws_afh.c` provides three public functions, four private callbacks/helpers
and const tws_afh_sync. Channel 3/attribute 5 table, all callback relocations,
28-byte target size, section and alignment match SDK IR. Private ten-byte
zero map and four-u32 classification table values/alignment also agree;
private versus internal linkage and private global names are immaterial here.
38 mapped type shapes match. All seven functions were reviewed against raw
SDK IR and original BR23 tws_pair.c.o counterparts, not merely symbol coverage.

Coordinator removed an incorrect IRQ section from master RX, restored per-pair
classification stores/reloads and widened its lookup table to original u32.
Remaining normalized differences are loop rotation/branch order, local lifetime
markers, equivalent scalar arithmetic, declaration prototypes and call flags.
The helper now preserves overlapping-buffer source reloads and output stores;
actual production caller uses disjoint stack arrays. Master TX retains exact
pending-update zero result, lazy timer, strict change threshold, callback reloads,
aligned timestamp store, and refreshed link-map copy. No BR23 source or
application policy was imported. C25/B48–B49 track sharing, timing/framing and
unsupported optional WiFi imports. Worker wording about BR23 app masking is
qualified: this is already present at clean 44d0073, not a newly imported fix.

Focused tests now exercise every byte classification and popcount threshold,
last-byte override, unsigned counter wrap, config/NULL/sniff gates, negative RX
length, duplicate early exit, unchanged-map response suppression, lazy 0xffff
timer result, counter reset before classification callback, clock-hook eSCO
mutation, wrapping +400, refreshed-link map after set_afh, no retransmit while
pending, expiry clearing, phone-map and all-ones fallbacks, and no-op completion.
Fixtures use complete mocks; standalone ASan/UBSan links without unresolved-symbol
suppression and passes. Both target compiler builds pass.

Worker combined controller validation/census pass: 201 `: ok` reports, 12 legacy
reports, 31 tooling tests, both layouts and 80 ownership groups. Totals:
81 accepted members / 929 public functions / 74 globals; 30 retained members /
1134 verified index entries. Original 123 BR23 C files are byte-identical to
44d0073. No firmware-image or radio/hardware validation is claimed.

Dependency limitation: numbits_table and three wifi_det_*_channel_map helpers
have no providers in the pinned cpu/bd19/liba archive indexes/current C, just
as in the original member. Application CONFIG_WIFI_DETECT_ENABLE is zero.
Do not enable this optional path without providers and actual linking tests;
controller ownership audits do not establish whole-application linkability.

TWS AFH combined checkpoint: `91f3781` passes canonical controller checks
and census: 201 `: ok` reports, 12 legacy reports, 31 tooling tests, both
layouts and all 80 ownership groups. Totals: 81 accepted / 30 retained members,
929 functions, 74 globals and 1134 retained index entries. Clean worker/dependency
status and merged ancestry checked; completed worktree and branch removed.
Optional WiFi imports remain unsupported as recorded in B49. No hardware tests.

## Clean-branch BD19 TWS event acceptance

Reviewed worker `f628a13`, `tws_event.c.o` occurrence 1, SHA-256
`73dd3fbcd0a7f28525c2014c208463e8c0aff5efa19ceebd87b97e5937211b53`.
Dedicated `bd19/tws_event.c` supplies two public functions, six private helpers
and const tws_event_sync. Channel 8/attribute 15 table, callback relocations,
28-byte target size, section/alignment and all eight persistent private globals
match SDK IR; key_events additionally gains only unnamed_addr. The exact
45-byte diagnostic string agrees. 41 mapped type shapes have no mismatch.
All eight functions were checked against original occurrence IR and BR23 IR/C.
Remaining differences are equivalent pointer versus integer sentinel comparison
on the 32-bit target, loop rotation, pure load/arithmetic scheduling, prototypes,
call flags and removal of an impossible null comparison for static queue storage.

Coordinator removed an extra key.init reset inside the receive loop: SDK clears
the event only once, so callback changes to that field must survive subsequent
notifications. Loop offsets now use int as in SDK. Focused checks cover that
mutation, direct zero/negative RX with valid backing bytes, wrapper zero-length
skip, 21-record/63-byte capacity and overflow rejection, fresh length after IRQ
enable, both 1000ms delay paths and sniff-call short circuit, zero timer result
and retry, lock/link gates, timeout without timer deletion and no-op init. Existing
worker fixtures cover three-byte truncation, duplicate sequence, acknowledgment,
notification-driven length/index changes and reset preserving timer. Original
SDK behaviors remain unchanged; C26/B50–B51 record protocol, framing, callback
and timer follow-ups. BR23's record-stride limitation does not apply to BD19.

Worker combined controller checks and census pass: 202 `: ok` reports, 12 legacy
reports, 31 tooling tests, both target layouts and 81 ownership groups. Totals:
82 accepted members / 931 public functions / 75 globals; 29 retained members /
1131 verified index entries. Both target compilers build the source; standalone
ASan/UBSan with complete mocks passes. All 123 original BR23 C files remain
byte-identical to 44d0073. No application-image or hardware validation claimed.

TWS event combined checkpoint: acceptance `3bdfa91` passes canonical controller
checks/census with 202 `: ok` reports, 12 legacy reports, 31 tooling tests,
both layouts and 81 ownership groups. Totals: 82 accepted / 29 retained members,
931 functions, 75 globals and 1131 retained index entries. Clean worker/dependency
status and merged ancestry checked; completed worktree and branch deleted.
No hardware validation or pending worker.

## Clean-branch BD19 TWS TX-sync acceptance

Reviewed worker `e024e56`, `tws_tx_sync.c.o` occurrence 1, SHA-256
`6d7850f03e1b0c8ff8942410e4493e9568a240c9817ca7a0e6863ccfdbc8779a`.
Dedicated `bd19/tws_tx_sync.c` supplies three public functions, seven private
helpers/callbacks and const tws_tx_sync. Channel 4/attribute 1 table matches
SDK initializer, relocations, 28-byte size, section/alignment. All eight private
zero state objects, including eight 8-byte records, match. 39 mapped type
shapes have no mismatch. Every body was independently checked against raw IR.
BR23 member supplies only empty init; no other unique counterparts exist.
No original BR23 source was changed or application behavior imported.

During worker recovery coordinator identified/corrected direct-post return,
role snapshot before clock, deadline zero/nonzero branch and stop behavior,
length reload after hash, output-pointer reply guard and aligned u32 response.
The committed worker includes these corrections plus host-safe named fields and
separate hash-call/result-store statements. Subsequent coordinator review
restored TX search next-pointer capture before JBHash and tx_num store after
record copies; loop index width matches SDK. Copies preserve all 8 bytes,
including padding, using aligned may-alias u64 accesses. Remaining differences
are equivalent container-derived addresses at checked target offsets, loop
rotation/branch fusion, prototypes, pure arithmetic scheduling and call flags.

Fixtures cover add/post paths, role snapshot and hash-index mutation, deadline
stop/release and zero-input-length full hash reply. Coordinator added all 65,536
sequence pairs, padding-preserving serialization across ring wrap, single-subtract
ACK behavior with byte truncation, length+hash matched-prefix posting with
redundant rehash calls, length mutation during hash, hash-only ACL prefix release,
next-pointer mutation after capture, and reset/init preserving records/list/hash.
These are safe modeled callbacks, not concurrency or radio validation. C27 and
B52–B53 retain queue capacity, framing, padding, sequence, hash and lifecycle
questions separately from corrected translation defects.

Both target compilers and standalone ASan/UBSan with complete mocks pass.
Worker combined controller checks/census: 203 `: ok` reports, 12 legacy reports,
31 tooling tests, both layouts and 82 ownership groups. Totals: 83 accepted
members / 934 functions / 76 globals; 28 retained members / 1127 verified index
entries. Retained bredr_link.c.o supplies post/available helpers and retained
tws_pair.c.o occurrence 1 supplies JBHash/is_tws_esco_working. All 123 original
BR23 C files remain byte-identical to 44d0073. No application-image or hardware
validation claimed.

TWS TX-sync combined checkpoint: acceptance `01f483b` passes canonical
controller checks/census with 203 `: ok` reports, 12 legacy reports, 31 tooling
tests, both layouts and 82 ownership groups. Totals: 83 accepted / 28 retained
members, 934 functions, 76 globals and 1127 retained index entries. Clean
worker/dependency status and merged ancestry checked before removing the
completed worktree and branch. No pending worker or hardware validation.

## Clean-branch BD19 TWS scheduler acceptance

Reviewed worker `7ad3d84`, `tws_schedule.c.o` occurrence 1, SHA-256
`7dd81f66381eb12585d2af283f24e346d5056c987260ef42f587f5d8af8298d0`.
Dedicated `bd19/tws_schedule.c` supplies six public functions, three private
helpers and two zero-initialized private byte globals. All functions except
list_empty use .tws_irq_code exactly as in the original IR; no public globals.
39 mapped type shapes agree, public boundaries and private initializers match.
Every function was checked against occurrence-specific raw IR. BR23 has no
corresponding member or unique functions; no common source is justified.

Coordinator corrected sniff gating to enable alone, nonmonitor check127/255
dispatch to mode2 and all other checks to mode1, and the empty RX-list early
exit before SBC evaluation. Queue counting uses signed int; unsigned one-bit
shift preserves LLVM behavior at index31 (indices >=32 remain invalid).
Remaining normalized differences are loop/branch reorganization, equivalent
comparisons, pure load/arithmetic scheduling, prototypes and call annotations.
Cached versus reloaded link pointers remain path-specific, including role
reload after timer disable, entry snapshot on fifth-error scheduling, and
fresh links after clock/SBC hooks. The int-to-void timer callback bitcast is
preserved rather than hidden behind a behavior-changing wrapper.

Worker temporary fixture is superseded by committed complete-mock tests.
They cover modes0–3, queue counts0–6, SBC open/closed, signed remaining times
-1/10/11/40/41, empty-queue early exit, clear-A2DP boundary31/32, phone threshold
180/181, clock snapshot and link reload, SBC callback link mutation, wait-TX
clearing, ACL priority, sniff enable versus unrelated bits, mode dispatch,
error threshold4/5, pending-mask gate, role reload after disable, timer reload
after task hook, timer0/255 lifecycle and both no-op public hooks. No mismatched
int-return callback invocation is performed in host tests. C28/B54–B55 record
feature ownership, timer ABI/lifetime and scheduling assumptions. Corrected
translation defects are not recorded as SDK bugs.

BD19 target compile and standalone ASan/UBSan pass. Normal BR23 portability
compile of this extra file is unsupported because BR23 lacks the BD19 payload
header type; no BR23 declarations or layouts were invented. The unchanged
actual BR23 source set builds in combined validation. Worker full controller
checks/census pass: 204 `: ok` reports, 12 legacy reports, 31 tooling tests,
both layouts and 83 ownership groups. Totals: 84 accepted members / 940 public
functions / 76 globals; 27 retained members / 1121 verified index entries.
Retained lmp.c.o provides SBC time, bredr_frame.c.o the clock, and tws_link.c.o
slave-monitor enable. Original 123 BR23 C files remain byte-identical to
44d0073. No application-image or hardware/timing validation claimed.

TWS scheduler combined checkpoint: acceptance `f2007fa` passes canonical
controller checks/census with 204 `: ok` reports, 12 legacy reports, 31 tooling
tests, both layouts and 83 ownership groups. Totals: 84 accepted / 27 retained
members, 940 functions, 76 globals and 1121 retained index entries. Clean
worker/dependency status and merged ancestry checked; completed worktree and
branch deleted. No pending worker or hardware/timing validation.

## Shared BR23/BD19 H4 USB descriptor acceptance

Reviewed worker `8ebb4b0`, BD19 `h4_usb_descriptor.c.o` occurrence 1,
SHA-256 `a53b109f48a09d1cc990398d8bf14fde9806069163c88c3a69e504803604fc96`.
The five SDK globals are identical between chips and had no previous C owner.
Coordinator authorized a common new root source `src/btctrler/h4_usb_descriptor.c`
instead of a BD19 duplicate. Both normal target compile commands succeed.
Coordinator decoded both actual build objects and compared every initializer:
device_descriptor18, configuration_descriptor177, string0_descriptor4,
string1_descriptor4 and string2_descriptor24. All 227 bytes, array types,
external const linkage, .hci_interface_const sections and align1 match each
original SDK exactly. No functions, private data or imports are introduced.
No translation correction was needed; source comment now names both targets.

Descriptor data are preserved without changing product identity. Clarification
of worker report: bytes20/25 at offsets12/13 are device release0x2520, not
idProduct. VID bytes12/0a at offsets8/9 give0x0a12; PID bytes01/00 at10/11
give0x0001. Product text is JL AC69 A10. C29 records sharing already completed;
B56 keeps product identity/string policy for later interface/application review.
This literal-data recovery does not establish USB enumeration or Nova2 support.

Worker combined controller checks/census pass: 204 `: ok` reports, 12 legacy
reports, 31 tooling tests, both layouts and 84 ownership groups. Totals:
85 accepted members / 940 public functions / 81 globals; 26 retained members /
1116 verified index entries. All 123 original BR23 C files remain byte-identical
to 44d0073; the new common source adds five SDK-equivalent BR23 definitions.
No host tests mirroring literals or sanitizer run were needed: compiled-IR
initializer equality provides direct verification. No application images,
USB enumeration or hardware validation claimed.

USB descriptor combined checkpoint: acceptance `9e52e7e` passes canonical
controller checks/census with 204 `: ok` reports, 12 legacy reports, 31 tooling
tests, both layouts and 84 ownership groups. Totals: 85 accepted / 26 retained
members, 940 functions, 81 globals and 1116 retained index entries. Clean
worker/dependency status and merged ancestry checked; completed worktree and
branch deleted. The new common source is SDK-identical for both targets;
original BR23 sources are unchanged. No pending worker or hardware validation.

## Clean-branch BD19 TWS data-forwarding acceptance

Completed and reviewed partial worker checkpoint `78c5461` for
`tws_data_forwarding.c.o` occurrence 1, SHA-256
`dee9f3c5e2eaa9dae33a72b4a6491973b773f25a1beb69328bc07874cef6faed`.
The worker explicitly left RX-list callback undefined and was never admitted
in that state. Coordinator completed that callback, supplied all remaining
public hooks, and reviewed the production bodies against the entire raw member.
`bd19/tws_data_forwarding.c` now defines exactly the original 18 function
identities: nine public functions and nine private functions. Public const
`tws_acl_data_sync` matches channel6/attribute0, callback relocations, target
size28, section and align4. Private sync initializer/type/align4 matches the
40-byte target state. All 21 diagnostic constants match original bytes,
including SDK file provenance/line502 and assertion text. All 42 mapped type
shapes agree. No additional compiled private functions/globals were introduced.

Source review corrected initial worker abandonment gating, public event-handler
linkage, omitted exported hooks, callback-triggered count reloads and list next
pointer capture before wire writes. Coordinator RX classification preserves
full32-bit versus zero-extended16-bit hash matching, four-byte padded IDs,
ready/lost/send snapshots, abandonment limits, deferred last-two IDs, prefix
reuse, original head-offset reload after memset, reference increments, and
end-marker sequencing. Public RX preserves signed count consumption below
zero, prior-hash reload after logging, embedded packet metadata, cached LMP
versus fresh link fields, history-hook hash reload and signed byte sequence
comparisons. Original assertion/reset branches continue to the notification
hook if the platform helper returns; no application assertion policy was
substituted. Remaining normalized differences are equivalent typed/container
GEPs, control-flow/loop organization, pure scalar scheduling, prototypes and
compiler call/analysis annotations; this is not a formal equivalence proof.

Durable complete-mock fixtures cover channel/no-op/reset state, cleanup hash
width and count consumption, request wire layout/padding, callback reply length,
empty and later-ID matching, abandonment packets, prefix-backed in-place TX,
reference increments, end markers, both failed-allocation branches, receive
prefix release, history callback mutation, completion sequence cutoff, signed
single-bad timing/countdown, event27 lengths and logging-driven state reloads.
Safe backing storage is used for tested SDK span quirks. Standalone ASan/UBSan
passes without unresolved-symbol suppression. These fixtures do not cover every
queue interleaving, malformed span, DMA or runtime lifetime. C30/B57–B59 record
sharing, hash/framing, storage, reset and recovery follow-ups.

Completed BD19 target build passes. Normal BR23 portability compile is
unsupported (its event handler returns void, and its packet/state layouts
are different); the actual unchanged BR23 source set builds in full validation.
No BR23 source or later app fixes were imported. All 123 original BR23 C files
remain byte-identical to 44d0073. Worker full controller checks/census pass:
205 `: ok` reports, 12 legacy reports, 31 tooling tests, both layouts and 85
ownership groups. Totals: 86 accepted members / 949 public functions / 82 globals;
25 retained members / 1106 verified index entries. No application-image,
controller-radio or hardware validation claimed.

Data-forwarding combined checkpoint: acceptance `b2ad19c` passes canonical
controller checks/census with 205 `: ok` reports, 12 legacy reports, 31 tooling
tests, both layouts and 85 ownership groups. Totals: 86 accepted / 25 retained
members, 949 functions, 82 globals and 1106 retained index entries. Clean
worker/dependency status and merged ancestry checked; completed worktree and
branch deleted. Original BR23 sources unchanged. No pending worker or hardware
validation.

## Clean-branch BD19 pairing API occurrence-2 acceptance

Reviewed worker `6758a78`, `tws_pair.c.o` occurrence2, SHA-256
`65c6a1e91c93144bf28cebd5dd16c7c07909316150deb2a7efd95552527f76f1`.
Dedicated `bd19/tws_pair_api.c` supplies exactly ten public functions and three
private callbacks/helpers, no public globals. All 13 functions retain the SDK's
absence of code sections. Mutable six-byte wait_pair_addr, two initially-zero
byte handles aligned 4 and private 111 string align 1 match the original data.
39 mapped type shapes agree. The timer helper is always inlined and adds no
compiled definition. Every function was compared against occurrence-specific
raw IR. No unique BR23 public counterparts exist; incompatible page API
prototypes prevent compiling this extra source under the BR23 header.

During recovery coordinator corrected the private address interpretation
(14 30 e3 11 08 3a), code sections, handle alignment, byte-return busy-flag ABI,
failed-timer arming and retry termination. Subsequent review corrected the
existing-link wait-pair gate and removed a post-delay state reread: exhausted
wait-pair retains its captured retry decision even if the final delay changes
connection state. Remaining normalized differences are equivalent boolean
algebra, bounded-loop comparisons, scalar flag representation, rollback block
organization, private names, fixed-void versus old-style declarations and
compiler call annotations. Signed timeout multiply/divide and repeated quick
address calls remain unchanged.

Expanded fixtures cover all 36 combinations of link presence, role0/1/2,
monitor0/1/2 and retry exhaustion, exactly ten lookup/delay calls with no extra
lookup, state mutation on the final delay, distinct inquiry/page-scan/page
failures, duplicate cancellation disables, no-op eSCO cancellation, two quick
address values, negative timeout conversion, timer255 success without arming,
state before timer deletion, post-put timer overwrite, timeout callback event
arguments and search-timeout state retention. Complete-mock ASan/UBSan passes.
C31/B60–B61 record occurrence boundaries and API/timer lifecycle follow-ups;
corrected draft defects are not SDK quirks or application fixes.

Worker full controller checks/census pass: 206 `: ok` reports, 12 legacy reports,
31 tooling tests, both layouts and 86 ownership groups. Totals: 87 accepted
members / 959 public functions / 82 globals; 24 retained members / 1096 verified
index entries. Retained archive has exactly one tws_pair.c.o whose SHA-256
is the original occurrence1 hash 7b63d337…fee6; its core/callback/hash providers
remain untouched. All 123 original BR23 C files remain byte-identical to 44d0073.
The intended BD19 target and actual BR23 source set build. No application-image,
pairing-radio or hardware validation claimed.

Pairing API combined checkpoint: acceptance `fe73848` passes canonical
controller checks/census with 206 `: ok` reports, 12 legacy reports, 31 tooling
tests, both layouts and 86 ownership groups. Totals: 87 accepted / 24 retained
members, 959 functions, 82 globals and 1096 retained index entries. Clean
worker/dependency status and merged ancestry checked; completed worktree and
branch deleted. Original pairing occurrence1 and BR23 source bytes unchanged.
Correction to the acceptance commit report: the link/role/monitor/exhaustion
matrix contains 36 combinations (2*3*3*2), not 72; all 36 pass. No pending
worker or hardware validation.


## Clean-branch BD19 timed sync-call acceptance

Reviewed worker `07f43a9`, `tws_sync_call.c.o` occurrence1, SHA-256
`dbb5d6d9c680f99d0dced6a526672df01e2848eefef02bcddb1ee62ff988fef4`.
Dedicated `bd19/tws_sync_call.c` supplies five public functions, two public
globals and all 15 SDK function identities including private helpers. All 40
mapped type shapes agree; all four public/private initializers, sections and
alignments match exactly. sc_slot is mutable24/align4/timer255, channel9 is
const28/align4/attribute15, head is self-linked, format is 441-NUL/align1.
Only TX and combined RX/TX use .tws_irq_code; other routines use
.classic_tws_code except the three unsectioned helpers. No duplicate UUID API:
BR23 task source is outside BD19 ownership; retained BD19 task has no such export.

Coordinator guided correction of delay-before-probe sniff waits, wrapped timer
multiplication, callback/UUID snapshots, actual role-bit field and deletion
ordering, then expanded durable fixtures. Full raw-IR function review plus
normalized compiled review found remaining loop rotation, equivalent branches,
byte-load ordering without intervening side effects, GEP/cast representation,
nonobservable local lifetime changes, and compiler annotations. These are not
a formal equivalence proof. All 123 baseline BR23 C files match 44d0073.
Compiling this extra BD19-only source under BR23 fails because its ring record
lacks list/timer fields; the actual BR23 source set passes validation.

Fixtures cover short padded RX records, queue selection/no retransmission,
timeout/disconnect/taskq status and cleanup order, deletion-time UUID mutation,
callback-time timer mutation, timer255, no-link paths, allocation failure,
clock-time field mutation and captured link, signed negative delay, wrapping
multiply/cutoff, sniff wait probe counts/first and fifteenth delays, IRQ skip,
role suppression, and no-op init/TX-success. Complete-mock ASan/UBSan passes.
C32/B62–B63 record separate-protocol rationale, framing, lifetime and ignored
failure questions. H21 is inherited BR23 evidence, not this BD19 member's ABI.

Worker full controller checks and census pass: 207 : ok reports, 12 legacy
reports, 31 tooling tests, both layouts and 87 ownership groups. Totals: 88
accepted members / 964 functions / 84 globals; 23 retained members / 1089
verified archive index entries. No application images or hardware tested.


Timed sync-call combined checkpoint: acceptance `c8f149b` passes canonical
controller checks/census: 207 : ok reports, 12 legacy reports, 31 tooling
tests, both layouts, 87 ownership groups. Totals: 88 accepted / 23 retained
members, 964 functions, 84 globals, 1089 retained index entries. All 123 original
BR23 sources remain byte-identical to 44d0073. Clean worker and all three nested
dependency statuses and merged ancestry checked; completed worker worktree and
branch deleted. No pending worker, application-image or hardware validation.


## Clean-branch BD19 CTE math acceptance

Worker `74ed2d6` preserved a partial draft after accidental truncation; the
coordinator reconstructed missing functions from raw SDK IR and completed the
review. Member `cte_data_algorithm.c.o` occurrence1 SHA-256
`37f5c7b5972b53d764f03f1a91760f9f21afc6d098f2050f96f725b3f374f108`.
Dedicated source supplies all 16 function definitions (13 public, three private)
and six public tables. Weak put_float remains weak/no-op; all functions and data
retain the absence of sections. Public tables total2046 bytes: arctan257/sin128/
acos257 shorts align2, log81 words/m21 floats align4, cte_data354 bytes align1.
Private context pointer startsNULL/align4. Every initializer bit and all seven
string literals/alignments match compiled SDK IR. Primary context size388 and
field layout agree. Both target compilers build this source; actual BR23 files
are untouched (123 originals byte-identical to44d0073).

Raw function review and normalized compiled IR reviewed for endian conversion,
float/double operation order, signed divisions/truncations, explicit wrapping
pair sums, allocation lifecycle, and repeated global/context loads around calls.
Coordinator corrected draft endian/power/sign/interval errors, restored missing
functions and original log_print, cached allocation before the second malloc,
and reloaded global context after diagnostic logging. Corrected draft defects
are not SDK findings. IQ diagnostic SDK loads cross its declared76-element
arrays into adjacent context fields; two-byte aligned builtin copies preserve
the raw loads without introducing C subarray indexing UB. B64–B66 document
this behavior, strict lookup/NaN fallback, invalid-input math domains, unchecked
allocation/reinitialization/double-close and zero-signal division. C33 records
verified shared-table/helper candidates without changing BR23 ownership.

New tools/btctrler/check_cte_algorithm.py adapts original LLVM to x86_64 solely
for host comparison (debug/target attrs removed, definitions renamed and private
helpers exposed). It compares128611 calls exactly: all65536 sine inputs,
50000 randomized atan inputs plus axes, randomized CFO/frequency/angle/distance/
IQ, context and output bytes, diagnostic float values, short scalar boundaries,
NaN/infinite log ratios, and five full public conversion/angle pipelines.
It requires a host clang; used clang21.1.8 and gcc -O2 -ffp-contract=off.
This is additional evidence, not a formal or target FP/RF equivalence proof.

Durable host fixtures test byte decoding/0x8000 powers, terminal record rotation,
strict log endpoint and NaN row79, aliased complex outputs, zero-length paths,
SDK sample angle49/output sums9633/11085, stale close pointer and self-test.
Complete-mock ASan/UBSan passes. Full worker controller checks/census pass:
208 : ok reports,12 legacy reports,31 tooling tests,both layouts,88 groups.
Totals89 accepted / 22 retained members,977 public functions,90 globals and1070
verified retained index entries. No application-image or hardware validation.


CTE combined checkpoint: acceptance `44758f3` passes canonical controller
checks/census:208 : ok reports,12 legacy reports,31 tooling tests,both layouts,
88 ownership groups. Canonical host differential check also passes128611 exact
calls. Totals89 accepted /22 retained members,977 functions,90 globals,1070
verified index entries. All123 original BR23 C files match44d0073. Clean worker
and three dependency repositories and merged ancestry verified; completed
worktree and branch removed. No pending worker or hardware validation.


## Clean-branch BD19 media-sync acceptance

The one Luna worker stopped after inventory with no code/commit. Coordinator
completed the recovery in its prepared isolated branch from972cd09. Member
tws_media_sync.c.o occurrence1 SHA-256
`f69f075bc3e851ffc74e2db6a28116a991bc65c1fd8ee23d185c697adc78aa80`.
Dedicated source supplies14 public functions, two globals and all24 definitions.
Public aud_sync_head is self-linked/align4; const channel7/attribute7 is
.tws_sync_channel.7/align4. Private i1 nettime flag startsfalse/align4; volatile
peer-play byte starts0/align1. All four initializers match SDK exactly. All
functions retain .tws_media_sync_code except three unsectioned list helpers;
weak master-clock function remains weak.38 SDK type shapes agree; generated
layout checks cover52-byte messages and124-byte handle offsets4/108/116/120.

Every raw function and normalized target body reviewed. Preserved MMIO32 at
0x1c0000, frame volatile16 masks2048/4096/6144, nested IRQ wrappers, peer reloads
between clock calls, unsigned625 remainder/threshold4, no-op get_bt_slot_time/
bt_audio_tws_lat_time/sbc_sync_packet_len, mutable callback loads, post-callback
list advancement, two-slot indices, masks and memory intrinsics. Parent draft
list-delete reset was corrected to SDK self-linking, not NULL poisoning.
Remaining differences are loop/control organization, container-of GEPs proven
by layouts, equivalent boolean guards, private helper call versus inlining,
local lifetimes and compiler/prototype annotations. No formal equivalence claim.

Complete host fixtures plus ASan/UBSan pass. Expected values from SDK IR cover
allocation failure/config-off open/close, play-message bootstrap, full48-byte
queue/busy/wrap/ACK behavior, empty-list offline frame, multi-handle overwrite
and mark-all, callback-only pool clearing, output/index aliasing, unchecked RX
framing and negative forwarded length, callback replacement during IRQ/role
hooks, post-callback list traversal, offline notifications, selection fallback,
role/hardware masks, nested wrappers, zero timing hooks and remote peer reload.
Unwritten notification word is not read in fixtures. C34/B67–B69 track timing
and sharing boundaries, payload/multi-handle behavior and callback/IRQ lifetime.

BD19 target builds; extra-source BR23 compile fails because its frame lacks
miscntl_of_audsync. Actual BR23 set passes;123 original C files still match
44d0073 byte-for-byte. Full worker controller checks/census pass:209 : ok
reports,12 legacy reports,31 tooling tests,both layouts,89 ownership groups.
Totals90 accepted members/991 public functions/92 globals;21 retained members
and1054 verified index entries. No app-image, audio-peer or hardware validation.


Media-sync combined checkpoint: acceptance `e3f9bc5` passes canonical controller
checks/census:209 : ok reports,12 legacy reports,31 tooling tests,both layouts,
89 ownership groups. Totals90 accepted/21 retained members,991 functions,
92 globals,1054 verified index entries. All123 original BR23 C files match
44d0073. Clean worker and three nested dependency repositories and merged
ancestry verified; completed worktree and branch deleted. No pending worker,
application-image or hardware validation.


## Clean-branch BD19 data-transfer acceptance

Reviewed substantive worker checkpoint8bb8858, completed corrections and host
fixtures on its isolated branch. The worker commit's hash prose mistakenly
labels an incomplete hash as SHA1; authoritative member identity is
`tws_data_trans.c.o` occurrence1 SHA-256
`1fb4fb840fcfd13df5cbc712097bb660dece4c1a4fcdcb734a3e34ab8866bbcb`.
Dedicated source supplies all21 function identities,14 public functions and
one public global. Channel14/attribute2 is const/.tws_sync_channel.14/align4;
private command1413764179 stub is const/.tws_func_stub/align4 with exact
llvm.used retention. Private channel byte starts0/align1 and head self-links/
align4. All five initializer/section/alignment/retention records and eight
format/diagnostic strings match target SDK exactly.44 mapped type shapes agree.
The local stub type preserves this occurrence's u8 callback flag, rather than
the common header's bool typedef. No shared header changes were needed.

Coordinator corrections cover draft role/state and drop/hold inversions,
retry/return semantics, TX field offsets and hold bit, byte callback ABI,
allocator prototypes/volatile ticks, memory intrinsics/alignments, frame/list
prefix frees, unknown-channel outputs, signed/byte command arithmetic, cached
next around queue callbacks, post-unlock fetch length, and original diagnostics.
Corrected draft errors are not SDK defects. Every original raw function and
normalized target body reviewed; remaining differences are inlined lookup/loop
organization, layout-proven container GEPs, equivalent guards/width operations,
unused local array initialization, and compiler/prototype annotations. All
functions retain absence of sections. Target handle108,frame16,TX44,prefix28
and pool/entry/header offsets agree with generated layout assertions.

Focused fixtures with complete mocks and ASan/UBSan pass. They check ignored
semaphore errors, ID wrap/collision, open/start/stop/close failures, role1 close,
u8 command RX flags and zero-length wrap, TX field metadata/hold, nested IRQ,
INIT blocking, STOP/missing-peer paths, cached next across TX mutation,
completion/deferred free, RX retry/reclaim/protection/signed deadlines,
fetch-vs-pop unlock order, aliased check outputs, last-length versus sum,
post-event detach state overwrite and returning reset/assert hooks. Host RX
records are padded for wider intrusive links; target IR independently checks
exact wire copy lengths and alignment. C35/B70–B73 track ABI/sharing, command
and semaphore contracts, pool pressure, frame lifetimes and IRQ/callback issues.

BD19 target builds. Extra-source BR23 compile is unsupported: that header lacks
the transfer API/complete data structures. Actual BR23 set passes; all123
original C files match44d0073. Full worker controller checks/census pass:
210 : ok reports,12 legacy reports,31 tooling tests,both layouts,90 groups.
Totals91 accepted/1005 public functions/93 globals;20 retained members/1039
verified archive index entries. No application-image, peer-radio or hardware
validation; host and symbol/layout checks are not formal equivalence proof.


Data-transfer combined checkpoint: acceptance `ae99222` passes canonical
controller checks/census:210 : ok reports,12 legacy reports,31 tooling tests,
both layouts,90 groups. Totals91 accepted/20 retained members,1005 public
functions,93 globals,1039 verified index entries. All123 original BR23 C files
match44d0073. Clean worker and three dependency statuses and merged ancestry
verified; completed worktree/branch deleted. No pending worker, app-image or
hardware validation.

## Clean-branch BD19 local-media acceptance

Reviewed worker checkpoint `aa5571e`; completed corrections and focused host
fixtures on its isolated branch. Authority: `local_media_sync.c.o` occurrence1,
SHA-256 `db81f8037bcf4ed8c839cd224ea6732ec23cf7902578968adacbb23997377917`.
Dedicated `bd19/local_media_sync.c` supplies all35 function identities,25 public
functions (including weak void(void) notification) and one public channel table.
Channel12/attribute3 is const/.tws_sync_channel.12/align4. The private command
1280136259 stub uses exact void(u8*,u16,u8), const/.tws_func_stub/align4 and
llvm.used. All eleven global initializer/section/alignment/retention records
and seven strings match target SDK exactly;43 mapped type shapes agree.
All functions retain .tws_media_sync_code except four unsectioned helpers.
Target assertions pin frame44,head28,entry32,payload42; named offsets allow
host pointer widening without imposing host layout on the target wire format.

Coordinator corrections preserve volatile jiffies, deadline load order, input
length/pool reloads after allocator/free callbacks, list insertion/init order,
memset28/align4 and RX memcpy(len+4)/align2, byte-aligned sequence/timestamp
stores, wrapping allocation/length/total arithmetic, u16 diagnostic counters,
stop state-store order and uninitialized unused command bytes. Raw IR confirms
START len0 passes signed-1 (not65535), and diagnostic arguments are state2,
state1,other. These are not SDK bugs introduced by corrected draft translations.
Every raw function and normalized target body reviewed; remaining differences
are loop organization/container GEPs, equivalent guards, compiler annotations,
legacy unspecified-argument import declarations, stack lifetime placement and
explicit alignment assumptions matching the SDK's access alignments.

C36/B74–B76 record sharing/ABI boundaries, ignored semaphore/send failures,
unchecked command/copy assumptions and returning reset/assert continuation,
pressure/deadline/head blocking, frame lifetime and IRQ/callback ordering.
The worker's claimed matching weak hook is incorrect: BR23 has an incompatible
int(u8*,int,u8) boundary. No shared headers or BR23 sources changed. Extra-source
BR23 compilation is unsupported because its frame and API differ; the actual
BR23 source set builds, and all123 original C files match44d0073 byte-for-byte.
No application-image, peer-radio or hardware validation; ownership/layout and
host checks are not formal equivalence proof.

Focused fixture with complete mocks passes ASan/UBSan including leak detection.
Checks cover allocation sentinel/prefix and TX fields, ignored send failures,
head-only blocking versus ready totals/unfiltered next-fetch, deleted cleanup,
fetch/pop length timing across IRQ enable, cached next across queue mutation,
in-flight clear/completion, RX retries and signed deadlines, input length reload
after free, post-task/event state mutation order, zero-length START argument,
byte-aligned sequence/timestamp stores, length wrapping and returning reset/
assert hooks. Host RX/list records use padding for widened pointers; target IR
independently pins exact wire copies and alignments. Normal fixture compilation
has no unresolved-symbol suppression. Full worker checks/census pass:211 : ok
reports,12 legacy reports,31 tooling tests,both layouts,91 ownership groups.
Totals92 accepted/1030 unique public functions/94 globals;19 retained members/
1013 verified archive index entries.


Local-media combined checkpoint: acceptance `2f82bb1` passes canonical controller
checks/census:211 : ok reports,12 legacy reports,31 tooling tests,both layouts,
91 ownership groups. Totals92 accepted/19 retained members,1030 unique public
functions,94 globals,1013 verified archive index entries. All123 original BR23
C files match44d0073. Worker and three dependency statuses were clean; merged
ancestry verified. Completed worktree/branch deleted. No pending worker,
application-image or hardware validation.

## Clean-branch BD19 TWS sniff acceptance

Base `32174b1`. One Luna worker stopped without a usable recovery or commit;
coordinator replaced its BR23 copy with the full BD19 implementation in the
same isolated worktree. Authority: `tws_sniff.c.o` occurrence1 SHA-256
`acba8584e987aa21cfc59c5f800a7078e124e3201532756b79e3ae319a55226a`.
Dedicated source supplies all34 definitions,24 public functions and two globals:
zero byte sniff_test_tone/align1 and common zero tws_sniff/align4,36 target bytes.
Every body retains .classic_tws_code except unsectioned cpu_reset; the anchor
hook is weak. Both initializer/linkage/section/alignment records and all43
format/function-name strings match SDK exactly.38 mapped type shapes agree.

BR23 source/IR/census comparison shows20 changed instruction bodies,10 without
counterparts,3 boundary-only matches and one direct candidate (cpu_reset).
BD19 implements no-phone negotiation, TX requests, D updates and monitor
handoffs rather than BR23's stubs; headers have changed signatures/frame
registers. No BR23 or shared header changes. The extra-source BR23 build is
unsupported (19 errors including signatures and absent frame fields/helpers);
the actual BR23 source set builds. Original123 BR23 C files match44d0073.

Coordinator reviewed every raw body and target-normalized diff, preserving
unsigned32 wrap and remainders, signed deltas, signed byte wake offsets,
27-bit masking only at recurring stored wake, first-anchor repeat versus
recurring single-period adjustment and zero-delta boundary. Timer gets the
unmasked wake and snapshots its fine count before the clock helper. Role
snapshot precedes the weak anchor hook, while timer/offset/state fields reload
where SDK reloads them. Master logging gate is cached; slave frame pointers
are cached before callbacks; handoff fields use volatile16-bit accesses with
original ordering. Local byte-return alias preserves link_idle_task_enable_detect
SDK i8 ABI despite the generated header bool declaration. Remaining target
IR differences are equivalent guards/loop forms, reordering of ordinary loads
without intervening side effects, integer widening of nonnegative values,
stack lifetime/initializer lowering, legacy unspecified import prototypes,
and identical12-byte aligned clearing lowered with i32 rather than i64 length.
No strict whole-program equivalence or hardware correctness is claimed.

Focused fixture with complete mocks passes ASan/UBSan and leak detection,
without unresolved-symbol suppression. Covers zero/wrapped anchor deltas,
fine-count snapshot across escaped-pointer mutation, nested IRQ/no-phone
negotiation, phone gates, post-send pure-monitor reload, command fields,
three-retry limit, peer/no-peer master packet, timer callback handoffs,
volatile frame halfwords, changed T after preset hook, anchor retry/setup,
returning reset/assert hooks, signed/unsigned clock correction, exact12-byte
teardown, IRQ-state reload after idle hook, TX toggle/state gates. Host structs
widen pointers; original target IR/layout checks pin hardware accesses.
C37/B77–B79 record sharing, unchecked period/input/allocation, timer/LMP/frame
lifetimes, ignored sends and retry/reset contracts; inherited BR23 #27 now
has confirmed BD19 dereference evidence, with runtime reachability unresolved.
No application-image, peer-radio or hardware validation.

Full worker controller checks/census pass:212 : ok reports,12 legacy reports,
31 tooling tests,both layouts,92 ownership groups. Totals93 accepted members,
1054 unique public functions,96 globals;18 retained members,987 verified
archive index entries. Focused sanitizer exit0; baseline comparison123/123.


TWS-sniff combined checkpoint: acceptance `a03f305` passes canonical controller
checks/census:212 : ok reports,12 legacy reports,31 tooling tests,both layouts,
92 ownership groups. Totals93 accepted/18 retained members,1054 unique public
functions,96 globals,987 verified archive index entries. All123 original BR23
C files match44d0073. Worker and three dependency statuses were clean; merged
ancestry verified. Completed worktree/branch deleted. No pending worker,
application-image or hardware validation.

## Clean-branch BD19 TWS task acceptance

Reviewed worker `a4826d1` on base `b15970f`. Pinned `tws_task.c.o` occurrence1,
SHA-256 `e6645497c9bf55f0ce6e4d2ddafd0af97e067f033489d4d897c1245d6400ee46`.
Dedicated source supplies all41 identities,36 public functions and no public
globals. All functions .classic_tws_code except unsectioned cpu_reset and
cpu_in_irq; local-address hook remains weak. Sole private global quick-address
six bytes35:27:2a:ae:85:9c/align1 and all seven strings match exactly.
46 mapped type shapes agree. Low-latency export has exact i8 zeroext via local
assembly alias despite generated bool header. UUID remains owned by sync_call.

Coordinator corrected get_tws_state peer-link snapshot to follow connection
lookup, packet header halfword alignment4, and host-safe sizeof state clears
(target still196/80). Complete host fixture checks successful/error/full queue
posts, original count flags/result+80/pointer words, unbounded API retries,
40x5-tick detach/exit failure, dangling published semaphore, ignored semaphore
errors, dropped pair timeouts/callee identities, IRQ/internal role semantics,
LMP core state versus link state and lookup callback mutation, framing/channel18,
int-to-u16 length truncation/nested IRQ, noncanonical byte2 low-latency enable,
weak/address/channel/config APIs, exact startup order and queue13 dispatch,
internal cancel order/semaphore/delay10000 and key event value. Finite task
execution uses test-only longjmp; serialized32-bit pointer words are checked
as values and not reconstructed as native host pointers. Semaphores are opaque
mocks. ASan/UBSan and leak detection pass without unresolved-symbol suppression.

Every changed/raw body and normalized target body reviewed; reusable bodies
checked against existing baseline C and target boundary. Remaining differences
are loop/guard organization, equivalent16-bit arithmetic, layout-proven GEPs,
ordinary independent operation reordering, stack lifetimes, compiler annotations
and legacy unspecified-argument declarations. CPU ICFG assembly/mask255 matches.
No shared header or original BR23 source changed. Extra-source BR23 build is
unsupported due target API/layout differences; actual BR23 source set builds,
and all123 original C files remain byte-identical to44d0073. C38/B80–B81 track
sharing/byte ABI, semaphore lifetime and ignored errors, retry/timeout/payload
bounds and lifecycle/IRQ contracts. No app-image or hardware validation, and
host/layout/symbol checks are not formal whole-program equivalence proof.

Full worker checks/census pass:213 : ok reports,12 legacy reports,31 tooling
tests,both layouts,93 groups. Totals94 accepted/1090 public functions/96 globals;
17 retained members/951 verified index entries. Focused sanitizer exit0.


TWS-task combined checkpoint: acceptance `d870d68` passes canonical controller
checks/census:213 : ok reports,12 legacy reports,31 tooling tests,both layouts,
93 ownership groups. Totals94 accepted/17 retained members,1090 unique public
functions,96 globals,951 verified archive index entries. All123 original BR23
C files match44d0073. Worker and three dependency statuses were clean; merged
ancestry verified. Completed worktree/branch deleted. No pending worker at
this checkpoint; no application-image or hardware validation.

## Clean-branch BD19 HCI USB transport acceptance

Reviewed worker `2c7d5f9` on base `9b0f6d4`, completed corrections and host
fixtures in its isolated worktree. Pinned `h4_usb_interface.c.o` occurrence1,
SHA-256 `2af2757bc3d18de29f734545e3f36646419139eab2cad5c012a13634a8579b62`.
Dedicated source recovers all28 function identities; sole public function
hci_transport_usb_instance returns the private H2_MUSB transport. Public arrays
hci_encryption_change(3 bytes08/04/00) and smp_encryption_information(9 bytes)
retain .hci_interface_const/align1. All six globals, including private transport,
8244-byte zero handle/.hci_interface_bss/align4, packet-handler callback/
.hci_interface_data/align4 and six-byte ISO table/.hci_interface_const/align1,
match exact initializers/sections/linkage/alignment. All47 byte strings match.
Eleven mapped type shapes agree. All functions .hci_interface_code except
cpu_reset; USB IRQ retains interrupt attribute. Endpoint init is explicitly
noinline so all original helper/diagnostic boundaries remain reviewable.

Coordinator corrections include runtime IRQ declaration byte ABI, polling
until bit15 set, TX CSR17/18 and RX20/21, direction bit128, count base0x1e1808
and32-bit stride with original mixed volatile/nonvolatile stores; byte-aligned
setup/ACL halfwords; volatile32 atomic reads and csync ordering; single saved
volatile EP0 state,8-bit SCO delivered length, full multi-chunk blocking send
with per-chunk deadline, idle/error/timeout returns and clear-before-log order;
class/vendor dispatch and null callbacks, setup ACK/descriptor/feature edge
cases, SET_INTERFACE value reload after ACK and cached logging gate, cached
packet lengths and post-hook ring/handler reloads, and complete SDK logs.
Corrected draft errors are not SDK bugs. Every original body and normalized
compiled target body reviewed. Remaining IR differences are CFG/stack lifetime
organization, equivalent arithmetic/GEPs, independent ordinary-load ordering,
immutable const-log gate hoisting, noinline annotation, and compiler/legacy
prototype annotations. No formal whole-program equivalence claim.

Host fixture uses complete mocks, fake MMIO/count banks and a scripted indirect
register layer; the actual target handshake, volatile accesses, register order,
csync and interrupt attribute were checked against raw compiled IR separately.
Checks cover table/open/DMA pointers/close, readiness stub, single and exact-full
packet termination, multi-chunk draining/deadlines, timeout state, SCO ring wrap,
idle/unknown types, deferred CMD2->1->0 and ACL/SCO delivery/backpressure,
USB reset/descriptor/address/class/vendor/ISO setup, first-slot SCO TX indexing,
callback/diagnostic mutation with cached lengths/log gates, SET_INTERFACE input
reload after ACK,8-bit SCO length wrap and low-byte pending suspension test.
ASan/UBSan with leak detection passes without unresolved-symbol suppression.
These fixtures do not emulate USB electrical timing, DMA, or real interrupts.

Census15 matching helpers/13 differing bodies informs C39; no previous owned
C existed. Both BD19 and BR23 extra-source compiler builds pass, but the source
still selects BD19 addresses and is admitted only for BD19. Existing123 BR23
C files match44d0073; no shared header change or app fixes. C39/B82–B84 track
sharing, readiness/send/handshake/timeout contracts, setup/receive bounds/ring
policy, and likely first-slot TX SCO indexing defect. No app-image, physical
USB or hardware validation. Full worker checks/census pass:214 : ok reports,
12 legacy reports,31 tooling tests,both layouts,94 groups. Totals95 accepted,
1091 unique public functions,98 globals;16 retained members/948 verified
archive index entries.


HCI USB combined checkpoint: acceptance `6098c8f` passes canonical controller
checks/census:214 : ok reports,12 legacy reports,31 tooling tests,both layouts,
94 ownership groups. Totals95 accepted/16 retained members,1091 unique public
functions,98 globals,948 verified archive index entries. All123 original BR23
C files match44d0073. Worker and three dependency statuses were clean; merged
ancestry verified. Completed worktree/branch deleted. No pending worker at
this checkpoint; no application-image or hardware validation.

## Clean-branch BD19 TWS eSCO acceptance

Input worker63cf3eb on basef6f63d2 supplied only3 of24 public bodies, explicitly
incomplete. Coordinator completed the remaining21 public/six private bodies,
corrected unconditional state-store ordering in TX requests, and added complete
host mocks/fixtures. Source `bd19/tws_esco.c` owns occurrence1,
SHA-256 e80d3a6ecf04b38a51d5d02d507b17844c687600a8550b55e97fc7dfd9cd934c.
All30 SDK function identities present; exact24-public-function/zero-public-global
boundary checked. Weak get_esco_packet_dump returns120; all public bodies and
private get-data/time-offset callbacks retain .classic_tws_code.esco; four
ordinary private helpers are unsectioned as in SDK. Private callback-local
last-index initializer is byte255/align1, unsectioned. All38 string byte arrays
and alignments and37 mapped type shapes programmatically checked.

BD19 has116-byte relay allocation versus BR2380-byte relay; hw state includes
reg/frame pointers, two timer IDs, deferred start callbacks, timing and CRC/
transpond protocol. Source preserves partial initialization (including writes
before allocation assertion), null-dereference preconditions, unchecked RX/TX
allocations, exact14-byte PDU copy and original memory intrinsics. Byte slot/
state arithmetic,16-bit pending counter, signed rem/unsigned clock modulo,
IRQ disable/enable and32-bit MMIO at1c0020/1c001c/1c00bc, volatile16-bit frame
fields and32-bit engine fields were checked against original/compiled IR.
Offset callbacks must observe cached role decisions for both open/close; these
were corrected and mutation-tested. The late _lc null diagnostic uses an empty
register-identity asm to keep it from being removed after typed dereferences;
it emits no instructions or memory clobber. No SDK guard/failure policy added.

Read all original bodies and reviewed normalized compiled bodies. Remaining
IR differences are equivalent CFG/list-loop organization, stack lifetime,
ordinary independent load order, safe stronger inferred alignment on already
aligned PDU fields, arithmetic flags under byte-range invariants, legacy
prototype/call annotations and immutable assertion-config load placement;
no formal whole-program equivalence claim. All volatile operation counts/
widths agree; changed static call-site counts in queue/step functions are CFG
merges with the same runtime paths. Census8 differing/22 without unique BR23
counterpart; no already-owned common body was needlessly retranslated. BR23
open/close stay byte-identical to baseline. Extra-source BR23 compile was tried
and fails on absent BD19 relay fields/APIs; this dedicated source is admitted
only for BD19, and the actual BR23 source set builds unchanged.

Complete host fixture links without unresolved-symbol suppression and passes
ASan/UBSan with leak detection. Expected values derive from SDK state constants,
raw arithmetic/branches, register writes and callback ordering: repeated TX
requests, strict RX>6 and byte wrap, ACL countdown/status truncation/engine gate,
DSCO numeric example and cached log gate, monitor selection/bitoff publication,
queue negative-byte return and flag-only clearing, bounded/all suspend frees,
21/41 command marshaling and signed delay limits, fine timer/soft-bitoff rollover,
engine activation/late skip and idle-frame suppression, strict timeout equality,
uncleared heap open, exact hardware configuration and cleanup, saved role across
mutating offset callback, relay selection/resend marker/copy/sem and step codes.
This does not emulate DMA, real scheduling/IRQ interleavings or radio behavior.
C40/B85–B87 record consolidation, initialization/lifetime, queue/marker and
interval/timeout follow-ups. No shared header/build-rule change or app fixes.
No application image or hardware validation. Full worker command `make -j32
controller-objects controller-test test controller-layouts controller-tools-test
controller-audit controller-compare` passes:215 : ok reports,12 legacy reports,
31 tooling tests,both layouts,95 ownership groups. Totals96 accepted members,
1115 unique public functions,98 globals;15 retained/924 verified index entries.

TWS eSCO combined checkpoint: acceptance `b789c51` passes canonical controller
checks/census:215 : ok reports,12 legacy reports,31 tooling tests,both layouts,
95 ownership groups. Totals96 accepted/15 retained members,1115 unique public
functions,98 globals,924 verified archive index entries. All123 original BR23
C files match44d0073. Three dependency repositories were clean. Removed only
the worker untracked read-only external symlink (target preserved), verified
clean status/merged ancestry, then deleted completed worktree and branch.
No pending worker at this checkpoint; no application-image/hardware validation.

## Clean-branch BD19 LE direct-test DUT acceptance

Reviewed worker a57ba98 on base8f6dec6. Pinned direct_test_dut.c.o occurrence1,
SHA-256 3953f19207b93c50dd9c2e7dde1d4d24a067794de3d952eccff825a4c3d71f78.
Dedicated bd19/direct_test_dut.c accounts for all36 SDK bodies and exact14 public
functions/six public globals. Ten global records match initializer/linkage/
alignment/sections programmatically: dut_hdl,rx_cnt,rx_phy_s,payload_length,
hci_transport,le_mac_addr,ble_dut_ops,__ble_dut_ops,rx_param,tx_param. Three
common globals remain common under -fno-common; transport/callback table and
state stay private. MAC20:22:33:44:55:66. All57 literal byte arrays, including
channel-map bytes00/18/4e and diagnostic strings, match; implicit align1 equals
explicit align1 for the SDK puts strings. All36 mapped type shapes agree.
prestart_cnt_adjust retains .volatile_ram_code; both IRQs retain interrupt
attribute; remaining functions/globals are unsectioned as in SDK.

Coordinator restored SDK printf/puts assertions with exact file/line/text,
original memory intrinsics under -fno-builtin,64-bit inst clear/align4,
wrapping unsigned allocation products/selection with signed divisions,
removed BR23-only TXAHDR1/TXDHDR1 initialization writes, corrected RX header
toggle and nonvolatile status/header snapshots, preserved volatile16/32 access
widths and slot-base halfword alignment, added missing diagnostics, and checked
post-log handle reloads/cached byte values. Adaptive RX raw slot-pointer casts,
returning assertions, missing length guards, unbounded hardware poll and other
SDK quirks remain. No application fixes or shared header changes.

Inlined nine C organization helpers to expose original bodies for review.
Read original bodies and reviewed normalized compiled IR, including initializer
bytes, pointer offsets, signedness, memory/IRQ/MMIO ordering and callback ABI.
Remaining differences are CFG/loop organization, equivalent arithmetic/GEPs,
stack lifetime, independent ordinary-load placement, imported legacy prototype
annotations and compiler specialization of unused/private constant arguments.
Private varargs use target32-bit integer words as in SDK, with host-pointer/
va_end adaptations only for UNITTEST. The SDK restarts/returns varargs without
all va_end calls; target behavior is retained. Prestart optimizer removes an
unreachable adjustment branch under masked-input bounds; standalone isolated
raw SDK LLVM versus recovered C passes101000 edge/random arithmetic cases.
This is additional arithmetic evidence, not whole-program or IRQ equivalence.

Complete host mocks link without unresolved-symbol suppression. ASan/UBSan with
leak detection passes. Fixtures derive expectations from raw IR and cover
allocation offsets/IRQ addresses, channel maps, fixed-channel edges, PHY and
coded exact-multiple timing, TX interval thresholds, -15 prestart/wrap, adaptive
RX status/count/window behavior, slot-pointer view accesses, exit clear/free,
callback mode init, HCI registration/reset/end/vendor event bytes, two-wire
length bitfields/RX/TX/end replies, untouched TX header1 initialization and
handle replacement during PHY diagnostics. Hardware timing/DMA are not emulated.
C41/B88–B90 track sharing, lifetime/validation, raw pointer-view concerns and
factory protocol. Existing123 BR23 sources unchanged from44d0073. Extra-source
BR23 compile was attempted and fails on osc_set and typed handler differences;
the actual BR23 source set still builds. No firmware-image or hardware test.

Validation: make -j32 controller-objects controller-test test controller-layouts
controller-tools-test controller-audit controller-compare; focused sanitizer
fixture; python3 tools/btctrler/check_dut_prestart.py --clang HOST_CLANG.

Full worker checks pass:216 : ok reports,12 legacy reports,31 tooling tests,
both layouts,96 ownership groups. Totals97 accepted/1129 unique public
functions/104 globals;14 retained members/904 verified archive index entries.

LE DUT combined checkpoint: acceptance `14c6ebd` passes canonical full controller
checks/census:216 : ok reports,12 legacy reports,31 tooling tests,both layouts,
96 ownership groups. Totals97 accepted/14 retained members,1129 unique public
functions,104 globals,904 verified archive index entries. Canonical arithmetic
oracle101000 cases pass; focused sanitizer passed in worker. All123 original
BR23 C files match44d0073. Worker and three dependency statuses clean, merged
ancestry verified, completed worktree/branch deleted. No pending worker at this
checkpoint. No application-image/hardware validation.

## Clean-branch BD19 TWS link acceptance

Base edc95f7; completed the Luna worker's uncommitted partial seed and four
scratch callback recoveries. Pinned tws_link.c.o occurrence1, SHA-256
 da807d9e86131c06657e4979814ee94ddf70673e456c92a3da142e124ff16f16.
Dedicated bd19/tws_link.c supplies all55 bodies,42 public functions and five
public globals. Original BR23 C remains unchanged. Census2 reuse/2 boundary-only/
15 differing/36 without a unique BR23 counterpart; existing BR23 C is a subset
with different monitor/frame/core/relay behavior, so it cannot replace this
member by field renaming. No application-branch changes were imported.

Public globals tws_link (common/80 bytes/alignment4), g_soft_agc_enable=1,
slave_rx_switch_irq=0, tws_link_sync and tws_link_task_ops exactly match SDK
initializers/linkage/alignment/sections after type mapping. Sync channel1,
attribute12 and callback slots are preserved. Sync descriptor .tws_sync_channel.1,
task ops .classic_tws_const; TX sync/RX packet/resume/suspend .tws_irq_code;
remaining monitor APIs and callbacks .classic_tws_code, ordinary private helpers
unsectioned. All55 compiled identities/sections, all23 literal byte arrays and
41 mapped type shapes checked against original SDK IR. No extra private bodies.

Coordinator corrected reversed monitor-disable role selection, forward-bitoff
zero extension versus normalized signed16 arithmetic, full16-bit header.check,
missing TX wait/retry return36, timeout count reload after clock callback,
AFH-end reload after comparison callback and peer reload after offset callbacks.
The may_alias32 wire type retains SDK staged stores between volatile16 frame
reads. Retained explicit memory intrinsics under -fno-builtin, common state
under -fno-common, exact assertions/provenance and all original unchecked paths.
The full recovery includes36-byte sync,68-byte core serialization, monitor/eSCO
setup, role switching, split clocks, packet assembly, timers and pending-list
movement. Ordinary-load ordering, equivalent GEP/arithmetic, loop/CFG layout,
stack lifetimes, compiler call attributes and calls versus inlined public
helpers account for remaining normalized IR differences. Copies preserve SDK
alignment contracts; some compiler-known alignment or conservative memcpy
alignment differs without changing the source pointer contract. No target
runtime helper replaces a required SDK body.

Focused fixture links with complete mocks (no unresolved-symbol suppression).
It covers signed/wrapped halfwords, disable-role mapping and enable no-op,
monitor/eSCO constants and transitions, IRQ balance, sequence flag2 unshifted,
monitor timer callbacks, hash/retry/5th-eSCO-request sync, full16/low8 timeout
split, post-clock and post-comparison reloads,16-bit check rejection, cached
packet header after scheduler mutation, complete/fragmented/probe-no-space/
allocation-failure/oversized packet paths, task early return/resume/suspend,
pending-list destination replacement and cached log gate, post-log bitoff
reload,68-byte core data/untouched tail,premaxval windows,receive-switch clock
copy and allocation/start/detach. ASan/UBSan with leak detection passes.

New check_tws_bitoff.py isolates the original raw LLVM body, rebases only its
two frame GEPs to input halfwords, and compares recovered C arithmetic on493216
cases (all65536 first inputs with six boundary second values plus100000 seeded
random pairs). All pass. This verifies arithmetic, not radio/IRQ scheduling.
Extra-source BR23 compilation fails on BD19-only frame fields as expected;
the actual BR23 source set builds. C42/B91–B93 track consolidation, unchecked
wire/lifetime contracts, timeout widths, packet capacity/ownership and no-ops.

Validation: make -j32 controller-objects controller-test test controller-layouts
controller-tools-test controller-audit controller-compare; focused complete-mock
sanitizer fixture; python3 tools/btctrler/check_tws_bitoff.py --clang HOST_CLANG.
Worker checks pass:217 : ok reports,12 legacy reports,31 tooling tests,both
layouts,97 ownership groups. Totals98 accepted members/1171 unique public
functions/109 globals;13 retained members/857 verified archive index entries.
No firmware-image, flashing or hardware validation.

TWS link combined checkpoint: acceptance `af13547` passes canonical full
controller checks/census:217 : ok reports,12 legacy reports,31 tooling tests,
both layouts,97 ownership groups. Totals98 accepted/13 retained members,
1171 unique public functions,109 globals,857 verified archive index entries.
Canonical raw-LLVM bitoff oracle493216 cases pass; complete-mock sanitizer
passed in worker. All123 original BR23 sources match44d0073. Worker clean and
merged ancestry verified; completed checkout/branch deleted after unlinking
only its external symlink (dependency targets preserved). SDK and btstack
nested repositories are clean; toolchain is an ordinary read-only input
directory inside the unrelated primary checkout, not a nested repository.
Primary changes were left untouched. No pending worker at this checkpoint.
No firmware-image or hardware validation.

## Clean-branch BD19 analog acceptance

Base520bd48; completed the Luna worker's uncommitted BR23 scaffold and its
three later private calibration helpers in the same isolated worker checkout.
Pinned analog.c.o occurrence1, SHA-256
949467efd69ef9689f9430f049c02b75439f627f751ee4474bb3dc4e0674205f.
Dedicated bd19/analog.c supplies all65 bodies,58 public functions/12 public
globals, with no extra private bodies. Census9 matching/37 differing/19 without
unique BR23 counterpart. Reused matching oscillator/accessor, modem and RX
counter bodies, retaining the clean BR23 source unchanged. Target-local recovery
handles changed PLL/calibration/MMIO/ABI rather than importing application fixes.

Exact21 non-string global records match initializer bytes/linkage/sections/
alignment, including the two83-entry PLL arrays and11-entry normal/FCC versus
7-entry BQB power arrays. All93 literal byte arrays (92 diagnostics plus the
PLL integer array) and five mapped type shapes match. Public globals:
TXPWR_table,TXPWR_table_bqb,TXPWR_table_fcc,TXSET_table,cap_freq_r,
config_mesh_certification,trim_info,ldo_trim_res,lna_g,dm_g,bp_g,tsel.
Packed trim_info/ldo_trim_res alignment1,cap_freq_r alignment2; data/const/bss
sections and all65 function .bt_rf_code sections match. Private RX counters
retain original dotted assembler names. Mutable single-carrier configuration
retains the SDK global declaration; VBG trim returns16 bits.

Coordinator completed direct FIFO PLL configuration/single-carrier adjustment,
full modem/analog/RF setup, bank scans/status, temperature refresh, gain tables,
ADC clock selection, LDO setters/scans/application and VCO energy measurement.
Worker calibration is expressed with labels/width-checked PHIs to retain the
full SDK loops,37-byte cache/version4098 and24+13-byte serialization. Corrected
missing diagnostics, narrow signed extensions, hardware constants/table entry,
private/public boundaries, missing receive-init tail, duplicate tail RMWs and
sample reads moved across delays by the pseudo-C reading aid, and a signed
negative left shift in the RX DC recurrence (now an explicit unsigned bitvector
shift). Raw IR is the
authority. Preserved self-RMWs, three separate status reads, packed record
access, original memory intrinsics, byte/halfword wrap, logging gates, callbacks,
cache length truncation, unchecked divisions/indexing and no-timeout waits.
set_bt_trim_mode is noinline to retain its original independently reviewable
private body; other compiled differences are CFG/loop organization, ordinary
loads and pure expression order, equivalent widening/narrowing, stack lifetimes,
call annotations and calls to verified public helpers instead of inlining them.

The RX DC recurrence has an original LLVM undef incoming value; C concretizes
it as0, an allowed choice, not a claim about an undocumented hardware value.
C43/B94–B97 retain this uncertainty plus consolidation, persistent-record/ADC/
IRQ contracts, bank/AGC bounds, no-op features and factory-mode observations.
No BR23/application fixes or shared header/build changes were introduced.

New tools/btctrler/check_analog.py and analog_trace.c compare original SDK LLVM
with the recovered production q32s object after isolated native lowering.
The adaptation removes target layout/attributes/debug metadata, exposes private
entry points and replaces every volatile32 MMIO load/store (including SSA-
selected RX trim pointers) with bounds-checked ordered software model calls.
The model provides ADC/DMA completion, monotonic PLL response and bounded signed
samples; it normalizes the DMA stack-buffer address and leaves the explicitly
zeroed sample buffer as a zero-input model. No physical MMIO occurs. Fingerprints
and event counts cover ordered loads/stores, delays, external callbacks, logs,
ABI-width returns and final RF/trim/cap state. All730 executions pass:65 bodies
under10 configuration/sample scenarios plus80 argument edges. Both processes
must exit0; matching crashes/timeouts are failures. Earlier exploratory model
results were discarded after correcting register extent/completion/pointer
modeling. The successful strict run also caught/fixed receive-tail duplication
and raw-load/delay ordering. This sampled model is not proof of all paths,
undefined inputs, RF calibration, delay-loop duration, DMA timing or IRQ safety.

Validation: make -j32 controller-objects controller-test test controller-layouts
controller-tools-test controller-audit controller-compare; python3
 tools/btctrler/check_analog.py --clang HOST_CLANG. All pass:217 : ok reports,
12 legacy reports,31 tooling tests,both layouts,98 ownership groups.
Totals99 accepted members/1229 unique public functions/121 globals;
12 retained members/787 verified archive index entries. All123 original BR23
root sources match44d0073 byte for byte. Extra-source BR23 compile fails on
BD19 btagc ABI/table sizes and target fields, as expected; actual BR23 source
set builds. No application-image, flashing or hardware validation.

Analog combined checkpoint: acceptance `d1b1b9e` passes canonical full
controller checks/census:217 : ok reports,12 legacy reports,31 tooling tests,
both layouts,98 ownership groups. Totals99 accepted/12 retained members,
1229 unique public functions,121 globals,787 verified archive index entries.
Canonical strict modeled comparison passes730 executions; initializer/literal/
section/type/boundary checks pass again on the integration object. All123
original BR23 root sources match44d0073. Worker clean and ancestor tip verified;
SDK/btstack nested repositories clean,toolchain directory has no own .git.
Removed only the worker external symlink (target preserved),then deleted the
completed worktree and branch. No pending worker at this checkpoint.
No application-image, flashing or hardware validation.

## Clean-branch BD19 BLE5 radio acceptance

Review input: Luna worker checkpoint 7eb8aef, based on integration 6bbdd96.
SDK RF_ble5.c.o occurrence1, SHA256
5d861cc0756d0c9d2522ac27e1d497f9d583dda9382eb138cafd0977388aad59.
Manifest source: src/btctrler/bd19/RF_ble5.c. Whole-member boundary accepted
only after coordinator corrections; the initial checkpoint was not accepted.

Public definitions (21): __ble5_hw_anchor_cnt_timeout, __ble5_hw_event_process, __ble5_hw_rx_process, __hw5_free_slots, __master_reset_transmitWindowDelay, __reset_connection_winsize, __set_ble_hw_timeout_irq_init, __set_ext_aux_event_hold, __set_ext_mode_enable, __set_extended_conn_dead_times, __set_extended_empty_tx_data, __set_extended_tx_data, __slave_reset_widen, assign_aux_data_channel, ble5_master_primary_event_end_handle, ble_ext_primary_scan_preprocess, get_if_exist_ble5, record_rx_chsel_algorithm, repair_algorithm2_hop, set_hw_channel_selection_algorithm, set_tx_pdu_channel_selection_algorithm.
Sole public global: __ble5_ops, .ble_rf_data align4, points to ble5_ops.
Private globals: aux_filter zero-initialized42 bytes .ble_rf_bss align2;
aux_data_channel byte255 .ble_rf_data align1; ten-entry ble5_ops operation
callback table .ble_rf_const align4; ext_header_table12 bytes .ble_rf_const
align1. All five records match SDK initializer/linkage/section/alignment after
checked type mapping. All77 literal byte arrays match. Sixty transitive named
type shapes map without mismatch. All80 SDK bodies are accounted for:79
compiled definitions and __ext_hw_enable inlined into its two variable-enable
advertising calls. No extra private compiled definitions, no llvm.used or
llvm.compiler.used roots. Hardware code .ble_rf_code; generic list helpers
and cpu_reset use ordinary text (explicit .text and default text equivalent).
The compiled public function/global boundary is exact.

Reuse census:10 reuse candidates,26 same-instruction boundary checks,44
changed instruction bodies. Root BR23 C was the starting implementation;
checked BD19 field layouts and original raw IR govern the target recovery.
All123 original BR23 C files remain byte-identical to44d0073. No later app
fixes were imported. BR23-only all-event-hold and instant-validity exports
are omitted rather than leaking into BD19 ownership. Dedicated source keeps
BD1916-byte extended RX records/data16/status15/RSSI12 and32-bit ana_index15
initialization separate from BR23 size20 and its gain register initialization.

Coordinator corrections and raw-IR evidence:
- RX process now samples ble_analog_get_rssi after header/status stores, using
  an assembly alias for the SDK consumer's int-returning BLE5-frame ABI and
  retaining the low byte. Correlated hardware follows both loads through
  hw->ble5_ext->correlated_hw; the baseline C temporary was one level short.
- Event soft-state15 (not16) tests EXT_AUXCTL1 bit4096, conditionally sets
  anchor10 bit1024, then reloads the hardware frame before scan-state access.
  A review checkpoint described this as state16/AUXCTL0; raw GEP field34 is
  authoritative and those descriptions were corrected here. AuxPtr selection
  snapshots the packet pointer before random callbacks and retains packed
  i24 read/modify/write. Secondary reset preserves cached/reloaded hardware
  pointers around BLE_TO_PHY/primary-switch/PHY_TO_BLE callbacks and the gap
  read before conversion. Ring cursors/base are reloaded across byte copies.
- repair_algorithm2_hop reverses bits within each byte in three permutation
  rounds; the copied baseline reversed all16 bits and changed channel repair.
  Corrected byte-position arithmetic and preserved wrapping multiply/add,
  map/load order and conditional channel-bit removal. Aux-hop shift truncates
  to8 bits before packing, as SDK. Coded-S and dead-time helpers read volatile
  words before dependent RAM values, matching raw operation order.
- SyncInfo builder snapshots interval/AA/CRC/channel map before destination
  writes; packet diagnostics reload the built length after list insertion.
  Chain builder reloads hardware after each callback-bearing PDU construction.
  TX node traversal and halfwords preserve align1 loads via may_alias types.
- Original memory intrinsics use __builtin_memcpy/memset under -fno-builtin,
  retaining under-aligned copies. le_ext_hw_open reproduces config_asser,
  original file/line257 diagnostics, cpu_assert_debug versus reset, and normal
  continuation; no generic assertion-policy replacement remains here.
  All other BD19 path/line diagnostics match. ioctrl11 calls hw_dma_empty
  before fetching its output-pointer vararg. Extra C va_end is target no-op.
- Removed inherited forced linker-retention annotations on private list
  helpers; no SDK retention roots existed. Extra factoring helpers inline to
  make review of target operations direct; original __ext_hw_enable's branch
  remains semantically accounted for in its callers.

Behavior and follow-ups:
C44 records sharing boundaries and the untouched BR23 baseline's possible
recovery omissions. B98 confirms rejected ring-write zeroing, total_len load
before null check, equal DMA cursor full-wrap accounting and unvalidated RX
length/availability. B99 records unchecked allocation/PHY selection and queued
probe-callback lifetime contracts. B100 records conditional channel deletion,
state15 register behavior and exported no-op anchor timeout. These remain
SDK behavior; translation mistakes corrected above are not SDK defects.
Inherited observation37 is BR23-only: the function is absent from BD19.
Observation38 is confirmed on BD19. No hardware/IRQ/DMA concurrency proof.

Validation on this worker:
PATH=/work/bin:$PATH make -j32 controller-objects controller-test test
controller-layouts controller-tools-test controller-audit controller-compare
exits0:218 ': ok' reports,12 legacy reports,31 tooling tests,both layouts,
99 ownership groups, retained member bytes/order/index checked. Totals100
accepted members,1250 unique public functions,122 public globals;11 retained
members and765 verified archive index entries. Both real target source sets
build. Extra BD19 source under BR23 rejects ana_index as expected; the actual
BR23 source remains unchanged and builds.

Strict focused fixture uses gcc -O2 -DUNITTEST -DCONFIG_CPU_BD19 -iquote src
-ffunction-sections -fdata-sections -Wl,--gc-sections -no-pie source -lm,
without unresolved-symbol suppression: pass. Complete typed host mocks are
included only under UNITTEST; unsupported hardware calls abort. Tests include
ring wrap/rejected writes, PHY/header/table arithmetic, callback-sensitive
channel map writes, state15 AUXCTL1-versus-AUXCTL0 and hardware replacement,
and a real ring packet routing to correlated hardware with RSSI -42. Expected
values/order come from original SDK IR. Clang21 wrapper -O1 with address and
undefined sanitizers and ASAN_OPTIONS=detect_leaks=1: pass.

python3 tools/btctrler/check_ble5_hop.py --clang HOST_CLANG passes296608
cases: every16-bit event count under three addresses plus100000 seeded random
frame/channel-map vectors. It extracts original SDK LLVM arithmetic, rebases
nine frame/shadow field paths to nine halfwords, and compares every output
halfword with the extracted production C. Features/algorithm-enabled guard
inputs are fixed; this is arithmetic evidence, not RF/timing equivalence.
Global/literal/type/section/compiled-boundary inspection passes. Artifacts:
/work/analysis/btctrler-bd19/rf-ble5-review (ephemeral); this message and the
checked-in fixture/checker/revisit notes are the durable report. No app image,
flashing or hardware validation. Canonical merge/check/cleanup still required.

RF_ble5 combined checkpoint: acceptance `0218857` (worker checkpoint
`7eb8aef`) is merged by fast-forward and passes canonical controller checks:
218 : ok reports,12 legacy reports,31 tooling tests,both layouts,99 ownership
groups. Totals100 accepted members/11 retained,1250 unique public functions,
122 public globals,765 verified retained index entries. Canonical SDK LLVM
channel repair comparison passes296608 cases; all77 literal arrays,five
global records,60 type mappings,sections and public boundary pass inspection.
All123 original BR23 sources remain byte-identical to44d0073.
Worker tip ancestry and clean tracked state checked; SDK/btstack nested repos
clean; toolchain has no own .git. Removed only the external symlink, preserving
its dependency target, then deleted the completed worktree and branch.
No pending worker, app-image, flashing or hardware validation.
