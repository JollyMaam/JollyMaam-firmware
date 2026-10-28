# BD19 controller follow-ups

Migration scope: this list was imported from `btctrler/bd19-support` at
`7cae22b`. References to BR23 application fixes describe that app branch,
not necessarily the clean `44d0073` BR23 sources here. See
[vanilla migration](../../tools/btctrler/vanilla-migration.md) before using
a sharing/fix recommendation. IDs and original evidence remain intact.

This is the post-decompilation work list, not authorization to change SDK
behavior during ownership. Keep stable IDs. Mark resolved items with the
fix/decision commit; do not silently remove them. “Confirmed” means the
specified code/IR behavior was checked, not that its hardware consequence or
reachability has been demonstrated. “Candidate” needs a decision or proof.

## History audited

Backfill through `2b42237` plus BIS-slave acceptance `397c1a4` (worker `daf9867`):
all 91 commits unique to `btctrler/bd19-support` relative to main at that
checkpoint; all numbered BR23 recovery reports on `btctrler-decompiled`
(through #90 / `44d0073`), including reviewer corrections; the current
[review notes](../../../../tools/btctrler/review-notes.md); all 41 entries in
[the BR23 revisit list](revisit.md); and controller-path histories across
branches/renames. Also recovered the final reports in the two recent
`~/.codex-deepseek/sessions/2026/10/{26,27}/` sessions (LMP/HCI/task/power/
device/thread/FCC-DUT and ISO/BIS-slave). Those reports are leads, not acceptance:
FCC/DUT acceptance was withdrawn; FCC was subsequently recovered, DUT was not.
Earlier thread reports also understated the undefined-timeout problem.

Incremental review `2d79b41` accepts CIS master/slave after correcting workers
`41b2680` and `671de02`. Its commit body carries the full review and validation
report; this adds C14, B19–B23, R10 and the two member-coverage rows below.

This is an audit of recovery history and findings, not a new instruction-by-
instruction verification of every original object. The member coverage table
below records the accepted scope. Unaccepted members remain SDK code. Older
BR23 observations are explicitly inherited until a BD19 IR check establishes
them; old claims that a feature is discarded or unreachable must be rechecked
against the eventual Nova 2 configuration and linked image.

## Consolidation and deliberate application changes

| ID | Candidate and evidence | Follow-up / current boundary |
| --- | --- | --- |
| C01 | Shared named-field C already handles most layout differences. Census `8610b9c`, accepted no-source-change batch `7c790da`, grouped duplicate misc exports `99b9718`. | Use `controller-compare` for remaining members; check transitive types, globals, declarations and current app changes. Different SSA/loop shape alone needs no separate C. Do not duplicate already shared code. |
| C02 | Raw-byte DTM callers once had chip conditionals; `07fc0dc` already replaced them with one assembly alias. | **Resolved example.** Reuse this approach only for identical actual call ABIs. Callee header struct names alone are not a behavioral difference. |
| C03 | Byte/bool and enum ABI aliases remain in IRQ status, extended scan, idle detection, role-switch support, HCI emitters and periodic-sync calls (`c36289d`, `fe5a38b`, `eda4915`, `0fc29e3`, `fb6d1ac`). | Consolidate header declarations and definitions after both caller/callee recoveries. Preserve `i8 zeroext` versus `i1`; never delete aliases solely because return values currently look boolean. Weak LE key hooks have genuinely inconsistent call signatures; see B08. |
| C04 | BD19 selects compiler memory intrinsics where old BR23 C calls memory functions (`3dcf906`, `5752c05`, `4c462fe`). | Candidate common memory wrappers after checking overlap, width/alignment, builtin lowering and interposition. Named fields/`offsetof` can already share RX 24-byte and TX 28-byte headers. Do not equate `memcpy` and `memmove`. |
| C05 | Original SDKs cache advertising HW identically, but BR23 owned `ll_ext_adv.c` reloads across callbacks; BD19 preserves cached-then-reloaded boundaries (`4c462fe`). | Decide whether BR23's callback mutation behavior is deliberate; one implementation may follow after testing both targets. Shared RAM is not proof that callbacks cannot change it. |
| C06 | Width/sign differences in `btcontroller_util.c` store16 position wrap, link-bulk capacities, ACL TX comparison and periodic timeout multiplication (`93368e2`, `5752c05`, `bc6847b`, `0a6545e`). | Candidate normalization only after choosing the desired overflow contract. These differences are observable at boundary values, not cosmetic types. Preserve current tests for 65535 wrap, 40000 capacity and 6554 timeout. |
| C07 | BD19 retains SDK unchecked name/feature/message paths; BR23 has bounded inputs, null/error guards and task failure returns (`1e21c06`, `bc6847b`). | Port selected robustness fixes as explicit app changes, then share implementations where contracts agree. See B01–B04. Avoid spreading faithful unsafe SDK paths back onto BR23. |
| C08 | BR23 fixes thread minimum timeout, LL_VERSION_IND gating, queued role switching, page timeout/role recovery and air-RX hooks; BD19 deliberately omits some (`b4bd68a`, `3dcf906`, `1e21c06`). | Audit each fix's dependencies and applicability on Nova 2; add it separately from ownership. Thread minimum is an especially clear follow-up. Anchor/QoS hooks need BD19 timing integration. Page/role fixes also depend on members still retained. |
| C09 | BR23 private-version CRC retains low-byte behavior; BD19 SDK writes both CRC bytes (`1f4d062`, `0fc29e3`). | Determine intended wire format and caller expectations before sharing. This is an app divergence, not a harmless struct difference. |
| C10 | Link idle constants genuinely differ: BD19 four-slot lead/period and period-10, BR23 eight-slot lead and period-14 (`eda4915`). | Keep chip parameters, potentially in a small target constants block. Unify algorithms, not unexplained timing values; hardware validation is required before changing them. |
| C11 | IRQ address, AGC tables, RF/Classic/LE power operations, slot timers and FCC have actual hardware/table/ABI differences (`c36289d`, `bf0f833`, `e8d02e0`, `f9aa10c`, `0b5449a`, `99413ce`). | Share verified pure helpers if useful; retain target MMIO, volatile widths/order, callback tables, timer counts and retention roots. Six-entry BD19 versus nine-entry BR23 FCC packet tables are consequential. DUT was recovered afresh in `9140e3e`; its hardware implementation remains target-specific. PRBS/copy helpers are a later sharing candidate only after call/width/order contracts agree. |
| C12 | Controller task source split exports an internal entry on BR23 while BD19 keeps it with lifecycle; low-power targets differ only in initializer shape (`a59c62b`, `431d5c0`). | Later structural cleanup can share private declarations and designated initializers, provided exact external symbol groups, section/retention roots and internal linkage still audit. HCI acceptance (worker `843223a`) retains `bd19/assert.c` as a shared support unit: its app assertion policy intentionally replaces SDK diagnostic strings/private reset helpers and is not an original member export. Consolidate only if the support ownership/build boundary is deliberately changed. |
| C13 | Newly recovered BIS-slave lives in `bd19/ll_bis_slave.c`; the worker's root-level file could have joined BR23's wildcard source list despite differing SDK validation/timing. | Keep BD19-only until a full BR23 occurrence/layout review. Two simple exports match after type renaming; that does not establish whole-member equivalence. |

| C14 | CIS master/slave `2d79b41`: PHY conversion helpers and much original SDK logic agree across targets, but no prior BR23 C owner exists. | New files stay BD19-only. Consider sharing pure PHY/packet helpers after both target ownership boundaries exist; preserve byte emitter ABI, distinct handler gates and callback snapshots. |

| C15 | `bd19/h4_uart_hw.c`, worker `6e03c7a`: IRQ/close original bodies match BR23 SDK; transmit has equivalent loop shaping. BD19 init instead uses weak pins 49/50 and distinct enable writes. | Consider a common UART core when BR23's source ownership exists; keep routing/init constants and actual weak override ABI until verified on both targets. |

| C16 | `bd19/audio_codec.c`, worker `b4d0de8`: original BR23/BD19 operations, types and constants agree; StartStream/context lookup differ by loop shaping. No prior BR23 C owner exists. | Candidate single common source after reviewing BR23 ownership/selection. Keep public common scratch linkage and named SDK layouts; a matching stub is not proof of audio capability. |

| C17 | `bd19/h4_uart_interface_loop.c`, worker `e0df9fa`: 18/20 original functions agree with BR23 SDK; two parser routines differ in CFG. No prior BR23 C owner. `__data_empty` uses an explicit byte-return asm alias because the generated header says C bool. | Candidate common ring/transport implementation after both boundaries are reviewed. Consolidate byte/bool declarations through C03 while preserving the actual zeroext i8 export and callback table ABI. |

| C18 | `bd19/h4_controller_interface.c`, worker `cab02d3`: all seven original SDK function bodies agree across chips. Current BR23 app owner `hci_transport.c` combines this member with HCI controller, omits lifecycle logs/public flow pointer and uses uppercase `H4_CONTROLLER`; BD19 preserves SDK `H4_Controller`. | Split/share the tiny transport after reviewing the combined HCI controller ownership boundary. Keep explicit target app-name/log/global choices until deliberately reconciled; do not accept the whole combined file merely to avoid this small duplicate. |

| C19 | `bd19/hci_controller.c`, worker `843223a`: SDK lifecycle/dispatcher largely agree across targets, but BR23 app `hci_transport.c`/`hci_dispatch.c` combine members, add input guards and change queue/formatter behavior. | Share verified packet/event helpers after choosing bounds, allocation and lifecycle contracts. Preserve BR23 app fixes; common source must retain exact public/internal ownership and packed event offsets. Production app configuration is const: target optimization may cache mode across callbacks although mutable host fixtures preserve SDK reload boundaries. |

| C20 | `bd19/lmp_hci_cmd_parser.c` / `lmp_hci_baseband.c`, SDK member occurrence 1 `97b7f09b…127af`, worker `55e2b12`: several original SDK handlers match BR23, but baseline C in `link_control`, `link_policy`, `classic_baseband`, `hci_misc` already adds bounds, initialized message words, QoS hooks and reply corrections. | Share only after choosing the guard/formatter/unsupported-command contracts explicitly. Keep original BR23 recovery behavior unchanged during BD19 ownership; pure information/vendor/testing wrappers are the simplest later sharing candidates. |

| C21 | `bd19/au_law_codec.c` / `esco_codec.c`, worker `cc9237e`: BR23/BD19 selector and callback/table ABI agree; conversion bodies differ by loop shaping and equivalent integer operations. There was no previous BR23 C owner. | Candidate common implementation after explicit BR23 member ownership review. Preserve signed16 sample/count ABI and A-law full-scale behavior; do not infer codec activation from arithmetic tests. |

| C22 | `bd19/tws_power_balance.c`, occurrence 1 `eeb6614a…cb11`: common poll/role-count logic is close to BR23, but BD19 has table attribute 1 (BR23 3), a slave-only configuration gate, a weak role-decision hook and IRQ TX sections. Its hook returns zeroext i8 despite the generated bool declaration. | Candidate common core after preserving target table/gate/sections and hook behavior. Consolidate the byte-return alias with C03 only after auditing declarations and callers. Keep BR23 baseline bytes unchanged during this ownership stage. |

| C23 | `bd19/tws_ci_data.c`, worker `d5c3730`, occurrence 1 `c19a5053…c30c`: original BR23/BD19 core operations match; combined RX/TX differs by inlining. BD19 adds `.classic_tws_code` and an int-return event callback where BR23 returns void. No previous BR23 C owner exists. | Candidate common source after deliberate BR23 ownership and callback return/section handling. Preserve volatile flag accesses and copy/post/IRQ ordering; do not treat a matching copy loop as proof of common semaphore/lifetime policy. |

| C24 | `bd19/tws_lmp_sync.c`, worker `eaf2359`, occurrence 1 `e87bad0e…5ded`: counterparts live inside BR23 `tws_pair.c.o`. BD19 adds detach byte 5, timer-id tracking, a null-connection RX guard, channel attributes 12/13 and a strict +/-4 clock window (BR23 +/-12). | Share only after retaining these protocol, timing, table and member-boundary differences. BD19 timer callback remains external in retained `tws_pair.c.o` occurrence 1; do not expose BR23's private callback to satisfy it. |

| C25 | `bd19/tws_afh.c`, worker `90772cc`, occurrence 1 `bdb79b35…0ec0`: BR23 counterparts in `tws_pair.c.o` lack the WiFi detector/counter/classifier. BD19 uses table attribute 5 (BR23 2), fixed eSCO-dependent +200/+400, and returns zero during pending updates instead of retransmitting. | Keep separate implementations until a deliberate common protocol/policy exists. BR23 channel masking/run-slot delay are baseline behavior, not evidence of a BD19 bug. |

| C26 | `bd19/tws_event.c`, worker `f628a13`, occurrence 1 `73dd3fbc…1b53`: two 64-byte queues, three-byte records, timeout/ACK local delivery, int event callback and attribute 15 differ from BR23's two 16-byte queues, four-byte records, void callback/static handler and attribute 1. | Keep target wire formats and registration boundaries explicit. Empty init and combined RX/TX wrapper match SDK bodies, but sharing isolated functions is not useful until the surrounding ownership boundary is deliberately refactored. BR23's inherited record-stride issue does not describe BD19's stride-three loop. |

| C27 | `bd19/tws_tx_sync.c`, worker `e024e56`, occurrence 1 `6d7850f0…779a`: BR23 member only has empty init; BD19 adds phone-packet hooks, channel 4/attribute 1, deadline and hash-based prefix release, and an eight-entry record ring. | No common implementation beyond empty init is justified by the SDKs. Any BR23 activation needs an explicit protocol/application decision. Named packet fields preserve BD19 entry/data/length offsets 16/28/10 without imposing those offsets on host fixtures. |

## Confirmed BD19 behavior to revisit

Each row describes SDK behavior deliberately retained in accepted C. Fixes need
separate commits and tests of the chosen contract. Runtime effects remain
unestablished unless specifically stated.

| ID | Location / evidence | Follow-up |
| --- | --- | --- |
| B01 | `thread.c:thread_schedule`, `5443425`: timeout starts undefined on **all** paths reaching suspend, not just an empty list. | Port BR23's defined minimum/empty-list behavior and unify (C08). Do not invent an expected value for SDK UB in a test. |
| B02 | Classic HCI (`lmp_hci_settings/pairing/procedures.c`), `1f4d062` / `0fc29e3`: unchecked name length and feature-page index, four unaligned PIN words even for short PINs, unusual keypress varargs, unchecked connection senders. Device-manager baseband/information handlers ignore input lengths/page bounds (`5443425`). | Define host-input bounds and sender failures; compare BR23 app guards. Audit variadic formats independently before changing them. |
| B03 | Task queue/lifecycle, `a59c62b` / `0fc29e3`: argument-count assertions continue (negative counts included), `bredr_link_event` has no four-word bound, semaphore creation/post results are ignored and teardown waits/kills unconditionally. | Port reviewed count/failure handling; test IRQ/full-queue and failed semaphore/post cases. Preserve callback reloads and module snapshots when sharing. |
| B04 | Link/ACL/LMP allocation paths, `5752c05` / `bc6847b`: BD19 SDK proceeds after failed standard/control allocations; BR23 guards them. Heap type-0 failure loops forever (`2ce8540`); DMA can continue after returning assertion/reset (`971c549`). | Establish allocation failure policy across app/controller before replacing assertions or loops. Diagnostic hooks are not universally nonreturning. |
| B05 | `nv_memory.c`, `2ce8540`: allocation failure loops forever; release gives the pool base to `lbuf_free_btctrler`, whose metadata accesses precede its argument (BR23 revisit #21). | Confirm intended ownership of the 256-byte pool and its actual callers; repair release/failure policy deliberately. |
| B06 | HCI LE/vendor utilities (`aed66ee`, `93368e2`, `e73b179`): caller-sized copies, vendor debug copy into a 16-bit local, shared string storage, short scan-response uninitialized bytes and unsafe host lengths are retained. | Add bounds at real command/transport boundaries; do not assume trusted BTstack proves all callers safe. Review utility empty-string VLA and shared formatter buffers. |
| B07 | `ll_bis.c`, `aa66852`: close walks BIG list entries with BIS casts; access-address scoring includes the odd bit-23 term. | Trace list ownership and close ordering under multiple groups. Preserve the bit term until an independent algorithm/vector comparison justifies a change. |
| B08 | `ll_keys.c` weak hooks are no-ops; HCI acknowledges before calling them. Public-key caller has no argument and DH caller passes command size despite incompatible recovered prototypes (`7cbcdd0`, `07fc0dc`; BR23 #19). | Define a real key-generation/async completion contract and unify every caller/callee ABI before implementing a hook. |
| B09 | `ll_cis.c`, `fb6d1ac`: empty CIG free returns without releasing/removing it; CIS-ID search ignores CIG identity; interleaved duration uses unchecked lookup; handle exhaustion asserts then returns 102. | Test full multi-group create/destroy and exhaustion, then fix group ownership/lookup invariants. Channel setup remains a no-op. |
| B10 | `ll_common_iso.c`, `d596804`: fragmentation reads/writes null PDU/address 2; segmentation is empty; push logs `%d` without an argument; unpack leaves SDU/status bits undefined and clears decoded timestamps for first/complete fragments. | Complete the ISOAL contract before exposing ISO HCI traffic. Establish buffer allocation and fragment timestamp/length semantics. |
| B11 | `ll_common_iso.c`, `d596804`: codec success leaks its allocation; framed completion keeps its buffer; framed lengths/copies are unchecked, remaining-byte arithmetic wraps, later segments decode time offset from the **first** PDU payload. | Add packet-size validation and explicit buffer lifetime, then test malformed/multiple segments and error recovery. Do not mistake the SDK first-payload quirk for the corrected worker loop bug. |
| B12 | `le_cmd_set_iso`, `d596804`: missing handles leave sequence/counters undefined, BIS link quality fills only two stats; unrecognized BIS data-path direction succeeds; counter helper return values are ignored. `ll_iso_process` unsigned watermark starts UINT32_MAX and never normally advances. | Define error payload contents and status policy; initialize a meaningful watermark. RX-only test-end behavior is correct SDK recovery, not a remaining translation issue. |
| B13 | `bd19/ll_bis_slave.c`, original occurrence 1 `fe8dd656…4b86f1`: CREATE BIG accepts maxSdu zero, derives maxPdu zero and divides by it. Fragment-count rejection leaves allocated BIG state; accepted counts/timing can overflow a byte. | Validate SDU/PDU/NSE/latency ranges and release resource state on rejection as an app change. Test maxSdu 0 and multi-command retry with controlled failure handling. |
| B14 | `llcp_slv_create_big`, same IR: fixed 36-byte stack event, requests `36 + 2*n` bytes even though its six handles are already inside those 36 bytes; writes handles before its <=6 assertion. BIG lookup is unchecked. | Establish actual event encoding/length and size storage for all allowed streams. Host fixture clamps its copy; passing it does not establish safe production encoding. |
| B15 | BIS slave commands, same IR: TERMINATE BIG is handled but does nothing; production latency/advertising-handle parameters are validated/logged but not used to attach an advertising set or enforce latency. PHY feature mask still excludes bit 4. | Audit advertised ISO features against implemented operations, validate lifecycle and supported-PHY intent; do not infer usable LE Audio from symbol coverage. Encryption computes a local group session key without publishing it here; trace consumers before deciding that is a bug. |
| B16 | Periodic advertising `0a6545e`: BD19 16-bit timeout multiplication wraps (6554 -> 4 ms). Slot timer `f9aa10c`: remain(16), u16 fine-count and single-wrap quirks. Classic clock helpers `f57a6ce`: large before/after comparison windows. | Confirm caller range and units, select defined wrap contracts, then assess timing on hardware. Retain target hardware register sequences. |
| B17 | FCC `99413ce`: six-entry packet-table index is unchecked; failed link/frame allocation continues after diagnostic hooks; helper results may be ignored. | Validate factory-command inputs and failure policy if enabling test mode. Prior draft width/order/table errors were corrected and are not current SDK bugs. |
| B18 | TWS BLE `272ab0e`: optional monitor imports are unresolved in dormant discarded paths; AES diagnostics `a154c82` / `19c307e` use terminal loops and hardware byte ordering not demonstrated by host vectors. | Verify optional feature dependencies and actual AES/CCM engine vectors before enabling them. No firmware build proves those dormant features work. |

| B19 | `bd19/ll_cis_slave.c`, `2d79b41`: CIG allocation failure still invokes common accept using the link's existing CIS handle; invalid/unsupported requests free then attempt accept; masked host events queue reject followed by accept; null CIS allocation continues into a dereference. | Define one rejection/cleanup outcome and prevent stale-handle acceptance as an explicit app fix. Test asynchronous consumption of the two queued messages, not just their construction. |
| B20 | `bd19/ll_cis_master.c`, `2d79b41`: unpack writes caller count into six-element local arrays before validation (which allows up to 16); completion copies every global CIS handle to the input buffer while returning a size based on the requested count; zero-count CREATE CIS still reads the first pair and sets pending. | Bound command storage before parsing and make completion group/count/length agree; specify zero-count semantics. Preserve faithful recovery until that separate change. |
| B21 | Master test-CIG helpers cast test storage to production types, `2d79b41`. Production count/array start/stride are 16/17/9 bytes; test layout uses 19/20/14. Imported enable-count and interleaved-duration helpers therefore interpret different fields/records. | Confirm intended helper ABI and choose a corrected shared parameter contract before claiming test-mode support. Type-size assertions or an explicit C cast do not make these layouts equivalent. |
| B22 | Master bad CIS RSP posts failure, then success and create-done unconditionally (`2d79b41`). Its public RX adapter admits only RSP; the slave admits REQ/IND despite internal unknown/reject handlers. | Trace all LLCP dispatch callers and actual reachability; gate success on valid negotiation and ensure each failure completes once. Dormant handler cases are not proof the error route is connected. |
| B23 | CIS setup/IND arithmetic, `2d79b41`: NSE assertion requires strictly greater than both BNs despite an inclusive-range diagnostic; zero sizes/limits can lead to undefined division/float-to-byte conversion; slave started-anchor search is a wrapping u16 loop with no bound. | Validate negotiated limits and decide the inclusive NSE boundary. Add explicit scheduling failure/timeout policy; do not turn a host test of a returning assert hook into a hardware correctness claim. |

| B24 | `bd19/analog_ble.c`, original `analog_ble.c.o` occurrence 1 `fe535e9a…16f51`: AGC normal setters are no-ops; signed power byte wraps before clamping, max-level helper may run twice, and its callback-visible byte is reloaded. BLE4 sign-extends the final register value; BLE5 zero-extends it. | Establish valid level/callback contracts before simplifying; test hardware RSSI/power calibration independently. Review corrected a missing reload and zero-extension in the worker; those translation errors are resolved, not SDK defects. |

| B25 | Shared `authentication.c`, BD19 `authentication.c.o` occurrence 1 `af15cd3e…be2d`: g1/g2 rows for lengths 5/13/14 repeat earlier rows (BR23 #5); E22 always uses lmp_malloc but selects free by module bit. E22 PIN lengths >16 overrun its local buffer; E3 lengths outside 1..16 index outside reduction tables. | Correct tables against independent vectors as a separate app change; validate PIN/key lengths at real callers and decide allocator/failure policy. Existing host vectors deliberately retain SDK defects. Shared assertion diagnostics are normalized to owned policy, not byte-identical SDK reporting. |
| B26 | `bd19/uECC_curves.inc`, same occurrence: secp160r1/secp256k1 fast reductions use strict `result > p`; reducing p returns p rather than canonical zero. Python integer tests confirm this retained SDK behavior. | Trace crypto-toolbox callers for canonical-coordinate assumptions and test boundary vectors on the actual ROM backend before changing to >=. Host specialization tests use an independent arithmetic model; they do not validate ROM or real pairing. |

| B27 | `bd19/bredr_dut.c`, `9140e3e`, occurrence 1 `7a56461f…87ad`: failed LMP RX allocations are enqueued as null; loopback may use a null RX bulk; TX completion can free/clear the global DUT handle before the refill path unconditionally reads it. Unfinished TX slots may be overwritten; nonzero event_post with tws_link suppresses both event and free. Detach clears the handle without freeing it. | Define DUT lifecycle and buffer ownership, then add explicit allocation/error/cleanup policy as an app fix. Trace callback replacements and queued events; host hooks that repopulate the handle do not prove the real path safe. |
| B28 | Same DUT IR: IRQ-disabled hardware handshake polls indefinitely. PRBS probing reads three subsequent bytes even for short payloads; rare random error injection computes modulo the possibly zero TX length. The two-argument soft_agc caller ABI differs from the shared three-argument declaration. | Bound/validate DUT packet inputs, define hardware timeout handling, and consolidate the soft_agc declaration only with its caller/callee audit. Preserve snapshot-before-poll and target MMIO access widths/order. No RF or factory-mode hardware validation is established. |

| B29 | `bd19/h4_uart_hw.c`, `6e03c7a`, occurrence 1 `834544fb…e859`: TX polls indefinitely while CON0 bit 15 is **clear** and proceeds when set. Baud zero divides by zero; baud greater than clock/4 can underflow the divisor. IRQ hands the low 16 HRXCNT bits to a consumer without checking them against its 64-byte buffer. | Define baud/status/timeout contracts and audit consumer/hardware count bounds before enabling UART transport. The worker report inverted the TX-ready bit in prose; source, original IR, tests and this entry use set=ready. These are unchecked SDK assumptions, not proof hardware supplies an excessive count. |

| B30 | `bd19/audio_codec.c`, original occurrence 1 `602b140d…2d09`: DataInit does nothing, ConfigureDataPath always returns 1, StreamIn ignores audio/length and returns 0, StreamOut ignores payload and always mutes exactly 640 bytes in a 12-slot ring; saved ready callback is never invoked. | Decide whether any product needs this codec interface before exposing it; implement/test a real audio data contract separately. Decompilation and symbol coverage do not establish functioning audio. |
| B31 | Same occurrence: supported-codec enumeration leaves output entry unchanged at input count 0/1; stop only clears enabled; start/reuse does not reset ring or synchronization count. Packet counters wrap at 32 bits; printf diagnostics can reenter stop/start and mutate the selected context before later reloads. | Specify enumeration capacity semantics and lifecycle reset/callback/reentrancy policy before cleanup. Preserve current signed-delta/counter behavior until those contracts are decided; new tests confirm stale-state and reentry behavior rather than fixing it. |

| B32 | `bd19/h4_uart_interface_loop.c`, occurrence 1 `7445dd66…57c9`: ring push/pop/peek have no capacity/availability validation, offset is signed i8 and negative remainder narrows to a u16 index, counts wrap at 16 bits. Wrapped pop returns only the final segment length. Reset clears 1030 of 1034 bytes, preserving two auxiliary u16 fields. | Define valid lengths/index/count contracts before bounding copies or normalizing return/reset behavior. Tests preserve valid signed peeks, wrapping returns, IRQ reloads and prefix sentinel bytes; they do not prove malformed lengths safe. |
| B33 | Same occurrence: normal parser starts at count >=4 but type 2/5 peeks a two-byte length at offset 3; no 1024-byte packet limit is enforced, and incomplete nonempty frames leave the parser runnable. Two-wire accepts live count ==2, uses pre-timeout count's low byte for delivery length, then drops remaining input. Timeout comparison uses old signed counter >500000; bulk push does not reset it. | Validate parser framing/stack/ring bounds and define timeout/two-wire semantics. A diagnostic refill test confirms old count 6 can dispatch length 6 after only two new bytes arrive. Preserve length-helper calls/count snapshots until an explicit app fix. |
| B34 | Same transport callbacks: can-send always returns 0; set-baud only changes stored state; open ignores thread-create failure then initializes hardware and returns 0; close only closes hardware when thread.fun is nonnull. Handler registration accepts null; callbacks receive temporary stack packet storage. | Establish transport readiness, task-failure cleanup, effective baud changes and synchronous callback lifetime requirements before exposing UART transport. Host tests validate call order and mutation, not physical UART operation. |

| B35 | `bd19/h4_controller_interface.c`, occurrence 1 `3821c59f…d45a`: can-send always returns 0; registration simply stores the imported callback, including null. Send forwards the original buffer after narrowing type to u8 and size to u16, ignores the HCI handler return and returns 0. | Audit actual host callers and define readiness, null callback and size/error contracts before changing this thin boundary. Relate malformed-size validation to B06; source ownership alone does not justify a new transport policy. |

| B36 | `bd19/hci_controller.c`, worker `843223a`, occurrence 1 `c077ec3c…c0e8`: `hci_send_cmd_payload` allocates/sends wrapped u16(size+4), writes size+3 bytes, and leaves an extra unwritten byte for ordinary lengths; no allocation check or local free. `hci_send_cmd` copies into 256 stack bytes before checking narrowed final length, with initially undefined variable format length. | Audit caller bounds/ownership and choose a safe formatter/allocation contract. Large-size host fixtures use oversized backing solely to observe wrapped requests safely; they do not prove actual allocations safe or establish a leak without tracing ownership. |
| B37 | Same occurrence, event emitters: command-complete u8 length wraps (payload 253 gives wire length 2); extended data starts at offset 3 but wire length is u16(ext_len+2), omitting the final payload byte for positive valid lengths. Returning assertions still send narrowed lengths. | Confirm intended framing and length limits against callers/protocol before fixing. Packed 515-byte local storage and formatting assertions are not pre-copy bounds checks. |
| B38 | Same occurrence, incoming/lifecycle paths: dispatcher reads opcode/length without minimum-size checks; queued command assertion size<255 continues into allocation/copy and truncates stored size to u16. Direct ACL can route to both LE and Classic; queued LE match suppresses Classic even with roles disabled. Init continues after failed queue allocation, ignores thread-create result; destroy unconditionally calls transport close and does not delete the HCI thread. | Apply selected BR23 input guards (B06/C19) separately, define routing/failure/teardown contracts, and test lifecycle resource ownership. Preserve callback reloads and cached module/log snapshots; current host mocks establish ordering, not safe hardware teardown. |

| B39 | `bd19/lmp_hci_baseband.c`, same occurrence: EIR analysis wraps both index and total at eight bits, can cycle on malformed fields, checks total <=240 only after walking, and continues after returning assertions. It clears only four unaligned bytes before copying the reported length, preserving old tail data. Name read copies unbounded `strlen` into 248 allocated bytes; failed allocations assert then continue. | Define validated host/EIR/name bounds and failure policy separately. Host fixtures use larger backing or an assertion hook that supplies backing to observe SDK continuation; real buffers are not made safe by those fixtures. Check the consumer's use of preserved EIR tail bytes. |
| B40 | Same baseband occurrence: Read Connection Accept Timeout reads incoming bytes and overwrites the timeout; Read Current IAC LAP supplies its pointer to numeric format `113`; Read Page Scan Type uses format `1` with an extra value; Read Link Supervision Timeout supplies a double 32000.0 to format `1H2`. Voice-setting logs retain cached gating while reloading mutable settings between callbacks. | Port baseline BR23 reply corrections only as explicit behavior changes with protocol/caller tests. Do not let host variadic stubs reinterpret pointer/double arguments as integer values or silently normalize the SDK ABI. |
| B41 | `bd19/lmp_hci_cmd_parser.c`, same occurrence: commands ignore incoming length, several message paths initialize only opcode (including passkey routes), QoS is an unsupported placeholder emitting event 1 with `1H114444`; multiple flow-control/status commands are consumed without a reply. QoS handle is captured before status, payload fields reloaded afterward. | Audit downstream message consumers before initializing unused words or adding missing replies; define host support/validation policy (B06). Decide whether this placeholder should be exposed or replaced. Preserve saved-handle and callback reload boundaries until a separate fix. |

| B42 | `bd19/au_law_codec.c`, occurrence 1 `9f9eaff7…26d5`: A-law negates a negative sample at i16 width before sign extension; -32768 stays negative, selects segment 0 and encodes as `0x55` (decoding to -8), while +32767 encodes `0xaa`. Both codecs process only positive signed16 counts and always return 0. | Confirm intended full-scale treatment and count/status contract before any normalization. The independent integer model checks all signed16 inputs and all 256 codewords; this confirms SDK arithmetic, not audio-path reachability. |

| B43 | Same occurrence, `tws_power_balance_master_rx`: len is ignored and two bytes are always read before weak hooks; only slave TX has the enable gate. Init sets the poll counter to 800 without clearing the role counter/flag; true reports 1–4 preserve the old flag, while the fifth sets it. The default voltage hook returns 0; widened local+10 equality counts as a positive decision (H25 confirmed on BD19). | Establish framing, enabled-state, lifecycle reset and battery/hysteresis contracts before adding guards or normalizing state. Preserve post-hook counter reloads and byte/halfword wrap. Weak-hook reentry tests establish ordering, not real battery calibration or TWS hardware behavior. |

| B44 | Same occurrence, open/sync: open ignores semaphore-create failure and leaves pending data/length/flag intact. Sync ignores set failure, publishes/replaces a borrowed pointer without a busy check, and a failed pend returns 0 without canceling it. Times 1–9 become pend timeout 0. Completion and supervision-timeout/detach handlers post the same semaphore; sync checks only pend status. | Define buffer lifetime, cancellation, minimum wait and delivery-status contracts before adding fixes. A modeled successful wake during event 45 returns the original length despite no TX; this is control-flow evidence, not proof of a particular OS/hardware outcome. |
| B45 | Same occurrence: outbound copies use unchecked stored signed length and reload it after memcpy; RX callbacks differ for zero length between plain and combined paths. Public sync accepts an external context, while transport callbacks always use the singleton. TX completion clears volatile pending before post; events 45/46 post before clearing under IRQ disable. | Validate spans/framing and clarify supported handle use. Preserve aliasing and callback-state observations until explicit serialization/cancellation policy is chosen. Safe fixtures exercise len aliasing, callback TX replacement and reentrant semaphore writes; they do not establish concurrency safety. |

| B46 | `bd19/tws_lmp_sync.c`, occurrence 1 `e87bad0e…5ded`: timer allocation 255 is stored/passed unchecked; disable releases the timer before storing 255 and leaves detach instant unchanged. RX reloads the instant after the clock hook; TX reloads global instant after timer setup. | Audit allocation failure, cancellation and callback lifetime before changing scheduling. Preserve signed minimum-two delay and wrapping clock+300 arithmetic. |
| B47 | Same member: slot RX reports 24 while writing six bytes, leaving the tail untouched. Master RX ignores length, requires aligned input, clears pending state before dispatch and uses independent unsigned strict +/-4 comparisons. Core RX copies 44 bytes for every non-five length. Core-sync enable and slot-master TX are no-ops. | Trace caller framing and tail initialization before declaring a leak or fixing lengths; audit wrap-edge suppression and malformed packets. Determine whether no-op features are intentionally disabled. These are SDK behaviors, not translation defects. |

| B48 | `bd19/tws_afh.c`, occurrence 1 `bdb79b35…0ec0`: WiFi detection requires config exactly 1, lazily stores unchecked 16-bit timer result, ignores map-helper status, and requires unsigned counter >=6 and >1 changed bits. Counter wraps. RX accepts any nonzero length as timestamp+map; duplicate instant exits before WiFi response. TX completion is a no-op. | Audit timer teardown/retry, malformed framing, duplicate-response suppression and classification policy before application fixes. Preserve callback reloads, including eSCO state after clock and link/map after AFH setup. |
| B49 | Same member imports `numbits_table` and three `wifi_det_*_channel_map` functions. No providers found in the pinned BD19 `cpu/bd19/liba/*.a` archive indexes or current C. Original SDK member has the same unresolved imports; `src/platform_jieli_sdk.c` sets `CONFIG_WIFI_DETECT_ENABLE=0`. | Optional WiFi path remains unsupported until real providers and integration tests exist. Do not add production stubs or infer support from host mocks, ownership audits, or controller-only builds. Validate optional-feature linkability when enabling it. |

| B50 | `bd19/tws_event.c`, occurrence 1 `73dd3fbc…1b53`: record append truncates event/value to bytes, queues at most 63 bytes, logs and unsniffs before checking capacity, and arms/modifies a 500/1000 timeout under IRQ disable. Timeout and ACK deliver local events under IRQ disable and event_lock; event22 resets queues/sequences but leaves timer active. Timeout clears timer ID without deletion; ACK conditionally deletes after notification. | Audit truncation, notification reentrancy/latency and timer lifetime, including allocation returning zero. Preserve dynamic queue-length/index reloads and ACK unlock-before-clear ordering until an explicit application policy changes them. These fixtures model callbacks, not actual concurrency. |
| B51 | Same occurrence: direct RX always reads data[0], even for zero/negative len; wrapper calls RX for any nonzero len. Triples are decoded whenever i<len, with no check that the final triple fits. RX uses sequence-only deduplication; TX returns queue length reloaded after IRQ enable, which may differ from bytes just written. Event initialization occurs once per invocation, so notify changes to other fields survive later records. | Establish trusted framing, callback ownership and caller buffer contracts before validation fixes. Investigate short/truncated frames and sequence wrap; tests use real backing storage and do not execute invalid reads. |

| B52 | `bd19/tws_tx_sync.c`, occurrence 1 `6d7850f0…779a`: eight-entry ring has unchecked byte count, overwrite on wrap and no output capacity argument. Full eight-byte record copies include padding. ACK truncates sum to u8 then subtracts eight once, rather than general modulo. RX walks every positive length in steps of eight without a final-span check; response/master RX require aligned u32. | Establish producer backpressure, maximum outstanding records, buffer capacity and framing guarantees. Investigate padding initialization and malformed frames before changing protocol. Exhaustive sequence fixtures and boundary ACK fixtures preserve SDK behavior, not prove safety. |
| B53 | Same member: sequence acceptance is old<new or old-new>=8 after zero extension; hash-only acknowledgment uses zero as absent and ignores length. Matching packets release an entire prefix. TX deadline scan stops at first future packet; RX expiry flushes all pending packets. Event22 resets counters but leaves pending list, records and slave hash; no-op init does not initialize the list. | Audit loss/reordering, sequence wrap, hash collisions/zero, ordered deadlines, ownership and reset lifecycle. Preserve redundant hash/eSCO calls, saved-next timing, and callback reloads until serialization/lifetime contracts are clear. Corrected draft branches/byte store were translation defects, not SDK bugs. |

## Inherited BR23 follow-ups: BD19 confirmation queue

The linked [BR23 list](revisit.md) contains the original details and evidence.
This index carries **every** existing entry into the BD19 audit, without
claiming every bug occurs on BD19. “Bxx” points to a BD19 finding above; all
other rows require occurrence-specific confirmation. Hardware observations
from a BR23 pad do not establish Nova 2 behavior.

| BR23 entry | Topic | BD19 action / status |
| --- | --- | --- |
| [1](revisit.md) | Vendor LMP commands answered before authentication (escape 3). | Retained LMP escape-3 member: preserve app refusal policy when recovered; confirm BD19 command reachability. |
| [2](revisit.md) | A peer can set the LE data length to zero. | Shared data-length member accepted; check malformed peer lengths and radio reaction on BD19. |
| [3](revisit.md) | Controller scheduler passes an uninitialized timeout (fixed). | Confirmed B01; BR23 fixed, BD19 deliberately still SDK behavior. |
| [4](revisit.md) | Classic clock comparisons barely compare. | Confirmed shared clock behavior; B16. |
| [5](revisit.md) | Key length reduction tables wrong for 5, 13 and 14 bytes. | Confirmed on BD19: all g1/g2 bytes match the shared C, including duplicate rows 5/13/14; B25. |
| [6](revisit.md) | Secure Simple Pairing copies half of U/V. | Encryption task accepted; confirm half-copy/dangling-pointer paths against BD19 callers. |
| [7](revisit.md) | Page arguments swapped. | Retained LMP command/link members; verify BD19 parameter order before any A/B. |
| [8](revisit.md) | Legacy advertising clears privacy-resolution failures before filtering. | Advertising accepted; confirm privacy error-clear/filter interaction end to end. |
| [9](revisit.md) | Legacy advertising data setters trust the host length. | Shared advertising setters accepted; include in B06 input-bounds work. |
| [10](revisit.md) | White list: use after free, count overflow. | Shared white-list member accepted; confirm removal lifetime/count logic in BD19 before a fix. |
| [11](revisit.md) | Unchecked lengths in LE HCI commands. | Confirmed unsafe LE/vendor command copies; B06. |
| [12](revisit.md) | AES-CCM waits forever with interrupts off. | AES hardware accepted; B18 plus bounded IRQ-off completion policy. |
| [13](revisit.md) | Stray ADC readings on the L2 trigger (outside the controller). | Outside btctrler; BR23 input calibration fix, no BD19 controller action inferred. |
| [14](revisit.md) | The chip never enters light sleep, even on battery. | BR23 never-idle observation only; measure Nova 2 power policy separately. |
| [15](revisit.md) | The Classic sniff wedge (fixed; not the SDK's fault). | BR23 app fixes only; verify BD19 receive-window initialization and HCI sniff/report path. |
| [16](revisit.md) | The Switch 2 signals sleep before going silent on Classic (handled). | BR23 profile/app handling; verify product-specific console behavior separately. |
| [17](revisit.md) | Missing remote LE features can read from address zero. | LE events accepted; confirm null-RX event formatter/log path on BD19. |
| [18](revisit.md) | BLE RSSI can index past the AGC dBm table. | Absent from accepted BD19 analog BLE: RSSI uses direct bit arithmetic and no table index; hardware accuracy remains B24. |
| [19](revisit.md) | Default LE key hooks never produce keys. | Confirmed weak-hook boundary; B08. |
| [20](revisit.md) | LE power-control and BIG-sync dispatchers acknowledge no-ops. | Shared PC/BIG-sync no-ops accepted; audit feature advertising and actual dispatcher reachability. |
| [21](revisit.md) | Nonvolatile allocator failure spins; pool release frees the base. | Confirmed NV pool issues; B05. |
| [22](revisit.md) | LE AFH hit rate is always ~100%. | Shared LE AFH accepted; confirm BD19 counter packing/denominators and configuration. |
| [23](revisit.md) | FCC packet type index is unchecked. | Confirmed BD19 six-entry variant; B17 (BR23 table has nine). |
| [24](revisit.md) | Initiator/scanner leftovers (worker-reported, unverified). | Legacy scan/init accepted; older worker report is still not a fresh BD19 failure-path proof. |
| [25](revisit.md) | TWS eSCO relay leaves heap fields uninitialized. | TWS link retained; verify uninitialized relay fields and frees before enabling. |
| [26](revisit.md) | TWS semaphore allocations are smaller than the declared type. | TWS pair occurrences retained: verify each occurrence and OS_SEM ABI separately. |
| [27](revisit.md) | TWS sniff assumes both peer and phone state exist. | TWS sniff retained; confirm null-state reachability in BD19. |
| [28](revisit.md) | TWS timed-call queue accepts a ninth entry in an eight-entry ring. | TWS timed calls retained; confirm ring guard and queue counts. |
| [29](revisit.md) | TWS key synchronization drops the second record. | TWS events retained; compare record stride and frame count. |
| [30](revisit.md) | TWS access-address scorer compares bytes at different bit positions. | Shared scorer accepted; unnormalized comparison preserved (`cb82461`). |
| [31](revisit.md) | BLE interspace search can read beyond its eight-entry array. | Confirmed scheduler behavior retained; review58 + B16 timing work. |
| [32](revisit.md) | BLE polling trusts connection bookkeeping for timing and array sizes. | Confirmed scheduler undef/count assumptions retained; review58. |
| [33](revisit.md) | TWS eSCO removal stores a state through a possibly null link. | TWS pair retained; confirm event/step/null path on correct occurrence. |
| [34](revisit.md) | BLE link creation continues after allocation failure. | Accepted link-layer path; B04 allocation policy. |
| [35](revisit.md) | BLE ACL fragmentation can make no progress with oversized chunks. | Accepted link-layer source; confirm caller excludes byte-truncated zero-progress chunks. |
| [36](revisit.md) | BLE link deletion clears only five bytes of its procedure FIFO. | Accepted link-layer source; verify short-clear observation against BD19 FIFO ownership. |
| [37](revisit.md) | BLE instant validation can return with interrupts disabled. | RF_ble5 retained; recheck IRQ restoration when owning it. |
| [38](revisit.md) | A failed BLE extended ring write clears available storage. | RF_ble5 retained; confirm failed-write effects before enabling extended DMA paths. |
| [39](revisit.md) | BLE TX completion can dereference a slot cleared by its probe callback. | RF_ble retained; recheck callback-cleared TX slot lifetime. |
| [40](revisit.md) | HCI page timeout used as milliseconds. | BR23 fixed; retained BD19 link member needs units/fix applicability check. |
| [41](revisit.md) | A failed switch to master went back with a master's timing. | BR23 fixed; retained BD19 link member needs saved timing/role recovery review. |

## Additional findings recovered from older commit reports

These were not all indexed in the 41-item BR23 list. They remain historical
reports pending BD19 confirmation, even where the source is already shared.
References below identify the original BR23 object report, not BD19 acceptance.
Reproduce against the pinned BD19 occurrence before classifying as confirmed.

| ID | Historical report / affected code | Specific follow-up |
| --- | --- | --- |
| H01 | `ll_ext_adv.c`, `b6a3960`: Extended advertising: ineffective DID increment; legacy conversion copies up to 255 bytes into 31-byte stack arrays; secondary-link null accesses, unknown-handle list-head access, partial-open/periodic-close leaks, first-set-only exit, misleading unarmed timer type. | Review each branch/ownership against BD19 and its configured feature gates; add independent malformed-input and cleanup tests. |
| H02 | `ll_ext_scan.c`, `9e70740`: Uninitialized extended-header flag, stale legacy address type and uncleared auxiliary interval/TX-power fields. | Trace raw BD19 report initialization and packet lengths; do not assign invented values to SDK undef. |
| H03 | `ll_resolve_list.c`, `e082087`: One-entry guard, uninitialized local/peer RPA before first refresh, nonzero-only IRK check, error-tag refresh log and unmeasured RNG/AES ordering. | Check reported capacity, allocation initialization and cryptographic vectors on BD19 hardware. |
| H04 | `ll_periodic_advertising.c`, `526852d`: Create/close assumes period_sync is nonnull. | Trace timeout-stop and no-sync callers; distinguish failed-open cleanup, which the report found sound. |
| H05 | `ll_wifi_detect.c`, `e515477`: 1000000/(interval*625) despite a reported 1.25 ms interval unit; thread had no known caller. | Verify units and current reachability before changing the multiplier. |
| H06 | `ll_scan.c`, `9e6d362`: Uncleared handle/never-set enable byte, failed-open registered link, uninitialized scan post output. | Pair with BR23 #24 and H10; verify initialization and ioctrl output contracts on BD19. |
| H07 | `ll_init.c`, `16bb329`: Resolution failure cleared; failed hardware open leaves a registered handle/link; unchecked interval divisor and old-interval widening. | Confirm central command validation and failure cleanup in BD19. |
| H08 | `ll_slave.c`, `797a1dd`: Vendor callback can report >34 bytes after filling a 34-byte stack buffer; logging does not prevent sending that length. Connection update mixes previous interval/window offset. | Bound callback output and validate units only after BD19 IR and callback contract checks. |
| H09 | `ll_multiple_phys.c`, `797a1dd`: refresh_phy_param gates writes on a bitwise operation over output pointer values. | Confirm exact BD19 pointer/integer lowering; test legitimate disjoint output pointers. |
| H10 | `hci_ll.c`, `487207b`: Set Scan Enable writes only filter_duplicates before passing scan_enable; command-word API embeds pointers; role-dependent disconnect fall-through. | Confirm BD19 message constructors and scanner use; distinguish internal trusted-word API from byte-stream commands. |
| H11 | `btcontroller_util.c`, `a750002`: Random range returns max for max<=min; empty-string reverse uses zero-size VLA; shared address string aliases across calls. | Choose range/empty-input contracts and reentrant formatting only as explicit app changes. |
| H12 | `bredr_link.c`, `ca4b097`: AFH classification recounts channels (possible 10-channel map); 30th inquiry CRC overwrites result/search counts; EIR allocation unchecked; some RX paths index slot 255; unsupported packet groups/slot rows and AFH end time may be undefined. | Member remains retained; preserve existing BR23 AFH fixes, recover BD19 originals and track required app fixes separately. |
| H13 | `bredr_dut.c`, `0f2ef03`: Unfinished TX bulk overwritten, failed RX allocations used, handle dereferenced after free, modulo-zero TX data selection, short PRBS payload overread, detach may leak. | Confirmed against original BD19 IR and fresh recovery `9140e3e`; B27/B28. Rejected-draft translation errors were corrected and are not SDK defects. |
| H14 | `aes_hw.c`, `654bd17`: Unbounded IRQ-off AES/CCM polls, assertion-only header-length guard, debug logs reveal key/nonce when enabled. | B18 covers hardware uncertainty; verify actual callers/header range and logging configuration before enabling diagnostics. |
| H15 | `vsprintf.c`, `6ff9a5d`: Length calculator b overwrites accumulated length, omits S; formatter has no destination bound, B before L/l has undefined source length (old C chose zero). | Compare both formatter functions and every used format on BD19; fix size computation before relying on caller assertions. |
| H16 | `ll_ext_init.c`, `9c061ea`: Interval max 0/1 can divide by zero; failed primary open later closes a null hardware pointer. | Confirm upstream HCI range enforcement and failed-open lifecycle. |
| H17 | `tws_data_forwarding.c`, `5b6c067`: Abandon loop runs past received count; size-offset-20 can become a huge copy length. | TWS retained: verify exact BD19 object/name, frame layout and checks before enabling peer data. |
| H18 | `tws_ll_adv.c`, `1441708`: Scan-response length copied into 31-byte field; failed-open registration retained; separate bitfield RMW equivalence was assumed by worker. | Confirm BD19 byte offsets and access order, then validate length and failure policy. |
| H19 | `tws_le_link.c`, `241014a`: Scan type left uninitialized; eSCO advertising writes wrong parameter block; unchecked leftover advertising pointers; optional monitor imports. | B18 is not proof of these details: confirm every path against BD19, including dormant exports. |
| H20 | `tws_task.c`, `33f184a`: Nested IRQ wrappers can enable early; detach leaves dangling semaphore; wait-pair wrappers ignore timeout. | Verify BD19 semaphore size/lifetime and lock nesting before enabling TWS. |
| H21 | `tws_sync_call.c`, `cfb334f`: get_bt_slot_time has three-argument caller versus four-argument prototype. | Resolve actual BD19 caller/callee ABI; do not paper over with an unchecked cast. |
| H22 | `ll_bis.c`, `f902896`: Allocation/free list invariants and codec-start failure leave disabled path after stop; close uses wrong list casts. | B07 covers confirmed close behavior; check the other historical allocator/data-path findings independently. |
| H23 | `ll_slave.c`, `797a1dd`: Control-PDU handlers read fixed fields without checking rx length; automatic connection-update divisor may be zero on fresh link. | Validate RX storage and peer-controlled fields; do not assume fixed radio slot capacity makes stale reads harmless. |
| H24 | `analog.c`, `04748c7`: Register names and trim meanings inferred, hardware searches/cutoffs preserved; calibration/CCM/AES host doubles establish ordering rather than RF efficacy. | When recovering remaining analog/RF, retain masks/widths/order and measure independently before naming behavior as proven hardware facts. |
| H25 | `tws_power_balance.c`, `1b22394`: Battery balancing counts equality with local voltage + 10 as higher remote voltage (strict comparison). | BD19 widened equality confirmed in `1fd79e4` (B43). Confirm units/hysteresis intent before changing threshold. |
| H26 | `sys_timer.c`, `c238dc1`: Removal calls sys_timeout_del rather than sys_timer_del; handles truncate to u16 and modify results are ignored. | Check runtime API compatibility and timer lifetime on BD19; host adapters prove forwarding only. |

## Resolved recovery issues and process follow-ups

- **R01 — resolved, `07fc0dc`:** shared DTM caller alias removed a temporary
  chip conditional. Further consolidation needs actual ABI proof (C02/C03).
- **R02 — resolved, `d82dc24`:** LMP varargs cleanup now precedes bit updates
  and queue handoff. This was a translation error, not an SDK bug to fix later.
- **R03 — resolved, `1e21c06`:** HCI byte ABI/semaphore reloads, compensated
  low-power fine counts/anchor sequencing and device-manager dispatch were
  reviewed and finally merged. Original short worker reports are superseded.
- **R04 — resolved for FCC and DUT:** `dee8f74` withdrew false FCC/DUT
  acceptance; `99413ce` recovered FCC correctly. Fresh DUT recovery `9140e3e` and acceptance `5f51b69` replace the withdrawn draft; B27/B28 retain independently confirmed SDK issues.
  Do not reuse rejected hardware branches because symbol audits/tests passed.
- **R05 — resolved, `fb6d1ac`:** CIS init no longer clears the preserved
  bitmap; failure messages reload fields after callbacks.
- **R06 — resolved, `d596804`:** ISO init field, unsigned watermark comparison,
  opcode mask, segment-loop subtraction and packer reloads match SDK. Their
  corrected source mistakes are not open SDK defects; B10–B12 are separate.
- **R07 — resolved, `397c1a4`:** corrected reversed PHY timing,
  double numBis accounting, fragment-rejection cleanup, stale callback inputs,
  message policy snapshot and byte return ABI. The worker's happy-path tests
  mirrored wrong timing and did not model allocator numBis increments.
- **R08 — open reproducibility investigation:** forced/repeated builds once
  produced different BR23 frozen hashes (`bc6847b`, `3d1281f`); later review
  builds repeatedly matched all three originals. Cause not isolated; preserve
  baselines and compare flags/dependencies/toolchain/BUILD_ID if it recurs.
- **R09 — implemented in this change:** ownership/review skills require a full
  report in commit bodies and proposed/accepted revisit entries. Chat is only a
  commit/status pointer. No history rewrite: earlier chat-only evidence was
  backfilled here and in review notes. External session logs are optional
  recovery aids, never the sole durable report for new work.

- **R10 — resolved, `2d79b41`:** CIS pair recovery corrected the PDU-cap field,
  strict assertion conditions/accuracy threshold, extended-reject opcode,
  diagnostic and callback order, first-stream CIG delays, resource status,
  full-width internal offsets and slave anchor fields/count width. The new
  fixtures exercise failure and mutation paths; SDK issues B19–B23 remain.

## Accepted-member history coverage

Every accepted member was included in this history pass. The manifest-admission
revision identifies the acceptance checkpoint at the audited base,
not a claim of fresh full semantic review here. `bd19.json` pins the exact
occurrence/hash. “No additional finding” means none in the audited reports,
not absence of defects. See the earlier sections for cross-member issues.

| SDK member / occurrence | Manifest admission | Follow-up coverage |
| --- | --- | --- |
| `RF.c.o` / 1 | `a3a5af8` | C11; B18/H24 hardware proof |
| `analog_ble.c.o` / 1 | `c7712ed` | B24; separate hardware behavior, BR23 RSSI table issue does not apply |
| `aes128.c.o` / 1 | `a3a5af8` | B18 diagnostic vectors |
| `aes_hw.c.o` / 1 | `a3a5af8` | B18/H14 |
| `bredr_dut.c.o` / 1 | `5f51b69` | C11; B27/B28; H13/R04 |
| `bredr_fcc.c.o` / 1 | `99413ce` | C11; B17; R04 |
| `bredr_low_power.c.o` / 1 | `cb82461` | C11; B16; power policy BR23 #14 |
| `bredr_slot_timer.c.o` / 1 | `a1021a5` | C11; B16 |
| `bredr_utils.c.o` / 1 | `f57a6ce` | B16 / BR23 #4 |
| `au_law_codec.c.o` / 1 | `a3fce20` | C21; B42; two const callback tables |
| `esco_codec.c.o` / 1 | `a3fce20` | C21; exact four-entry selector |
| `audio_codec.c.o` / 1 | `e9f8aa7` | C16; B30/B31; stubs and lifecycle behavior |
| `authentication.c.o` / 1 | `3dfd4bd` | B25/B26; common Classic source plus BD19 ROM curve ABI |
| `btagc.c.o` / 1 | `a3a5af8` | C11; BR23 #18 is absent from accepted BD19 analog BLE (no RSSI table lookup) |
| `btcontroller_heap.c.o` / 1 | `2ce8540` | B04 |
| `btcontroller_linked_list.c.o` / 1 | `e39c89e` | No additional finding; shared body/data boundary reviewed in acceptance history. |
| `btcontroller_util.c.o` / 1 | `5c49316` | C06; B06/H11 |
| `btctrler_task.c.o` / 1 | `3d1281f` | C07/C12; B03 |
| `btencry_task.c.o` / 1 | `cee80c1` | BR23 #6 confirmation |
| `device_manager.c.o` / 1 | `cd41996` | C07; B02 |
| `hci_controller.c.o` / 1 | `843223a` | C19; B36–B38; B06 host-input guards |
| `hci_device_manager.c.o` / 1 | `e39c89e` | No additional finding; shared body/data boundary reviewed in acceptance history. |
| `hci_ll.c.o` / 1 | `5c49316` | B06/H10 |
| `h4_controller_interface.c.o` / 1 | `0af2992` | C18; B35; exact SDK callback/table ABI |
| `h4_uart_interface_loop.c.o` / 1 | `2d8a690` | C17; B32–B34; byte/bool ABI alias |
| `h4_uart_hw.c.o` / 1 | `23b5f74` | C15; B29; no hardware validation |
| `hci_ll_5_cmds.c.o` / 1 | `aed66ee` | B06; advertising H01 |
| `hci_ll_cmds.c.o` / 1 | `aed66ee` | C02/C03; B06/B08 |
| `hw_dma.c.o` / 1 | `971c549` | B04 |
| `interrupt.c.o` / 1 | `c36289d` | C03/C11 |
| `lbuf.c.o` / 1 | `a3a5af8` | B05 pool boundary |
| `le_low_power.c.o` / 1 | `007582d` | C11; R03; power policy BR23 #14 |
| `link_bulk.c.o` / 1 | `a1021a5` | C04/C06; B04 |
| `link_layer.c.o` / 1 | `a1021a5` | C08; B04; BR23 #34–36 |
| `link_task.c.o` / 1 | `cb82461` | C03/C10 |
| `ll_adv.c.o` / 1 | `a3a5af8` | B06; BR23 #8/9; H07 init comparison |
| `ll_afh.c.o` / 1 | `a3a5af8` | BR23 #22 counter denominator |
| `ll_bis.c.o` / 1 | `e39c89e` | B07/H22 |
| `ll_bis_master.c.o` / 1 | `7c790da` | BR23 #20 no-op commands |
| `ll_bis_slave.c.o` / 1 | `397c1a4` | C13; B13–B15; R07 |
| `ll_cis_master.c.o` / 1 | `2d79b41` | C14; B20–B23; R10 |
| `ll_cis_slave.c.o` / 1 | `2d79b41` | C14; B19/B22/B23; R10 |
| `ll_cis.c.o` / 1 | `fb6d1ac` | B09; R05 |
| `ll_common_iso.c.o` / 1 | `d596804` | B10–B12; R06 |
| `ll_data_extension.c.o` / 1 | `7c790da` | BR23 #2 peer length |
| `ll_events.c.o` / 1 | `cee80c1` | BR23 #17 formatter boundary |
| `ll_ext_adv.c.o` / 1 | `cb82461` | C05; H01 |
| `ll_ext_init.c.o` / 1 | `cee80c1` | H16 |
| `ll_ext_scan.c.o` / 1 | `fe5a38b` | C03; H02 |
| `ll_extended_advertising.c.o` / 1 | `cee80c1` | H01 host command boundary |
| `ll_init.c.o` / 1 | `a3a5af8` | BR23 #24; H07 |
| `ll_keys.c.o` / 1 | `7c790da` | C03; B08 |
| `ll_master.c.o` / 1 | `a3a5af8` | No additional finding; shared body/data boundary reviewed in acceptance history. |
| `ll_msg.c.o` / 1 | `5c49316` | B04/B06 unchecked state/returning assertions |
| `ll_multiple_phys.c.o` / 1 | `a3a5af8` | H09 |
| `ll_pc.c.o` / 1 | `7c790da` | BR23 #20 no-op commands |
| `ll_periodic_advertising.c.o` / 1 | `a3a5af8` | C06; B16/H04 |
| `ll_resolve_list.c.o` / 1 | `a3a5af8` | H03 |
| `ll_scan.c.o` / 1 | `cee80c1` | BR23 #24; H06 |
| `ll_slave.c.o` / 1 | `a3a5af8` | C08; H08/H23 |
| `ll_vendor.c.o` / 1 | `cee80c1` | No additional finding; shared body/data boundary reviewed in acceptance history. |
| `ll_white_list.c.o` / 1 | `cee80c1` | BR23 #10 lifetime/count |
| `ll_wifi_detect.c.o` / 1 | `7c790da` | H05 |
| `lmp_hci_cmd_parser.c.o` / 1 | `35f0689` | C20; B39–B41; original unguarded HCI ABI |
| `lmp_hci.c.o` / 1 | `1f4d062` | C07–C09; B02 |
| `lmp_rxtx.c.o` / 1 | `bc6847b` | C06/C07; B04; R02 |
| `lmp_utils.c.o` / 1 | `aed66ee` | C03; B16 clock/CRC boundaries |
| `misc_hw.c.o` / 1 | `cee80c1` | No additional finding; shared body/data boundary reviewed in acceptance history. |
| `misc_hw_common.c.o` / 1 | `cee80c1` | No additional finding; shared body/data boundary reviewed in acceptance history. |
| `multilink_schedule.c.o` / 1 | `c58c76e` | C12; BR23 #31/32 |
| `nv_memory.c.o` / 1 | `2ce8540` | B05 |
| `sys_timer.c.o` / 1 | `aed66ee` | H26 runtime timer API |
| `thread.c.o` / 1 | `cd41996` | C08; B01 |
| `tws_ble_link.c.o` / 1 | `a3a5af8` | B18 optional monitor imports |
| `tws_power_balance.c.o` / 1 | `1fd79e4` | C22; B43; H25; byte-return weak hook |
| `tws_ci_data.c.o` / 1 | `8e8c024` | C23; B44–B45; volatile/semaphore ordering |
| `tws_le_acc.c.o` / 1 | `cb82461` | BR23 #30 |
| `vendor_manager.c.o` / 1 | `cee80c1` | B06; BR23 #11 |
| `vsprintf.c.o` / 1 | `a3a5af8` | H15 |

| `tws_lmp_sync.c.o` / 1 | `eaf2359` + coordinator review | C24; B46–B47 |

| `tws_afh.c.o` / 1 | `90772cc` + coordinator review | C25; B48–B49 |

| `tws_event.c.o` / 1 | `f628a13` + coordinator review | C26; B50–B51 |

| `tws_tx_sync.c.o` / 1 | `e024e56` + coordinator review | C27; B52–B53 |
