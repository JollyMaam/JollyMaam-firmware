#!/usr/bin/env python3
"""Patch the BR23 controller archive.

The Jieli archive is LLVM bitcode, so a member can be edited as LLVM IR text
and re-emitted as bitcode with the vendor's own clang.  Re-emitting an
unmodified member is lossless (the only difference in the printed IR is the
``ModuleID`` comment), so a patch only changes what it means to.

Two modes:

``notify`` (default) adds one call to the archive - see
``patch_ssp_remote_notify()`` below - so the console-driven pairing path hands
the host the link key it derived.  Without it the host stack never learns the
key, never stores it, and every later reconnect is turned away.

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


def patch_bitcode_notify(source: Path, destination: Path, clang: str) -> None:
    """Emit ``source`` with the missing link-key notification added."""
    ir = source.with_suffix(".ll")
    run([clang, "-target", "pi32v2", "-x", "ir", "-S", "-emit-llvm",
         str(source), "-o", str(ir)])
    ir.write_text(patch_ssp_remote_notify(ir.read_text()))
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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--ar", default="ar")
    parser.add_argument("--mode", choices=("notify", "diagnostic"),
                        default="notify",
                        help="notify (default): add the missing link-key "
                             "notification to the console-driven pairing "
                             "path; diagnostic: the sniff override plus the "
                             "LMP trace printfs")
    args = parser.parse_args()
    args.source = args.source.resolve()
    args.destination = args.destination.resolve()

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
                                 args.clang)
        (temp / "lmp.c.o.patched").replace(temp / "lmp.c.o")
        if args.mode == "diagnostic" and (temp / "lmp_hci.c.o").is_file():
            patch_member(temp / "lmp_hci.c.o", temp / "lmp_hci.c.o.patched",
                         args.clang, patch_link_key_trace)
            (temp / "lmp_hci.c.o.patched").replace(temp / "lmp_hci.c.o")
        run([args.ar, "rcs", str(args.destination), *members], cwd=temp)


if __name__ == "__main__":
    main()
