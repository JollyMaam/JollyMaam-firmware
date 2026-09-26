#!/usr/bin/env python3
"""Patch the BR23 controller archive.

The Jieli archive is LLVM bitcode, so a member can be edited as LLVM IR text
and re-emitted as bitcode with the vendor's own clang.  Re-emitting an
unmodified member is lossless (the only difference in the printed IR is the
``ModuleID`` comment), so a patch only changes what it means to.

The build passes ``--config src/build_config.h`` and the mode comes from
``CYCLONE2_PATCH_BTCTLER`` there; ``none`` copies the archive unmodified.  The
two main modes:

``notify`` (default) adds one call to the archive - see
``patch_ssp_remote_notify()`` below - so the console-driven pairing path hands
the host the link key it derived.  Without it the host stack never learns the
key, never stores it, and every later reconnect is turned away.  It also lets
a received LMP_DETACH finish promptly - see ``patch_detach_drain_rx()``.

``diagnostic`` is the older experiment: it replaces ``lmp_esco_sniff_check``
with ``ret i1 1`` and adds three trace hooks.  The sniff override is not needed
(``src/profile_switch.c`` hands the profile the console's own 24-slot sniff
parameters and the stack calls ``lmp_set_sniff_establish_by_remote(1)``), and
this variant resets the chip in the middle of an SSP pairing when measured on
hardware, so it is for diagnostics only.  Two of its hooks are inert unless
``make DEBUG_LOG=1`` turned the vendor LMP tracing on:

* ``lmp_hci_link_key_request_reply`` prints the link key the host stack hands
  the controller when it answers a peer's link-key request;
* ``lmp_send_event_link_key_notification`` and ``lmp_rx_sres`` print the key a
  pairing produced and the challenge/response of a reconnect's authentication,
  so a rejected link key can be told apart from a peer that holds no key at all.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


FUNCTION = "lmp_esco_sniff_check"

# The pairing path a console drives: it pages us, runs SSP, and the library
# derives the link key itself.  See patch_ssp_remote_notify().
SSP_REMOTE_FUNCTION = "lmp_stage_auth_with_simple_pair_by_remote"
SSP_REMOTE_ANCHOR = "  store i8 36, i8* %step, align 1"

# Diagnostic: print the link key the stack hands the controller when it answers
# a peer's authentication request, so what was stored can be compared with what
# is actually used.  The four 32-bit words are the key loaded little-endian.
TRACE_FUNCTION = "lmp_hci_link_key_request_reply"
TRACE_ANCHOR = "  %a.sroa.6.0.copyload = load i32, i32* %a.sroa.6.0..sroa_cast10"
TRACE_FORMAT = "[LMP]link key %08x %08x %08x %08x\\0A\\00"


def patch_link_key_trace(text: str) -> str:
    encoded = TRACE_FORMAT.encode().decode("unicode_escape")
    name = "@.cyclone2_link_key_format"
    size = len(encoded)
    declaration = (f'{name} = private unnamed_addr constant '
                   f'[{size} x i8] c"'
                   + "".join("\\%02X" % b for b in encoded.encode()) + '", align 1\n')

    text = declaration + text
    marker = f"define void @{TRACE_FUNCTION}("
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"{TRACE_FUNCTION} was not found")
    anchor = text.find(TRACE_ANCHOR, start)
    if anchor < 0:
        raise RuntimeError(f"{TRACE_FUNCTION} key loads were not found")
    line_end = text.find("\n", anchor) + 1
    anchor_line = text[anchor:line_end]
    debug = re.search(r"!dbg !\d+", anchor_line)
    if debug is None:
        raise RuntimeError("anchor line has no debug location to reuse")

    call = (f'  %cyclone2_key_log = call i32 (i8*, ...) @printf(i8* '
            f'getelementptr inbounds ([{size} x i8], '
            f'[{size} x i8]* {name}, i32 0, i32 0), '
            f'i32 %a.sroa.0.0.copyload, i32 %a.sroa.4.0.copyload, '
            f'i32 %a.sroa.5.0.copyload, i32 %a.sroa.6.0.copyload), '
            f'{debug.group(0)}\n')
    return text[:line_end] + call + text[line_end:]


# Diagnostic: log the key a pairing produced (the controller library hands the
# host a LINK_KEY_NOTIFICATION here, which is what the host stores) and the
# authentication exchange of a reconnect.
NOTIFY_FUNCTION = "lmp_send_event_link_key_notification"
NOTIFY_ANCHOR = ("  %arraydecay1 = getelementptr inbounds %struct.lmp_connection, "
                 "%struct.lmp_connection* %conn, i32 0, i32 28, i32 7, i32 0")
NOTIFY_FORMAT = "[LMP]key notify %08x %08x %08x %08x\\0A\\00"

SRES_FUNCTION = "lmp_rx_sres"
SRES_ANCHOR = "  call void @function_E1_api(i8* %arraydecay, i8* %arraydecay4, i8* %arraydecay5, i8* nonnull %0)"
SRES_FORMAT = ("[LMP]sres rx=%08x ours=%08x rev=%08x "
               "K=%08x%08x%08x%08x R=%08x%08x%08x%08x\\0A\\00")

# Diagnostic: the SRES this side sends in answer to a peer's challenge, with the
# link key and random it was computed from.  This is the value a console
# compares against its own E1(), so it is the one to look at when a pairing
# stalls at LMP_AU_RAND.
OUT_FUNCTION = "__calc_and_send_sres"
OUT_ANCHOR = ("  call void @function_E1_api(i8* getelementptr inbounds "
              "(%struct.hci_parameter, %struct.hci_parameter* @hci_param, "
              "i32 0, i32 10, i32 0), i8* %rand_num, i8* %arraydecay, "
              "i8* nonnull %0)")
OUT_FORMAT = "[LMP]sres out=%08x K=%08x%08x%08x%08x R=%08x%08x%08x%08x\\0A\\00"


def emit_global(name: str, value: str) -> str:
    return f"{name} = private unnamed_addr constant " \
           f"[{len(value.encode())} x i8] c\"" \
           + "".join("\\%02X" % b for b in value.encode()) + "\", align 1\n"


def word_loads(prefix: str, indent: str, pointer: str,
               dbg: str) -> tuple[str, list[str]]:
    """Emit four i32 loads from ``pointer`` and return them by name.

    ``dbg`` is the debug location to tag the new instructions with; the
    controller archive is linked with LTO and debug info, where a function
    that has debug locations refuses to inline one that does not.
    """
    lines = [f"{indent}%{prefix}_p = bitcast i8* {pointer} to i32*{dbg}"]
    names = []
    for index in range(4):
        if index:
            lines.append(f"{indent}%{prefix}_{index}_p = getelementptr "
                         f"inbounds i32, i32* %{prefix}_p, i32 {index}{dbg}")
            source = f"%{prefix}_{index}_p"
        else:
            source = f"%{prefix}_p"
        name = f"%{prefix}{index}"
        lines.append(f"{indent}{name} = load i32, i32* {source}, align 4{dbg}")
        names.append(name)
    return "\n".join(line + "\n" for line in lines), names


def call_printf(indent: str, name: str, size: int, dbg: str,
                arguments: list[str]) -> str:
    escaped = arguments and ", " + ", ".join("i32 " + a for a in arguments) or ""
    return (f"{indent}%cyclone2_log = call i32 (i8*, ...) @printf("
            f"i8* getelementptr inbounds ([{size} x i8], "
            f"[{size} x i8]* @{name}, i32 0, i32 0)"
            f"{escaped}){dbg}\n")


def debug_location(line: str) -> str:
    match = re.search(r"!dbg !\d+", line)
    if match is None:
        raise RuntimeError(f"line has no debug location to reuse: {line}")
    return ", " + match.group(0)


def patch_key_notify_trace(text: str) -> str:
    encoded = NOTIFY_FORMAT.encode().decode("unicode_escape")
    declaration = emit_global("@.cyclone2_key_notify_format", encoded)
    text = declaration + text

    start = text.find("define void @" + NOTIFY_FUNCTION + "(")
    if start < 0:
        raise RuntimeError(f"{NOTIFY_FUNCTION} was not found")
    anchor = text.find(NOTIFY_ANCHOR, start)
    if anchor < 0:
        raise RuntimeError(f"{NOTIFY_FUNCTION} key source not found")
    line_end = text.find("\n", anchor) + 1
    dbg = debug_location(text[anchor:line_end])

    loads, words = word_loads("cyclone2_kn", "  ", "%arraydecay1", dbg)
    call = call_printf("  ", ".cyclone2_key_notify_format", len(encoded), dbg,
                       words)
    return text[:line_end] + loads + call + text[line_end:]


def patch_sres_trace(text: str) -> str:
    encoded = SRES_FORMAT.encode().decode("unicode_escape")
    declaration = emit_global("@.cyclone2_sres_format", encoded)
    scratch = ("@cyclone2_sres_reversed = internal global [16 x i8] "
               "zeroinitializer, align 4\n"
               "@cyclone2_sres_result = internal global [16 x i8] "
               "zeroinitializer, align 4\n"
               "declare i32 @llvm.bswap.i32(i32)\n")
    text = declaration + scratch + text

    start = text.find("define void @" + SRES_FUNCTION + "(")
    if start < 0:
        raise RuntimeError(f"{SRES_FUNCTION} was not found")
    anchor = text.find(SRES_ANCHOR, start)
    if anchor < 0:
        raise RuntimeError(f"{SRES_FUNCTION} E1 call was not found")
    line_end = text.find("\n", anchor) + 1
    dbg = debug_location(text[anchor:line_end])

    indent = "  "
    lines = []
    # Received SRES and the value this side computed from its stored key.
    lines.append(f"{indent}%cyclone2_rx_p = bitcast i8* %rx to i32*{dbg}\n")
    lines.append(f"{indent}%cyclone2_rx0 = load i32, i32* %cyclone2_rx_p, align 4{dbg}\n")
    lines.append(f"{indent}%cyclone2_ours_p = bitcast i8* %0 to i32*{dbg}\n")
    lines.append(f"{indent}%cyclone2_ours0 = load i32, i32* %cyclone2_ours_p, align 4{dbg}\n")
    # Key and rand, for the record.
    loads, words = word_loads("cyclone2_k", indent, "%arraydecay5", dbg)
    lines.append(loads)
    loads, randoms = word_loads("cyclone2_r", indent, "%arraydecay4", dbg)
    lines.append(loads)
    # Retry the same challenge with the key byte-reversed: if that is what the
    # peer computed, the stored key is in the wrong byte order.
    lines.append(f"{indent}%cyclone2_rev_p = bitcast [16 x i8]* "
                 f"@cyclone2_sres_reversed to i32*{dbg}\n")
    for index, word in enumerate(words):
        lines.append(f"{indent}%cyclone2_swap{index} = call i32 "
                     f"@llvm.bswap.i32(i32 {word}){dbg}\n")
        lines.append(f"{indent}%cyclone2_revslot{index} = getelementptr "
                     f"inbounds i32, i32* %cyclone2_rev_p, "
                     f"i32 {3 - index}{dbg}\n")
        lines.append(f"{indent}store i32 %cyclone2_swap{index}, i32* "
                     f"%cyclone2_revslot{index}, align 4{dbg}\n")
    lines.append(f"{indent}call void @function_E1_api(i8* %arraydecay, "
                 f"i8* %arraydecay4, i8* bitcast ([16 x i8]* "
                 f"@cyclone2_sres_reversed to i8*), i8* bitcast ([16 x i8]* "
                 f"@cyclone2_sres_result to i8*)){dbg}\n")
    lines.append(f"{indent}%cyclone2_rev_out_p = bitcast [16 x i8]* "
                 f"@cyclone2_sres_result to i32*{dbg}\n")
    lines.append(f"{indent}%cyclone2_rev0 = load i32, i32* "
                 f"%cyclone2_rev_out_p, align 4{dbg}\n")
    arguments = ["%cyclone2_rx0", "%cyclone2_ours0", "%cyclone2_rev0"] + words + randoms
    lines.append(call_printf(indent, ".cyclone2_sres_format", len(encoded),
                             dbg, arguments))
    return text[:line_end] + "".join(lines) + text[line_end:]


def patch_sres_out_trace(text: str) -> str:
    encoded = OUT_FORMAT.encode().decode("unicode_escape")
    text = emit_global("@.cyclone2_sres_out_format", encoded) + text

    start = text.find("define void @" + OUT_FUNCTION + "(")
    if start < 0:
        raise RuntimeError(f"{OUT_FUNCTION} was not found")
    anchor = text.find(OUT_ANCHOR, start)
    if anchor < 0:
        raise RuntimeError(f"{OUT_FUNCTION} E1 call was not found")
    line_end = text.find("\n", anchor) + 1
    dbg = debug_location(text[anchor:line_end])

    indent = "  "
    lines = [f"{indent}%cyclone2_out_p = bitcast i8* %0 to i32*{dbg}\n",
             f"{indent}%cyclone2_out0 = load i32, i32* %cyclone2_out_p, align 4{dbg}\n"]
    loads, words = word_loads("cyclone2_ok", indent, "%arraydecay", dbg)
    lines.append(loads)
    loads, randoms = word_loads("cyclone2_or", indent, "%rand_num", dbg)
    lines.append(loads)
    lines.append(call_printf(indent, ".cyclone2_sres_out_format", len(encoded),
                             dbg, ["%cyclone2_out0"] + words + randoms))
    return text[:line_end] + "".join(lines) + text[line_end:]


def patch_sres_markers(text: str) -> str:
    """Record the authentication packet path without the vendor printf path."""
    text = "declare void @cyclone2_bringup_record(i32)\n" + text
    for function, code in (
        ("lmp_tx_au_rand", 32000),
        ("lmp_rx_au_rand", 32001),
        (SRES_FUNCTION, 32002),
        ("lmp_rx_not_accept_au_rand", 32003),
    ):
        start = text.find("define void @" + function + "(")
        if start < 0:
            raise RuntimeError(f"{function} was not found")
        entry = text.find("\nentry:\n", start)
        if entry < 0:
            raise RuntimeError(f"{function} entry block was not found")
        insert_at = entry + len("\nentry:\n")
        function_end = text.find("\n}", insert_at)
        dbg_match = re.search(r"!dbg !\d+", text[insert_at:function_end])
        if dbg_match is None:
            raise RuntimeError(f"{function} has no debug location")
        dbg = ", " + dbg_match.group(0)
        text = (text[:insert_at] +
                f"  call void @cyclone2_bringup_record(i32 {code}){dbg}\n" +
                text[insert_at:])

    start = text.find("define void @" + SRES_FUNCTION + "(")
    if start < 0:
        raise RuntimeError(f"{SRES_FUNCTION} was not found")
    anchor = text.find(SRES_ANCHOR, start)
    if anchor < 0:
        raise RuntimeError(f"{SRES_FUNCTION} E1 call was not found")
    line_end = text.find("\n", anchor) + 1
    dbg = debug_location(text[anchor:line_end])
    marker = (
        f"  %cyclone2_rx_p = bitcast i8* %rx to i32*{dbg}\n"
        f"  %cyclone2_rx_value = load i32, i32* %cyclone2_rx_p, align 1{dbg}\n"
        f"  %cyclone2_ours_p = bitcast i8* %0 to i32*{dbg}\n"
        f"  %cyclone2_ours_value = load i32, i32* %cyclone2_ours_p, align 1{dbg}\n"
        f"  %cyclone2_sres_match = icmp eq i32 %cyclone2_rx_value, %cyclone2_ours_value{dbg}\n"
        f"  %cyclone2_sres_code = select i1 %cyclone2_sres_match, i32 31745, i32 31999{dbg}\n"
        f"  call void @cyclone2_bringup_record(i32 %cyclone2_sres_code){dbg}\n"
    )
    text = text[:line_end] + marker + text[line_end:]
    text = patch_lmp_rx_opcode_markers(patch_unknown_opcode_markers(text))
    return patch_detch_reason_marker(text)


def patch_remote_auth_reply_markers(text: str) -> str:
    """Show whether a host key reply reaches the LMP SRES response path."""
    route = "lmp_hci_cmd_addr_to_conn"
    route_start = text.find("define void @" + route + "(")
    route_end = text.find("\n}", route_start)
    if route_start < 0 or route_end < 0:
        raise RuntimeError(f"{route} was not found")
    route_body = text[route_start:route_end]
    route_entry = route_body.find("\nentry:\n")
    if route_entry < 0:
        raise RuntimeError(f"{route} entry block was not found")
    route_dbg_match = re.search(r"!dbg !\d+", route_body[route_entry:])
    if route_dbg_match is None:
        raise RuntimeError(f"{route} has no debug location")
    route_dbg = ", " + route_dbg_match.group(0)
    route_body = (route_body[:route_entry + len("\nentry:\n")] +
                  f"  call void @cyclone2_bringup_record(i32 32018){route_dbg}\n" +
                  route_body[route_entry + len("\nentry:\n"):])
    lookup = re.search(
        r"(?m)^  %call = tail call %struct\.lmp_connection\* "
        r"@lmp_conn_for_address\(i8\* %cmd\)(?: #\d+)?, !dbg !\d+.*$",
        route_body)
    if lookup is None:
        raise RuntimeError(f"{route} connection lookup was not found")
    lookup_dbg = debug_location(lookup.group(0))
    opcode_trace = (
        f"  %cyclone2_hci_opcode_value = load i32, i32* %msg, "
        f"align 4{lookup_dbg}\n"
        f"  %cyclone2_hci_opcode_low = and i32 "
        f"%cyclone2_hci_opcode_value, 255{lookup_dbg}\n"
        f"  %cyclone2_hci_opcode_tag = or i32 "
        f"%cyclone2_hci_opcode_low, 30720{lookup_dbg}\n"
        f"  call void @cyclone2_bringup_record(i32 "
        f"%cyclone2_hci_opcode_tag){lookup_dbg}\n")
    route_body = route_body[:lookup.start()] + opcode_trace + route_body[lookup.start():]
    lookup = re.search(
        r"(?m)^  %call = tail call %struct\.lmp_connection\* "
        r"@lmp_conn_for_address\(i8\* %cmd\)(?: #\d+)?, !dbg !\d+.*$",
        route_body)
    if lookup is None:
        raise RuntimeError(f"{route} connection lookup moved unexpectedly")
    lookup_dbg = debug_location(lookup.group(0))
    result = (
        f"  %cyclone2_conn_missing = icmp eq %struct.lmp_connection* "
        f"%call, null{lookup_dbg}\n"
        f"  %cyclone2_conn_code = select i1 %cyclone2_conn_missing, "
        f"i32 32020, i32 32019{lookup_dbg}\n"
        f"  call void @cyclone2_bringup_record(i32 %cyclone2_conn_code)"
        f"{lookup_dbg}\n")
    route_body = route_body[:lookup.end()] + "\n" + result.rstrip("\n") + route_body[lookup.end():]
    text = text[:route_start] + route_body + text[route_end:]

    function = "lmp_stage_auth_with_link_key_by_remote"
    start = text.find("define i32 @" + function + "(")
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError(f"{function} was not found")
    body = text[start:end]
    entry = body.find("\nentry:\n")
    if entry < 0:
        raise RuntimeError(f"{function} entry block was not found")
    insert_at = start + entry + len("\nentry:\n")
    dbg_match = re.search(r"!dbg !\d+", body[entry:])
    if dbg_match is None:
        raise RuntimeError(f"{function} has no debug location")
    dbg = ", " + dbg_match.group(0)
    body = (body[:entry + len("\nentry:\n")] +
            f"  call void @cyclone2_bringup_record(i32 32016){dbg}\n" +
            body[entry + len("\nentry:\n"):])

    calls = list(re.finditer(
        r"(?m)^(?P<indent>  )tail call void @__calc_and_send_sres\(.*$",
        body))
    if not calls:
        raise RuntimeError(f"{function} has no SRES response call")
    for match in reversed(calls):
        marker = (f"{match.group('indent')}call void "
                  f"@cyclone2_bringup_record(i32 32017)"
                  f"{debug_location(match.group(0))}\n")
        body = body[:match.start()] + marker + body[match.start():]
    return text[:start] + body + text[end:]


def patch_lmp_address_lookup_fallback(text: str) -> str:
    """Match HCI command addresses in either HCI or controller byte order."""
    function = "lmp_conn_for_address"
    start = text.find("define %struct.lmp_connection* @" + function + "(")
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError(f"{function} was not found")
    body = text[start:end]
    branch = "  br i1 %tobool5, label %if.then6, label %for.inc"
    position = body.find(branch)
    if position < 0:
        raise RuntimeError(f"{function} address comparison was not found")
    branch_line_end = body.find("\n", position) + 1
    dbg = debug_location(body[position:branch_line_end])
    replacement = (
        "  br i1 %tobool5, label %if.then6, "
        "label %cyclone2_addr_reverse\n"
        "cyclone2_addr_reverse: ; preds = %if.end\n"
    )
    for index in range(6):
        conn_ptr = (f"%cyclone2_conn_addr_{index} = getelementptr "
                    f"inbounds i8, i8* %arraydecay, i32 {index}{dbg}\n")
        addr_ptr = (f"%cyclone2_hci_addr_{index} = getelementptr "
                    f"inbounds i8, i8* %address, i32 {5 - index}{dbg}\n")
        conn_byte = (f"%cyclone2_conn_byte_{index} = load i8, i8* "
                     f"%cyclone2_conn_addr_{index}, align 1{dbg}\n")
        addr_byte = (f"%cyclone2_hci_byte_{index} = load i8, i8* "
                     f"%cyclone2_hci_addr_{index}, align 1{dbg}\n")
        equal = (f"%cyclone2_addr_equal_{index} = icmp eq i8 "
                 f"%cyclone2_conn_byte_{index}, "
                 f"%cyclone2_hci_byte_{index}{dbg}\n")
        replacement += conn_ptr + addr_ptr + conn_byte + addr_byte + equal
    replacement += (
        f"  %cyclone2_addr_equal_01 = and i1 %cyclone2_addr_equal_0, "
        f"%cyclone2_addr_equal_1{dbg}\n"
    )
    for index in range(2, 6):
        previous = f"0{index - 1}"
        current = f"0{index}"
        replacement += (
            f"  %cyclone2_addr_equal_{current} = and i1 "
            f"%cyclone2_addr_equal_{previous}, "
            f"%cyclone2_addr_equal_{index}{dbg}\n"
        )
    replacement += (
        f"  br i1 %cyclone2_addr_equal_05, label %if.then6, "
        f"label %for.inc{dbg}\n"
    )
    body = body[:position] + replacement + body[branch_line_end:]

    # A null address is used below only as the final fallback for this
    # single-ACL controller.  Permit that lookup to find a live HCI handle
    # before the vendor's separate L2CAP link pointer has been attached.
    no_link_branch = (
        "  br i1 %tobool1, label %for.inc, label %if.then")
    no_link_position = body.find(no_link_branch)
    if no_link_position < 0:
        raise RuntimeError(f"{function} active-link gate was not found")
    no_link_end = body.find("\n", no_link_position) + 1
    no_link_dbg = debug_location(body[no_link_position:no_link_end])
    no_link_replacement = (
        "  br i1 %tobool1, label %cyclone2_no_link_pointer, "
        f"label %if.then{no_link_dbg}\n"
        "cyclone2_no_link_pointer: ; preds = %land.lhs.true\n"
        f"  br i1 %tobool2, label %if.then, label %for.inc{no_link_dbg}\n")
    body = (body[:no_link_position] + no_link_replacement +
            body[no_link_end:])
    text = text[:start] + body + text[end:]

    # The HCI parser has several command-buffer layouts across its call sites.
    # Keep the parameter-only lookup first, then try bounded offsets that can
    # contain the six-byte BD_ADDR in a framed command buffer.  For a Link Key
    # Request Reply only, this port's single ACL slot is an unambiguous fallback
    # if the controller's address representations do not match.
    route = "lmp_hci_cmd_addr_to_conn"
    route_start = text.find("define void @" + route + "(")
    route_end = text.find("\n}", route_start)
    if route_start < 0 or route_end < 0:
        raise RuntimeError(f"{route} was not found")
    route_body = text[route_start:route_end]
    lookup = re.search(
        r"(?m)^  %call = tail call %struct\.lmp_connection\* "
        r"@lmp_conn_for_address\(i8\* %cmd\)(?: #\d+)?, !dbg !\d+.*$",
        route_body)
    if lookup is None:
        raise RuntimeError(f"{route} connection lookup was not found")
    dbg = debug_location(lookup.group(0))
    fallback = (
        f"  %cyclone2_conn_original = tail call %struct.lmp_connection* "
        f"@lmp_conn_for_address(i8* %cmd){dbg}\n"
        f"  %cyclone2_original_found = icmp ne %struct.lmp_connection* "
        f"%cyclone2_conn_original, null{dbg}\n")
    offset_names = []
    for offset in range(1, 10):
        fallback += (
            f"  %cyclone2_cmd_addr_offset_{offset} = getelementptr "
            f"inbounds i8, i8* %cmd, i32 {offset}{dbg}\n"
            f"  %cyclone2_conn_offset_{offset} = call "
            f"%struct.lmp_connection* @lmp_conn_for_address(i8* "
            f"%cyclone2_cmd_addr_offset_{offset}){dbg}\n"
            f"  %cyclone2_offset_found_{offset} = icmp ne "
            f"%struct.lmp_connection* %cyclone2_conn_offset_{offset}, "
            f"null{dbg}\n")
        offset_names.append(offset)
    fallback += (
        f"  %cyclone2_hci_opcode = load i32, i32* %msg, align 4{dbg}\n"
        f"  %cyclone2_is_key_reply = icmp eq i32 %cyclone2_hci_opcode, "
        f"1035{dbg}\n"
        f"  %cyclone2_acl_slot = getelementptr inbounds "
        f"[1 x %struct.lmp_connection], [1 x %struct.lmp_connection]* "
        f"@lmp_acl_link, i32 0, i32 0{dbg}\n"
        f"  %cyclone2_conn_active_acl = select i1 "
        f"%cyclone2_is_key_reply, %struct.lmp_connection* "
        f"%cyclone2_acl_slot, %struct.lmp_connection* null{dbg}\n"
        f"  %cyclone2_conn_fallback_9 = select i1 "
        f"%cyclone2_offset_found_9, %struct.lmp_connection* "
        f"%cyclone2_conn_offset_9, %struct.lmp_connection* "
        f"%cyclone2_conn_active_acl{dbg}\n")
    for offset in range(8, 0, -1):
        fallback += (
            f"  %cyclone2_conn_fallback_{offset} = select i1 "
            f"%cyclone2_offset_found_{offset}, %struct.lmp_connection* "
            f"%cyclone2_conn_offset_{offset}, %struct.lmp_connection* "
            f"%cyclone2_conn_fallback_{offset + 1}{dbg}\n")
    fallback += (
        f"  %call = select i1 %cyclone2_original_found, "
        f"%struct.lmp_connection* %cyclone2_conn_original, "
        f"%struct.lmp_connection* %cyclone2_conn_fallback_1{dbg}")
    route_body = (route_body[:lookup.start()] + fallback +
                  route_body[lookup.end():])
    selected = re.search(r"(?m)^  %call = select i1 .*?, !dbg !\d+.*$",
                         route_body)
    if selected is None:
        raise RuntimeError(f"{route} selected connection was not found")
    dbg = debug_location(selected.group(0))
    compare = (
        f"  %cyclone2_diag_acl = getelementptr inbounds "
        f"[1 x %struct.lmp_connection], [1 x %struct.lmp_connection]* "
        f"@lmp_acl_link, i32 0, i32 0{dbg}\n"
        f"  %cyclone2_diag_addr = getelementptr inbounds "
        f"%struct.lmp_connection, %struct.lmp_connection* "
        f"%cyclone2_diag_acl, i32 0, i32 29, i32 1, i32 0{dbg}\n")
    for index in range(6):
        compare += (
            f"  %cyclone2_diag_cmd_ptr_{index} = getelementptr inbounds "
            f"i8, i8* %cmd, i32 {index}{dbg}\n"
            f"  %cyclone2_diag_conn_ptr_{index} = getelementptr inbounds "
            f"i8, i8* %cyclone2_diag_addr, i32 {index}{dbg}\n"
            f"  %cyclone2_diag_conn_rev_ptr_{index} = getelementptr inbounds "
            f"i8, i8* %cyclone2_diag_addr, i32 {5 - index}{dbg}\n"
            f"  %cyclone2_diag_cmd_byte_{index} = load i8, i8* "
            f"%cyclone2_diag_cmd_ptr_{index}, align 1{dbg}\n"
            f"  %cyclone2_diag_conn_byte_{index} = load i8, i8* "
            f"%cyclone2_diag_conn_ptr_{index}, align 1{dbg}\n"
            f"  %cyclone2_diag_conn_rev_byte_{index} = load i8, i8* "
            f"%cyclone2_diag_conn_rev_ptr_{index}, align 1{dbg}\n"
            f"  %cyclone2_diag_direct_{index} = icmp eq i8 "
            f"%cyclone2_diag_cmd_byte_{index}, "
            f"%cyclone2_diag_conn_byte_{index}{dbg}\n"
            f"  %cyclone2_diag_reverse_{index} = icmp eq i8 "
            f"%cyclone2_diag_cmd_byte_{index}, "
            f"%cyclone2_diag_conn_rev_byte_{index}{dbg}\n")
    for index in range(1, 6):
        current = f"0{index}"
        previous = "0" if index == 1 else f"0{index - 1}"
        compare += (
            f"  %cyclone2_diag_direct_{current} = and i1 "
            f"%cyclone2_diag_direct_{previous}, "
            f"%cyclone2_diag_direct_{index}{dbg}\n"
            f"  %cyclone2_diag_reverse_{current} = and i1 "
            f"%cyclone2_diag_reverse_{previous}, "
            f"%cyclone2_diag_reverse_{index}{dbg}\n")
    compare += (
        f"  %cyclone2_diag_reverse_code = select i1 "
        f"%cyclone2_diag_reverse_05, i32 32022, i32 32023{dbg}\n"
        f"  %cyclone2_diag_match_code = select i1 "
        f"%cyclone2_diag_direct_05, i32 32021, i32 "
        f"%cyclone2_diag_reverse_code{dbg}\n"
        f"  call void @cyclone2_bringup_record(i32 "
        f"%cyclone2_diag_match_code){dbg}\n")
    route_body = (route_body[:selected.end()] + "\n" + compare.rstrip("\n") +
                  route_body[selected.end():])
    return text[:route_start] + route_body + text[route_end:]


def patch_lmp_rx_opcode_markers(text: str) -> str:
    """Record each incoming primary LMP opcode without logging payloads."""
    start = text.find("define void @lmp_acl_c_handler(")
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError("lmp_acl_c_handler was not found")
    function = text[start:end]
    anchor = "  store i8 %1, i8* %opcode0, align 1,"
    position = function.find(anchor)
    if position < 0:
        raise RuntimeError("lmp_acl_c_handler opcode decode was not found")
    line_end = function.find("\n", position) + 1
    line = function[position:line_end]
    dbg = debug_location(line)
    marker = (
        f"  %cyclone2_rx_opcode = zext i8 %1 to i32{dbg}\n"
        f"  %cyclone2_rx_marker = or i32 %cyclone2_rx_opcode, 32256{dbg}\n"
        f"  call void @cyclone2_bringup_record(i32 %cyclone2_rx_marker){dbg}\n"
    )
    function = text[start:end]
    function = function[:line_end] + marker + function[line_end:]

    # Escape-3/4 PDUs put their extended opcode in opcode1.  The primary
    # marker above only says "escape"; this marker records the sub-opcode so
    # an unknown extended request can be distinguished from its response,
    # without exposing the PDU payload.
    stores = list(re.finditer(
        r"(?m)^\s*store i8 %(?P<value>[\w.]+), i8\* %opcode1, align 1,"
        r"(?P<metadata>.*)$", function))
    if len(stores) < 2:
        raise RuntimeError(f"expected extended-opcode stores, found {len(stores)}")
    for suffix, match in reversed(list(enumerate(stores[:2]))):
        dbg_match = re.search(r"!dbg !\d+", match.group("metadata"))
        if dbg_match is None:
            raise RuntimeError("extended-opcode store has no debug location")
        dbg = ", " + dbg_match.group(0)
        value = match.group("value")
        line_end = function.find("\n", match.end()) + 1
        ext_marker = (
            f"  %cyclone2_ext_opcode_i32_{suffix} = zext i8 %{value} to i32{dbg}\n"
            f"  %cyclone2_ext_opcode_tag_{suffix} = or i32 "
            f"%cyclone2_ext_opcode_i32_{suffix}, 30464{dbg}\n"
            f"  call void @cyclone2_bringup_record(i32 "
            f"%cyclone2_ext_opcode_tag_{suffix}){dbg}\n"
        )
        function = function[:line_end] + ext_marker + function[line_end:]
    return text[:start] + function + text[end:]


def patch_detch_reason_marker(text: str) -> str:
    """Record the reason every local LMP_DETACH is sent with."""
    start = text.find("define void @lmp_tx_detch(")
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError("lmp_tx_detch was not found")
    entry = text.find("\nentry:\n", start, end)
    if entry < 0:
        raise RuntimeError("lmp_tx_detch entry block was not found")
    location = re.search(r"!dbg !\d+", text[entry:end])
    dbg = ", " + location.group(0) if location is not None else ""
    marker = (
        "  %cyclone2_detach_reason = zext i8 %err_code to i32" + dbg + "\n"
        "  %cyclone2_detach_marker = or i32 %cyclone2_detach_reason, 32512" + dbg + "\n"
        "  call void @cyclone2_bringup_record(i32 %cyclone2_detach_marker)" + dbg + "\n"
    )
    absolute = entry + len("\nentry:\n")
    return text[:absolute] + marker + text[absolute:]


def patch_supervision_context_markers(text: str) -> str:
    """Record the SDK's role and stage whenever it sends the timeout PDU."""
    declaration = "declare void @cyclone2_bringup_record(i32)\n"
    if declaration.strip() not in text:
        text = declaration + text

    start = text.find("define void @lmp_tx_supervision_timeout(")
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError("lmp_tx_supervision_timeout was not found")
    entry = text.find("\nentry:\n", start, end)
    if entry < 0:
        raise RuntimeError("lmp_tx_supervision_timeout entry was not found")

    function_body = text[entry:end]
    dbg = debug_location(function_body)
    marker = (
        "  %cyclone2_role_p = getelementptr inbounds %struct.lmp_connection, "
        "%struct.lmp_connection* %conn, i32 0, i32 28, i32 2\n"
        "  %cyclone2_role = load i8, i8* %cyclone2_role_p, align 2\n"
        "  %cyclone2_role32 = zext i8 %cyclone2_role to i32\n"
        "  %cyclone2_role_marker = or i32 %cyclone2_role32, 31232\n"
        "  call void @cyclone2_bringup_record(i32 %cyclone2_role_marker)"
        f"{dbg}\n"
        "  %cyclone2_step_p = getelementptr inbounds %struct.lmp_connection, "
        "%struct.lmp_connection* %conn, i32 0, i32 28, i32 1\n"
        "  %cyclone2_step = load i8, i8* %cyclone2_step_p, align 1\n"
        "  %cyclone2_step32 = zext i8 %cyclone2_step to i32\n"
        "  %cyclone2_step_marker = or i32 %cyclone2_step32, 30976\n"
        "  call void @cyclone2_bringup_record(i32 %cyclone2_step_marker)"
        f"{dbg}\n"
    )
    insert_at = entry + len("\nentry:\n")
    return text[:insert_at] + marker + text[insert_at:]


def insert_at_entry(text: str, signature: str, lines: str) -> str:
    """Insert IR ``lines`` (with ``{dbg}`` placeholders) at a function entry."""
    start = text.find(signature)
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError(f"{signature} was not found")
    entry = text.find("\nentry:\n", start, end)
    if entry < 0:
        raise RuntimeError(f"{signature} entry block was not found")
    dbg = debug_location(re.search(r".*!dbg !\d+.*", text[entry:end]).group(0))
    insert_at = entry + len("\nentry:\n")
    return text[:insert_at] + lines.format(dbg=dbg) + text[insert_at:]


def patch_detach_flow_markers(text: str) -> str:
    """Follow a received LMP_DETACH through to the connection teardown.

    lmp_rx_detch() only records the reason, sets bit 0x8 in the connection
    state and sleeps 20 ticks; lmp_detach_check() is what tears the link down
    when it sees that bit.  Markers:

      0x7d20  lmp_rx_detch entered
      0x7d21  lmp_rx_detch back from its os_time_dly(20)
      0x75NN  lmp_detach_check tearing a connection down, NN = its err_code
      0x7d23  lmp_connection_timeout (supervision timeout) fired
    """
    text = insert_at_entry(
        text, "define void @lmp_rx_detch(",
        "  call void @cyclone2_bringup_record(i32 32032){dbg}\n")
    start = text.find("define void @lmp_rx_detch(")
    delay = text.find("  tail call void @os_time_dly(i32 20)", start)
    if delay < 0:
        raise RuntimeError("lmp_rx_detch delay was not found")
    line_end = text.find("\n", delay) + 1
    dbg = debug_location(text[delay:line_end])
    text = (text[:line_end] +
            f"  call void @cyclone2_bringup_record(i32 32033){dbg}\n" +
            text[line_end:])

    start = text.find("define internal fastcc void @lmp_detach_check(")
    anchor = text.find("  tail call fastcc void @lmp_rx_handler(", start)
    if start < 0 or anchor < 0:
        raise RuntimeError("lmp_detach_check teardown was not found")
    dbg = debug_location(text[anchor:text.find("\n", anchor)])
    marker = (
        f"  %cyclone2_detach_err_p = getelementptr inbounds "
        f"%struct.lmp_connection, %struct.lmp_connection* %conn.0, "
        f"i32 0, i32 2{dbg}\n"
        f"  %cyclone2_detach_err = load i8, i8* %cyclone2_detach_err_p, "
        f"align 2{dbg}\n"
        f"  %cyclone2_detach_err32 = zext i8 %cyclone2_detach_err to i32{dbg}\n"
        f"  %cyclone2_detach_check = or i32 %cyclone2_detach_err32, 29952{dbg}\n"
        f"  call void @cyclone2_bringup_record(i32 %cyclone2_detach_check){dbg}\n")
    text = text[:anchor] + marker + text[anchor:]

    # When the teardown is skipped because received ACL data is still queued
    # (rx_acl_u not empty), record what is in the queue:
    #   0x73NN  number of queued packets (capped at 255)
    #   0x72FF  the first packet's flag bits (0x20 handed to the host,
    #           0x08 done and freeable)
    #   0x71LL  the first packet's length, low byte
    start = text.find("define internal fastcc void @lmp_detach_check(")
    skip = text.find("\ndo.end:", start)
    if skip < 0:
        raise RuntimeError("lmp_detach_check queue-not-empty block was not found")
    body_start = text.find("\n", skip + 1) + 1
    delay_line = text[body_start:text.find("\n", body_start)]
    dbg = debug_location(delay_line)
    queue = (
        f"  %cq_head_p = bitcast %struct.list_head* %rx_acl_u to i8**{dbg}\n"
        f"  %cq_first = load i8*, i8** %cq_head_p, align 4{dbg}\n"
        f"  %cq_flags_p8 = getelementptr inbounds i8, i8* %cq_first, i32 -14{dbg}\n"
        f"  %cq_flags_p = bitcast i8* %cq_flags_p8 to i16*{dbg}\n"
        f"  %cq_flags = load i16, i16* %cq_flags_p, align 2{dbg}\n"
        f"  %cq_flags8 = and i16 %cq_flags, 255{dbg}\n"
        f"  %cq_flags32 = zext i16 %cq_flags8 to i32{dbg}\n"
        f"  %cq_len_p8 = getelementptr inbounds i8, i8* %cq_first, i32 -16{dbg}\n"
        f"  %cq_len_p = bitcast i8* %cq_len_p8 to i16*{dbg}\n"
        f"  %cq_len = load i16, i16* %cq_len_p, align 2{dbg}\n"
        f"  %cq_len8 = and i16 %cq_len, 255{dbg}\n"
        f"  %cq_len32 = zext i16 %cq_len8 to i32{dbg}\n"
        f"  br label %cq_count{dbg}\n"
        f"cq_count:\n"
        f"  %cq_node = phi i8* [ %cq_first, %do.end ], [ %cq_next, %cq_count ]\n"
        f"  %cq_n = phi i32 [ 0, %do.end ], [ %cq_n1, %cq_count ]\n"
        f"  %cq_n1 = add i32 %cq_n, 1{dbg}\n"
        f"  %cq_node_p = bitcast i8* %cq_node to i8**{dbg}\n"
        f"  %cq_next = load i8*, i8** %cq_node_p, align 4{dbg}\n"
        f"  %cq_head8 = bitcast %struct.list_head* %rx_acl_u to i8*{dbg}\n"
        f"  %cq_end = icmp eq i8* %cq_next, %cq_head8{dbg}\n"
        f"  %cq_cap = icmp ugt i32 %cq_n1, 254{dbg}\n"
        f"  %cq_stop = or i1 %cq_end, %cq_cap{dbg}\n"
        f"  br i1 %cq_stop, label %cq_done, label %cq_count{dbg}\n"
        f"cq_done:\n"
        f"  %cq_count_code = or i32 %cq_n1, 29440{dbg}\n"
        f"  call void @cyclone2_bringup_record(i32 %cq_count_code){dbg}\n"
        f"  %cq_flags_code = or i32 %cq_flags32, 29184{dbg}\n"
        f"  call void @cyclone2_bringup_record(i32 %cq_flags_code){dbg}\n"
        f"  %cq_len_code = or i32 %cq_len32, 28928{dbg}\n"
        f"  call void @cyclone2_bringup_record(i32 %cq_len_code){dbg}\n"
    )
    text = text[:body_start] + queue + text[body_start:]

    return insert_at_entry(
        text, "define internal void @lmp_connection_timeout(",
        "  call void @cyclone2_bringup_record(i32 32035){dbg}\n")


def patch_supervision_role_guard(text: str) -> str:
    """Skip the Central-only timeout PDU when this connection is Peripheral."""
    start = text.find("define void @lmp_tx_supervision_timeout(")
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError("lmp_tx_supervision_timeout was not found")
    header_end = text.find("\n", start)
    entry = text.find("\nentry:\n", start, end)
    if entry < 0:
        raise RuntimeError("lmp_tx_supervision_timeout entry was not found")
    dbg = debug_location(text[header_end:end])
    guard = (
        "  %cyclone2_guard_role_p = getelementptr inbounds "
        "%struct.lmp_connection, %struct.lmp_connection* %conn, "
        "i32 0, i32 28, i32 2\n"
        "  %cyclone2_guard_role = load i8, i8* %cyclone2_guard_role_p, align 2\n"
        "  %cyclone2_is_peripheral = icmp eq i8 %cyclone2_guard_role, 1\n"
        "  br i1 %cyclone2_is_peripheral, "
        "label %cyclone2_skip_timeout, label %cyclone2_send_timeout\n"
        "cyclone2_skip_timeout:\n"
        f"  call void @cyclone2_bringup_record(i32 31745){dbg}\n"
        "  ret void\n"
        "cyclone2_send_timeout:\n"
    )
    insert_at = entry + len("\nentry:\n")
    return text[:insert_at] + guard + text[insert_at:]


def patch_suppress_supervision_timeout(text: str) -> str:
    """Disable the optional LMP_SUPERVISION_TIMEOUT request for an A/B run.

    The Switch returns LMP_NOT_ACCEPTED(LMP_SUPERVISION_TIMEOUT,
    LMP_PDU_NOT_ALLOWED) during the failing reconnect.  Keep a marker so the
    reset-surviving ring proves this stub ran, without changing the receive
    path or the HCI supervision timeout.
    """
    start = text.find("define void @lmp_tx_supervision_timeout(")
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError("lmp_tx_supervision_timeout was not found")
    header_end = text.find("\n", start)
    header = text[start:header_end]
    location = re.search(r"!dbg !\d+", text[header_end:end])
    dbg = ", " + location.group(0) if location is not None else ""
    replacement = (header + "\nentry:\n"
                   f"  call void @cyclone2_bringup_record(i32 31744){dbg}\n"
                   "  ret void\n}")
    return text[:start] + replacement + text[end + 2:]


def patch_unknown_opcode_markers(text: str) -> str:
    """Keep the two unknown-LMP fallback operands in the reset log.

    lmp_acl_c_handler has separate escape3 and escape4 dispatchers.  The
    vendor's `unknow_opcpde: %d, %x` message prints the extended opcode and
    the first following byte, so a value such as `55, 24` is not enough to
    identify the escape class.  Record a class marker and the packed pair at
    each fallback, without changing how the controller handles the packet.
    """
    start = text.find("define void @lmp_acl_c_handler(")
    if start < 0:
        raise RuntimeError("lmp_acl_c_handler was not found")
    end = text.find("\n}", start)
    if end < 0:
        raise RuntimeError("lmp_acl_c_handler end was not found")

    function = text[start:end]
    pattern = re.compile(
        r"^(?P<indent>\s*)tail call void \(i32, i8\*, i8\*, \.\.\.\) "
        r"@log_print\(i32 4, i8\* null, i8\* getelementptr .*?"
        r"@\.str\.411,.*?i32 %(?P<opcode>[\w.]+), i32 %(?P<param>[\w.]+)\)"
        r"(?P<attrs> #\d+)?(?P<dbg>, !dbg !\d+)?$",
        re.M,
    )
    matches = list(pattern.finditer(function))
    if len(matches) != 2:
        raise RuntimeError(
            f"expected 2 unknown-LMP fallback sites, found {len(matches)}")

    for ordinal, match in reversed(list(enumerate(matches))):
        opcode = match.group("opcode")
        suffix = f"{ordinal}"
        code = 31488 + ordinal  # 0x7b00 = escape3; 0x7b01 = escape4
        labels = list(re.finditer(
            r"(?m)^sw\.default[^\n]*:\s*(?:;[^\n]*)?$",
            function[:match.start()]))
        if not labels:
            raise RuntimeError("unknown-LMP default block was not found")
        label = labels[-1]
        insert_at = function.find("\n", label.end()) + 1
        if insert_at <= label.end():
            raise RuntimeError("unknown-LMP default block has no body")
        dbg = match.group("dbg") or ""
        injected = (
            f"  %cyclone2_lmp_unknown_byte_ptr_{suffix} = "
            "getelementptr inbounds %struct.bredr_rx_bulk, "
            f"%struct.bredr_rx_bulk* %rx, i32 0, i32 8, i32 2{dbg}\n"
            f"  %cyclone2_lmp_unknown_byte_{suffix} = load i8, "
            f"i8* %cyclone2_lmp_unknown_byte_ptr_{suffix}, align 1{dbg}\n"
            f"  %cyclone2_lmp_unknown_byte32_{suffix} = zext i8 "
            f"%cyclone2_lmp_unknown_byte_{suffix} to i32{dbg}\n"
            f"  %cyclone2_lmp_unknown_op_{suffix} = and i32 %{opcode}, 255{dbg}\n"
            f"  %cyclone2_lmp_unknown_op_shift_{suffix} = shl i32 "
            f"%cyclone2_lmp_unknown_op_{suffix}, 8{dbg}\n"
            f"  %cyclone2_lmp_unknown_packet_{suffix} = or i32 "
            f"%cyclone2_lmp_unknown_op_shift_{suffix}, "
            f"%cyclone2_lmp_unknown_byte32_{suffix}{dbg}\n"
            f"  call void @cyclone2_bringup_record(i32 {code}){dbg}\n"
            f"  call void @cyclone2_bringup_record(i32 "
            f"%cyclone2_lmp_unknown_packet_{suffix}){dbg}\n"
        )
        function = function[:insert_at] + injected + function[insert_at:]

    return text[:start] + function + text[end:]


def patch_ll_slave_rx_markers(text: str) -> str:
    """LE peripheral receive: 0x4e00|opcode for each LL control PDU,
    0x4f01 for each data PDU handed towards the host."""
    declaration = "declare void @cyclone2_bringup_record(i32)\n"
    if declaration.strip() not in text:
        text = declaration + text
    start = text.find("define internal void @__conn_slave_rx_post_handler(")
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError("__conn_slave_rx_post_handler was not found")
    body = text[start:end]
    load = re.search(r"(?m)^  %7 = load i8, i8\* %arrayidx\.i\.i, align 4.*$", body)
    if load is None:
        raise RuntimeError("LL control opcode load was not found")
    dbg = debug_location(load.group(0))
    marker = (f"\n  %cy_ll_op = zext i8 %7 to i32{dbg}\n"
              f"  %cy_ll_code = or i32 %cy_ll_op, 19968{dbg}\n"
              f"  call void @cyclone2_bringup_record(i32 %cy_ll_code){dbg}")
    body = body[:load.end()] + marker + body[load.end():]
    data = body.find("\nsw.bb:")
    if data < 0:
        raise RuntimeError("LL data PDU branch was not found")
    line_end = body.find("\n", data + 1) + 1
    call = re.search(r"(?m)^  tail call void @conn_rx_acl_u_post_handler.*$", body[line_end:])
    if call is None:
        raise RuntimeError("conn_rx_acl_u_post_handler call was not found")
    dbg = debug_location(call.group(0))
    body = (body[:line_end] +
            f"  call void @cyclone2_bringup_record(i32 20225){dbg}\n" +
            body[line_end:])
    return text[:start] + body + text[end:]


def patch_ll_send_control_markers(text: str) -> str:
    """LE: 0x4b00|opcode for each LL control PDU the controller sends."""
    declaration = "declare void @cyclone2_bringup_record(i32)\n"
    if declaration.strip() not in text:
        text = declaration + text
    start = text.find("define void @ll_send_control_data(")
    end = text.find("\n}", start)
    entry = text.find("\nentry:\n", start, end)
    if start < 0 or entry < 0:
        raise RuntimeError("ll_send_control_data was not found")
    dbg = debug_location(re.search(r".*!dbg !\d+.*", text[entry:end]).group(0))
    marker = (f"  %cy_tx_op = and i32 %opcode, 255{dbg}\n"
              f"  %cy_tx_code = or i32 %cy_tx_op, 19200{dbg}\n"
              f"  call void @cyclone2_bringup_record(i32 %cy_tx_code){dbg}\n")
    at = entry + len("\nentry:\n")
    return text[:at] + marker + text[at:]


def patch_member(source, destination, clang, hook):
    ir = source.with_suffix(".ll")
    run([clang, "-target", "pi32v2", "-x", "ir", "-S", "-emit-llvm",
         str(source), "-o", str(ir)])
    ir.write_text(hook(ir.read_text()))
    run([clang, "-target", "pi32v2", "-x", "ir", "-c", "-emit-llvm",
         str(ir), "-o", str(destination)])


def run(command: list[str], cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def patch_ssp_remote_notify(text: str) -> str:
    """Make the console-initiated pairing hand the host its link key.

    ``lmp_stage_auth_with_simple_pair_by_remote`` is where a pairing driven by
    the other side lands - that is Change Grip/Order, where the console pages
    us and runs SSP.  At the end of the DHKey exchange the library derives the
    link key with ``f2_function_api`` straight into ``conn->core.link_key`` and
    then moves on without telling anyone.  Every other path that ends up with a
    key raises the event (``..._by_local`` reaches it through
    ``lmp_stage_calc_link_key``, and the two PIN paths call it directly), so
    this is the one place the notification is missing.

    It has to be there: the host stack is the side that stores EDR link keys
    and answers the peer's link-key request on a later connection.  With the
    event missing the host's database stays empty (measured: the reserved VM
    area is still 0xff after a pairing that produced a working HID session),
    so a reconnect asks for the key, is told PIN_OR_KEY_MISSING, falls back to
    a fresh SSP, and a console that already holds a key drops the link with
    reason 5.

    The insertion is one call to a function that this same member defines, at
    the point where the key has just been computed and before the stage/step
    move on.  ``ret`` is 0 for success, exactly as the five existing call sites
    pass it.
    """
    start = text.find("define i32 @" + SSP_REMOTE_FUNCTION + "(")
    if start < 0:
        raise RuntimeError(f"{SSP_REMOTE_FUNCTION} was not found")
    end = text.find("\n}\n", start)
    anchor = text.find(SSP_REMOTE_ANCHOR, start)
    if anchor < 0 or end < 0 or anchor > end:
        raise RuntimeError("the SSP link key derivation was not found")
    line_end = text.find("\n", anchor) + 1
    dbg = debug_location(text[anchor:line_end])
    call = ("  call void @lmp_send_event_link_key_notification("
            "%struct.lmp_connection* nonnull %conn, i32 0)"
            f"{dbg}\n")
    return text[:anchor] + call + text[anchor:]


def patch_detach_drain_rx(text: str) -> str:
    """Let a detach finish instead of waiting for the supervision timeout.

    lmp_detach_check() only tears a connection down once its received-ACL
    queue (rx_acl_u) is empty; otherwise it skips the connection and waits
    for a later pass - and after the peer's LMP_DETACH there may never be
    one, so the teardown happens only when the supervision timeout forces it
    (err_code 8), seconds later and with the wrong reason.

    With config_btctler_hci_standard = 1, lmp_send_acl_u_packet_to_host()
    hands one packet to the host per call and marks it done (0x08); it only
    unlinks and frees done packets on its *next* call.  So the packet the
    host received last - measured: the peer's 12-byte L2CAP Disconnection
    Request right before its LMP_DETACH, flags 0x2a - is still queued when
    the detach arrives, and nothing calls the function again.

    Drain the queue here with that same function (bounded), so delivered
    packets are freed and any undelivered one is handed to the host first.
    """
    start = text.find("define internal fastcc void @lmp_detach_check(")
    end = text.find("\n}", start)
    anchor = ("  %call83 = tail call fastcc i32 @list_empty("
              "%struct.list_head* %rx_acl_u)")
    position = text.find(anchor, start, end)
    if start < 0 or position < 0:
        raise RuntimeError("lmp_detach_check rx_acl_u check was not found")
    block = text.rfind("\nif.end80:", start, position)
    if block < 0:
        raise RuntimeError("lmp_detach_check if.end80 block was not found")
    dbg = debug_location(text[position:text.find("\n", position)])
    drain = (
        f"  br label %cyclone2_drain{dbg}\n"
        "cyclone2_drain:\n"
        "  %cyclone2_drain_i = phi i32 [ 0, %if.end80 ], "
        "[ %cyclone2_drain_next, %cyclone2_drain_body ]\n"
        f"  %cyclone2_drain_empty = tail call fastcc i32 @list_empty("
        f"%struct.list_head* %rx_acl_u){dbg}\n"
        f"  %cyclone2_drain_is_empty = icmp ne i32 %cyclone2_drain_empty, 0{dbg}\n"
        f"  %cyclone2_drain_limit = icmp ugt i32 %cyclone2_drain_i, 15{dbg}\n"
        f"  %cyclone2_drain_stop = or i1 %cyclone2_drain_is_empty, "
        f"%cyclone2_drain_limit{dbg}\n"
        f"  br i1 %cyclone2_drain_stop, label %cyclone2_drain_done, "
        f"label %cyclone2_drain_body{dbg}\n"
        "cyclone2_drain_body:\n"
        f"  tail call fastcc void @lmp_send_acl_u_packet_to_host("
        f"%struct.lmp_connection* nonnull %conn.0){dbg}\n"
        f"  %cyclone2_drain_next = add i32 %cyclone2_drain_i, 1{dbg}\n"
        f"  br label %cyclone2_drain{dbg}\n"
        "cyclone2_drain_done:\n"
    )
    return text[:position] + drain + text[position:]


def patch_bitcode_notify(source: Path, destination: Path, clang: str,
                         with_traces: bool = False,
                         with_markers: bool = False,
                         suppress_supervision_timeout: bool = False,
                         supervision_role_guard: bool = False) -> None:
    """Emit ``source`` with the missing link-key notification added."""
    ir = source.with_suffix(".ll")
    run([clang, "-target", "pi32v2", "-x", "ir", "-S", "-emit-llvm",
         str(source), "-o", str(ir)])
    text = patch_ssp_remote_notify(ir.read_text())
    text = patch_detach_drain_rx(text)
    if with_traces:
        # Print the key a pairing derived and the challenge/response of a
        # reconnect's authentication (needs make DEBUG_LOG=1 to be visible).
        text = patch_key_notify_trace(text)
        text = patch_sres_trace(text)
        text = patch_sres_out_trace(text)
    if with_markers:
        text = patch_sres_markers(text)
        text = patch_remote_auth_reply_markers(text)
        text = patch_lmp_address_lookup_fallback(text)
        text = patch_supervision_context_markers(text)
        text = patch_detach_flow_markers(text)
    if suppress_supervision_timeout:
        if not with_markers:
            raise RuntimeError("timeout suppression requires marker support")
        text = patch_suppress_supervision_timeout(text)
    if supervision_role_guard:
        if not with_markers:
            raise RuntimeError("timeout role guard requires marker support")
        text = patch_supervision_role_guard(text)
    ir.write_text(text)
    run([clang, "-target", "pi32v2", "-x", "ir", "-c", "-emit-llvm",
         str(ir), "-o", str(destination)])


def patch_lmp_outgoing_opcode(source: Path, destination: Path, clang: str) -> None:
    """Record only the primary opcode byte of each locally built LMP PDU."""
    ir = source.with_suffix(".ll")
    run([clang, "-target", "pi32v2", "-x", "ir", "-S", "-emit-llvm",
         str(source), "-o", str(ir)])
    text = ir.read_text()
    declaration = "declare void @cyclone2_bringup_record(i32)\n"
    if declaration.strip() not in text:
        text = declaration + text
    start = text.find("define i32 @lmp_request(")
    end = text.find("\n}", start)
    if start < 0 or end < 0:
        raise RuntimeError("lmp_request was not found in lmp_rxtx.c.o")
    function = text[start:end]
    anchor = "  call void @lmp_format_packet(%struct.bredr_tx_bulk* %call, i8* %format, i8* %3)"
    position = function.find(anchor)
    if position < 0:
        raise RuntimeError("lmp_request packet formatting call was not found")
    line_end = function.find("\n", position) + 1
    dbg = debug_location(function[position:line_end])
    marker = (
        "  %cyclone2_lmp_opcode_p = getelementptr inbounds %struct.bredr_tx_bulk, "
        f"%struct.bredr_tx_bulk* %call, i32 0, i32 13, i32 0{dbg}\n"
        "  %cyclone2_lmp_opcode = load i8, i8* %cyclone2_lmp_opcode_p, align 4"
        f"{dbg}\n"
        "  %cyclone2_lmp_opcode_i32 = zext i8 %cyclone2_lmp_opcode to i32"
        f"{dbg}\n"
        "  %cyclone2_lmp_opcode_tagged = or i32 %cyclone2_lmp_opcode_i32, 30208"
        f"{dbg}\n"
        "  call void @cyclone2_bringup_record(i32 %cyclone2_lmp_opcode_tagged)"
        f"{dbg}\n"
    )
    absolute = start + line_end
    text = text[:absolute] + marker + text[absolute:]
    ir.write_text(text)
    run([clang, "-target", "pi32v2", "-x", "ir", "-c", "-emit-llvm",
         str(ir), "-o", str(destination)])


def patch_bitcode(source: Path, destination: Path, clang: str) -> None:
    ir = source.with_suffix(".ll")
    run([
        clang,
        "-target",
        "pi32v2",
        "-x",
        "ir",
        "-S",
        "-emit-llvm",
        str(source),
        "-o",
        str(ir),
    ])
    lines = ir.read_text().splitlines()

    start = next(
        (index for index, line in enumerate(lines)
         if re.match(rf"^define\b.*@{FUNCTION}\(", line)),
        None,
    )
    if start is None:
        raise RuntimeError(f"{FUNCTION} was not found in {source}")

    return_match = re.match(
        r"^define(?:\s+\w+)*\s+(i\d+|void)\s+@",
        lines[start],
    )
    if return_match is None:
        raise RuntimeError(f"could not determine the return type of {FUNCTION}")

    end = next(
        (index for index in range(start + 1, len(lines)) if lines[index] == "}"),
        None,
    )
    if end is None:
        raise RuntimeError(f"end of {FUNCTION} was not found")

    lines[start:end + 1] = [
        lines[start],
        "entry:",
        f"  ret {return_match.group(1)} 1",
        "}",
    ]
    text = "\n".join(lines) + "\n"
    text = patch_key_notify_trace(text)
    text = patch_sres_trace(text)
    text = patch_sres_out_trace(text)
    ir.write_text(text)
    run([clang, "-target", "pi32v2", "-x", "ir", "-c", "-emit-llvm", str(ir), "-o", str(destination)])


MODES = ("none", "notify", "trace", "markers", "markers-no-supervision",
         "markers-role-guard", "diagnostic")


def mode_from_config(path: Path) -> str:
    """The string CYCLONE2_PATCH_BTCTLER is defined to in the build config."""
    match = re.search(r'(?m)^\s*#\s*define\s+CYCLONE2_PATCH_BTCTLER\s+"([^"]*)"',
                      path.read_text())
    if match is None:
        raise SystemExit(f"{path}: no CYCLONE2_PATCH_BTCTLER \"mode\" line")
    if match.group(1) not in MODES:
        raise SystemExit(f"{path}: CYCLONE2_PATCH_BTCTLER is "
                         f"\"{match.group(1)}\"; expected one of "
                         + ", ".join(MODES))
    return match.group(1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--ar", default="ar")
    parser.add_argument("--config", type=Path,
                        help="read the mode from CYCLONE2_PATCH_BTCTLER in "
                             "this header (src/build_config.h); overrides "
                             "--mode")
    parser.add_argument("--mode", choices=MODES,
                        default="notify",
                        help="notify (default): add the missing link-key "
                             "notification to the console-driven pairing "
                             "path; trace: notify plus the link-key/authentication "
                             "LMP trace printfs; markers: notify plus a quiet "
                             "SRES result in the reset-surviving trace; "
                             "markers-no-supervision: same markers while "
                             "suppressing the optional supervision-timeout "
                             "request for an A/B reconnect run; "
                             "diagnostic: the sniff override "
                             "plus the same traces; none: copy the archive "
                             "unmodified")
    args = parser.parse_args()
    if args.config is not None:
        args.mode = mode_from_config(args.config)
    args.source = args.source.resolve()
    args.destination = args.destination.resolve()

    if args.mode == "none":
        args.destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.source, args.destination)
        return

    members = subprocess.run(
        [args.ar, "t", str(args.source)],
        check=True,
        text=True,
        capture_output=True,
    ).stdout.splitlines()
    if "lmp.c.o" not in members:
        raise RuntimeError("btctrler archive has no lmp.c.o member")

    args.destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cyclone2-btctrler-") as temp_name:
        temp = Path(temp_name)
        run([args.ar, "x", str(args.source)], cwd=temp)
        if args.mode == "diagnostic":
            patch_bitcode(temp / "lmp.c.o", temp / "lmp.c.o.patched",
                          args.clang)
        else:
            patch_bitcode_notify(temp / "lmp.c.o", temp / "lmp.c.o.patched",
                                 args.clang, with_traces=(args.mode == "trace"),
                                 with_markers=(args.mode in ("markers",
                                                             "markers-no-supervision",
                                                             "markers-role-guard")),
                                 suppress_supervision_timeout=(
                                     args.mode == "markers-no-supervision"),
                                 supervision_role_guard=(
                                     args.mode == "markers-role-guard"))
        (temp / "lmp.c.o.patched").replace(temp / "lmp.c.o")
        if args.mode in ("markers", "markers-no-supervision", "markers-role-guard"):
            for member, hook in (("ll_slave.c.o", patch_ll_slave_rx_markers),
                                 ("link_layer.c.o", patch_ll_send_control_markers)):
                patch_member(temp / member, temp / (member + ".patched"),
                             args.clang, hook)
                (temp / (member + ".patched")).replace(temp / member)
            if not (temp / "lmp_rxtx.c.o").is_file():
                raise RuntimeError("btctrler archive has no lmp_rxtx.c.o member")
            patch_lmp_outgoing_opcode(temp / "lmp_rxtx.c.o",
                                      temp / "lmp_rxtx.c.o.patched", args.clang)
            (temp / "lmp_rxtx.c.o.patched").replace(temp / "lmp_rxtx.c.o")
        if args.mode in ("trace", "diagnostic") and (temp / "lmp_hci.c.o").is_file():
            patch_member(temp / "lmp_hci.c.o", temp / "lmp_hci.c.o.patched",
                         args.clang, patch_link_key_trace)
            (temp / "lmp_hci.c.o.patched").replace(temp / "lmp_hci.c.o")
        run([args.ar, "rcs", str(args.destination), *members], cwd=temp)


if __name__ == "__main__":
    main()
