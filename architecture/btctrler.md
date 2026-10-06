# Owned Bluetooth controller implementation

The firmware links the recovered C controller in `src/btctrler/` directly with
BTstack and the SDK CPU/OS runtime. No `btctrler.a` is built or linked. The
startup version symbol is supplied by `src/btctrler/version.c`, preserving the
SDK's 34-byte `.sys.version` record.

## Layers

| Layer | Responsibility | Ownership |
| --- | --- | --- |
| Profiles and application | Report formats, cadence, pairing choices | App C |
| Host stack | HID, L2CAP, connection management and HCI commands | BTstack and app integration |
| HCI transport | Command, event and ACL transfer | App integration with owned controller entry points |
| Controller command handling | Decode commands, route connections, apply link policy | Owned C |
| Link Manager / BLE Link Layer | Negotiation, connection procedures and state | Owned C |
| Baseband and radio | Scheduling, packets, retransmission, encryption and hardware timing | Owned C |

## Recovery history

The following extraction boundaries describe the earlier recovery process.
The final build links all owned controller C directly; the archive construction
pipeline has been retired.


`src/btctrler_hci_dispatch.c` owns `hci_command_process`, including HCI
header/packet-size validation and all nine SDK command groups. The packet
excludes the H4 type byte. Less than two bytes supplies no complete opcode and
is dropped; otherwise a missing length byte or a size other than `3 + plen`
receives invalid-parameter Command Complete before any group handler is called.
Trailing bytes are rejected too. Complete framing does not prove that every
retained handler validates its command-specific parameter layout.

The original dispatcher was internal `fastcc`. The extraction replaces its
definition with an ordinary C declaration and changes both direct and queued
call sites to the C calling convention. No forwarding dispatcher is retained.
Queue admission/allocation, task scheduling and packet lifetime are unchanged.
The C call runs before queue storage is freed, or synchronously in the original
direct caller. SDK queue size constraints still apply; framing checks here do
not replace queue admission validation. The transport's QoS rejection and
Read Encryption Key Size emulation still intercept before this boundary.

Common handlers are tried before Classic handlers, with fallback only on an
exactly zero return. Standard LE commands retain the SDK module-enable check;
vendor LE commands retain its ungated behavior. All groups and handler order
are expressed in C. `BTCTRLER_BIN_finish_command` retains the SDK's
assertion/reset behavior for unknown groups and results greater than one,
and Unknown HCI Command completion for an unhandled known-group command.
Its assertion text, event construction and error print buffer remain original
SDK instructions. Dispatch debug printfs are omitted.

`src/btctrler_device_manager.c` owns the common controller/baseband and
local-information handlers from `device_manager.c.o`. Set Event Mask copies
eight bytes into SDK storage; Event Mask Page 2 retains its acknowledge-only
behavior. Reset preserves Classic-before-LE ordering, the SDK LE feature bits,
normal-mode-before-DUT precedence and completion after the reset helpers.
The radio reset procedures themselves remain retained implementation.

Local-information dispatch preserves SDK version field/event ordering,
supported-command and feature arrays, and Classic address precedence in the
dual build. Eleven additional typed accessors expose this SDK storage without
recreating its anonymous parameter struct in C. Extended-feature page zero
explicitly selects standard features, pages one/two select the two eight-byte
extension pages, and larger page numbers return invalid parameter with a zero
feature payload. The original SDK used unchecked page arithmetic that could
read adjacent local-device fields. Parameter lengths are checked before reads.
No feature capabilities or version constants are redefined by this recovery.

`src/btctrler_classic_baseband.c` owns the complete 45-opcode
`hci_lmp_controller_baseband_cmd_handler`. Its policy covers scan enable and
activity, authentication/PIN/scan-mode flags, SSP state, class/voice parameters,
page/accept timeouts, host-buffer sizes, local name, EIR and SDK command stubs.
Seventeen additional typed accessors expose individual scalar/array addresses
and EIR storage, plus a typed host-buffer setter. No C SDK struct is recreated.
These addresses are used synchronously in the controller context.

Write Scan Enable retains completion-before-scan ordering; Refresh Encryption
Key retains helper-before-status ordering and the SDK's ignored helper return.
Bit changes preserve unrelated flags; SSP uses the SDK's nonzero-to-one rule.
EIR owns policy and heap allocation while the SDK retains its buffer pointer
and consumes/frees it under its existing storage lifetime. Parsing is bounded
by the 240-byte EIR field; overrun rejects before state changes. Reused buffers
are fully zero-filled, and allocation failure returns Memory Capacity Exceeded
instead of asserting/resetting. Read Local Name bounds the SDK name to 32 bytes
and constructs a zero-padded 248-byte response; heap allocation avoids adding a
large controller-task stack buffer. The SDK event formatter copies it before
free, matching its original lifetime.

Recovered unsafe/malformed replies are corrected explicitly: Read Connection
Accept Timeout reads existing storage instead of nonexistent parameters;
Read Current IAC serializes the three-byte LAP value instead of a pointer;
Read Page Scan Type includes the dropped value; the hard-coded supervision
reply passes integer 32000 instead of a double to an integer formatter.
The formatter's integer interpretation was checked in retained `vsprintf.c.o`.
This is not a general implementation of unsupported SDK capabilities.

Synchronous-flow-control and Host Number Of Completed Packets commands still
consume without events. Stored-key, event-filter, IAC-list, AFH and LE-host
writes retain acknowledge-only behavior. Automatic-flush and supervision writes
still store nothing. The voice read retains the SDK's byte-width reply despite
its two-byte storage; encryption refresh still ignores the helper return.
These limitations are explicit in C, rather than advertised as recovered
radio procedures. Variable stored-key/IAC payload lengths are checked too.

`src/btctrler_link_control.c` implements the complete SDK
`hci_lmp_link_control_cmd_handler` boundary, covering 24 opcodes. The owned outer
parser tries the SDK common link-control handler first, then this Classic
handler. Its pointer is the unframed parameter buffer (`packet + 3`), and
its length is the HCI header's parameter count, now checked against the
actual supplied packet size by the owned outer dispatcher. This Classic
handler checks command-specific lengths before reads.

Valid-command event ordering, handle masking and six-word role-machine messages
are preserved. Key and PIN payloads are copied into those messages before
synchronous dispatch. Unused message words are now zero-initialized rather than
uninitialized stack data. Short commands return invalid-parameter events:
Command Complete for reply/cancel commands and Command Status for asynchronous
procedures. Inquiry and voice commands retain the platform's disabled-feature
consumed/no-event behavior. The SDK's unimplemented passkey/keypress payload
packing, remote extended-feature page limitation and voice-command routing
quirks are documented in C; recovering the dispatcher does not fix those
procedures or establish voice support.

`src/btctrler_link_policy.c` implements `hci_lmp_link_policy_cmd_handler` in C.
The build removes that definition from `lmp_hci_cmd_parser.c.o`, leaving an
external declaration. The owned outer dispatcher calls our implementation using
the original ABI. Dispatch returns one for consumed commands and zero when
another group should try the opcode.

The owned dispatcher covers sniff entry/exit, role discovery/switch, per-link
policy read/write, default policy read/write and QoS Setup. Ordinary branches
reuse the SDK's existing helpers. Short parameters receive invalid-parameter
Command Status before a read. Legacy unsupported opcodes retain the SDK's
consumed/no-event behaviour; missing sniff handles retain its silent behaviour.
The SDK stores the default policy in a byte but returns it with the event
formatter's two-byte field; both details are preserved. SDK debug printfs
from the replaced dispatcher are omitted.

`lmp_tx_supervision_timeout` is also owned C. It preserves the SDK's
millisecond-to-slot conversion and uses the SDK request formatter. The
peripheral-role guard and role/stage trace records are expressed directly in C.
A peripheral never sends this central-only procedure.

`lmp_tx_qos_req` and `lmp_vendor_qos_interval_config` are recovered C too.
They preserve the SDK's six-to-4096-slot range, zero-disabled configuration,
one-shot state bit and `121` request format with NBC 2. The slot interval
global is now owned C storage (`btctrler_qos_poll_slots`); the retained
MAX_SLOT receive path reads it through an external declaration.
`lmp_event_esco_link_setup` is an owned no-op implementing the pad's tested
voice-link exclusion. The former eSCO body-rewrite patch has been deleted.

`src/btctrler_address.c` owns `lmp_conn_for_address` and
`lmp_hci_cmd_addr_to_conn`. It preserves direct/reversed address matching,
the live-handle/null-address case before link attachment
and the single-slot Link Key Request Reply fallback from the tested
builds. IRQ exclusion and enable on
lookup return follow the SDK's existing contract. C selects the connection;
`BTCTRLER_BIN_route_selected` retains the SDK's assertion/reset and role-machine
dispatch instructions. No connection layout is recreated in C.

The retired patch builders are `patch_lmp_address_lookup_fallback`,
`patch_supervision_context_markers`, `patch_supervision_role_guard` and
`patch_suppress_supervision_timeout`. No build configuration retains a copy
of these IR builders; their behavior lives in owned C. Authentication-stage
markers remain a separate small patch.

The enclosing Classic link-control parser is now owned and checks lengths.
All eleven address-routing call sites in the audited SDK belonged to that
parser and passed the unframed parameter buffer, with BD_ADDR at offset zero.
The nine speculative offset probes have been deleted. Address routing reads
exactly six address bytes. Direct/reversed matching and the single-slot key
reply fallback remain; the latter can select the slot before handle/link
attachment. The retained assertion/role dispatch still handles failed lookup.

`cyclone2_lmp_qos_send` and `cyclone2_lmp_qos_answer` are owned C wrappers.
`btstack_qos_jieli.c` owns asynchronous transaction state and HCI completion.
Only validated LMP receive interception and thirty-nine SDK layout accessors are
generated as IR for this boundary. No connection struct is reconstructed in
C. Pointers remain SDK-owned and are used synchronously in the controller
thread; the asynchronous QoS state retains only the HCI handle.

Other existing archive fixes and diagnostic markers remain IR patches.
Authentication, encryption, packet transport, scheduling and the larger
LMP receive state machine remain SDK implementation. The owned C objects
are linked directly with the generated retained archive, allowing LTO across
the boundary.

## SDK comparison tools

`PATH=/work/bin:$PATH make controller-ir` generates readable optimized LLVM IR
and JSON inventories under `build/controller-ir/sdk/` from the pristine SDK
archive. This optional inspection target is independent of the firmware build.
The shared SDK archive is never edited. There is no generated controller archive,
retained-member inventory or archive manifest.

| Module under `tools/btctrler/` | Responsibility |
| --- | --- |
| `ir.py` | Checked function extraction, debug locations and bitcode round-trip. |
| `recover.py` | Audited SDK definition fingerprints and historical extraction transforms. |
| `archive.py` | SDK fingerprints, ownership inventory and inspection entry point. |
| `inspection.py` | Cached IR decoding and reference/debug metadata inventories. |
| `sdktypes.py` | Controller types recovered from the SDK debug metadata. |
| `crosscheck.py` and `review/` | Compare owned object IR with the SDK implementation. |

`tools/inspect_btctrler.py` is the inspection command. Generated inspection
artifacts stay outside source control.

## Guardrails for subsequent recovery

The audited SDK contains 108 members and 2,687 function definitions. The
link-policy dispatcher originally had one direct caller in `hci_controller.c.o`; the
archive scan found no debug metadata evidence of an embedded inline copy.
The supervision sender has two direct callers in `lmp.c.o` and no debug
metadata evidence of an inline copy. The retained archive contains only
declarations of all twelve owned definitions. Both recovered device-manager handlers originally had one direct caller in
the outer dispatcher, without inline metadata evidence or restrictive memory
attributes. The outer dispatcher has two audited callers, both changed from `fastcc` to
ordinary C calls, and no inline metadata evidence. Its original call attributes
impose only size preferences and `nounwind`. The link-control dispatcher has one direct caller with only size preferences
and `nounwind`, and no inline metadata evidence. The QoS and eSCO boundaries,
like both address functions, were also
audited for inline metadata and caller attributes, with no embedded copies or
purity/restricted-memory assumptions found.
The caller's attributes were checked: they impose size preferences and
`nounwind`, not stale purity or restricted memory effects.

The owned boundary requires the audited source archive SHA-256 in
`tools/btctrler/archive.py`. The removed definitions also have instruction
body fingerprints in `tools/btctrler/qos.py` and `tools/btctrler/recover.py`.
Address/supervision extraction is checked before unrelated patches run.
Updating the SDK deliberately
requires auditing the new implementation and its callers, then updating
the fingerprints. Layout accessors use the SDK's LLVM types and checked
field anchors; they do not guess C padding.

Debug metadata is useful evidence, not a universal proof that no code was
inlined. An SDK update must inspect actual call sites and inline state too.
Replacing a standalone function cannot replace code already embedded into
another function. Remove optimizer attributes that make assumptions about
the old implementation when replacing a definition, and review call-site
attributes as well. Preserve calling convention, event format, task context,
locking and connection lifetime at each new boundary.

Host tests exercise dispatcher routing, event field widths, short commands,
SDK-wrapper refusals and response construction. Existing QoS tests exercise
transaction lifecycle. Supervision tests check peripheral suppression and
central transmission, including the trace records used by the tested builds.
Address tests exercise direct/reversed/missing addresses, inactive handles,
link attachment, rejection of shifted address candidates, exact six-byte addresses and
key-reply-only fallback. Link-control tests exercise every truncated length,
message packing, handle masking, helper arguments and procedure/event ordering.
Outer dispatch tests exercise short/mismatched/trailing-byte frames, all nine
groups, common/Classic handler precedence, exact-zero fallback, preserved result
values and assertion/completion fragment arguments.
QoS sender tests exercise configuration limits, zero suppression, one-shot
state and preserving unrelated state bits; eSCO exclusion is exercised as C.
Radio behaviour still requires PC/console measurements;
host tests cannot establish scheduler correctness or controller coexistence.

Device-manager tests cover short mask commands, page-two acknowledge-only
behavior, reset ordering and mode precedence, feature-bit preservation,
version/event field widths, supported-command and address responses, and all
256 extended-feature page values. The Classic/BLE smoke-test pair is slot B
(Switch 1 on the PC) and slot Y (Switch 2 on the console), using existing bonds.

Classic baseband tests cover the exact 45-command set, short and variable
parameters, preserved no-op/silent behavior, all flag-write values, SSP
normalization, scan/refresh event ordering, host-buffer field widths, bounded
names, EIR termination/boundary errors/zero-fill and allocation failure.
The new buffer tests were also run with AddressSanitizer and
UndefinedBehaviorSanitizer. The dispatcher has one audited original caller;
no inline metadata evidence or restrictive memory-effect assumptions were found.

The final build smoke test also exposed a layout-sensitive platform tick
divider: with byte storage in a compiler-merged word, uptime and reports
advanced much too slowly; using a volatile 32-bit divider restored normal
wall-clock timing and B/Y operation in the tested image. The exact interfering
write is not established; keep the divider in word storage.
