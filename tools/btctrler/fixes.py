"""Remaining SDK correctness fixes. Owned policy is extracted separately."""
from .ir import debug_location

SSP_REMOTE_FUNCTION = "lmp_stage_auth_with_simple_pair_by_remote"
SSP_REMOTE_ANCHOR = "  store i8 36, i8* %step, align 1"


def notify_remote_ssp_key(text: str) -> str:
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


def drain_detach_rx(text: str) -> str:
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


