"""Quiet controller bring-up instrumentation; no policy selection."""
import re
from .ir import debug_location

SRES_FUNCTION = "lmp_rx_sres"
SRES_ANCHOR = "  call void @function_E1_api(i8* %arraydecay, i8* %arraydecay4, i8* %arraydecay5, i8* nonnull %0)"


def authentication(text: str) -> str:
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
    text = lmp_receive(unknown_opcode(text))
    return detach_reason(text)


def remote_auth_stage(text: str) -> str:
    """Show whether a host key reply reaches the LMP SRES response path."""
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


def lmp_receive(text: str) -> str:
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


def detach_reason(text: str) -> str:
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


def detach_flow(text: str) -> str:
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


def unknown_opcode(text: str) -> str:
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


def le_receive(text: str) -> str:
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
    # Byte 6 of the control PDU: for LL_CHANNEL_MAP_IND the low byte of its
    # instant, which tells a retransmission (same instant) from a new update.
    marker6 = (f"\n  %cy_ll_b6p = getelementptr inbounds i8, i8* %arrayidx.i.i, i32 6{dbg}\n"
               f"  %cy_ll_b6 = load i8, i8* %cy_ll_b6p, align 1{dbg}\n"
               f"  %cy_ll_b6w = zext i8 %cy_ll_b6 to i32{dbg}\n"
               f"  %cy_ll_b6c = or i32 %cy_ll_b6w, 18944{dbg}\n"
               f"  call void @cyclone2_bringup_record(i32 %cy_ll_b6c){dbg}")
    pos = body.find("call void @cyclone2_bringup_record(i32 %cy_ll_code)")
    pos = body.find("\n", pos)
    body = body[:pos] + marker6 + body[pos:]
    # For each control PDU also the 16-bit header field of struct ble_rx
    # (LLID at bits 4-5; SN/NESN/MD among the rest): 0x4900|low, 0x4800|high.
    hdr = (f"\n  %cy_hdr_p = getelementptr inbounds %struct.ble_rx, %struct.ble_rx* %rx, i32 0, i32 1{dbg}\n"
           f"  %cy_hdr = load i16, i16* %cy_hdr_p, align 4{dbg}\n"
           f"  %cy_hdr32 = zext i16 %cy_hdr to i32{dbg}\n"
           f"  %cy_hdr_lo = and i32 %cy_hdr32, 255{dbg}\n"
           f"  %cy_hdr_lc = or i32 %cy_hdr_lo, 18688{dbg}\n"
           f"  call void @cyclone2_bringup_record(i32 %cy_hdr_lc){dbg}\n"
           f"  %cy_hdr_hi = lshr i32 %cy_hdr32, 8{dbg}\n"
           f"  %cy_hdr_hc = or i32 %cy_hdr_hi, 18432{dbg}\n"
           f"  call void @cyclone2_bringup_record(i32 %cy_hdr_hc){dbg}")
    pos = body.find("call void @cyclone2_bringup_record(i32 %cy_ll_b6c)")
    pos = body.find("\n", pos)
    body = body[:pos] + hdr + body[pos:]
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


def le_control_send(text: str) -> str:
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


def lmp_send(text: str) -> str:
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
    return text
