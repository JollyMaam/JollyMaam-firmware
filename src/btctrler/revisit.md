# btctrler: findings to revisit

Things the decompilation passes turned up that deserve a proper look once the
passes are done.  Most interesting first.  Each entry says how we know it:
**proven** (read from the SDK IR and/or tested), or **untested** (inferred,
not yet confirmed on air or hardware).  Owned C keeps SDK behaviour; nothing
here has been "fixed" unless it says so.

1. **Vendor LMP commands answered before authentication (escape 3).**
   `lmp_escape3.c`.  The SDK served a factory test box over the air: test
   mode, oscillator trim written to flash, chip/flash ID, a chip-key CRC and
   starting the firmware update loader, to any device that pages the pad,
   before authentication.  Proven from the SDK code; we now refuse all of
   them.  Untested on air: confirm against a stock pad (or another Jieli
   device) from a patchable controller, then consider disclosure to Jieli.


2. **A peer can set the LE data length to zero.**  `ll_data_extension.c`
   `__ll_receive_length_req` stores the peer's LL_LENGTH_REQ octets and
   times unchecked (the spec allows 27..251 / 328..17040) and tells the radio
   the new effective octets (ioctrl 15/16), so a central sending zeros sets
   our effective TX/RX octets to 0.  Proven from the IR; the radio's
   reaction is untested.  Reachable over the air by any connected central,
   before pairing.  Worth a test from a controllable central (ESP32).


3. **Controller scheduler passes an uninitialized timeout (fixed).**
   `thread.c`, `thread_schedule`: the suspend timeout starts undefined (LLVM
   `undef`) and is min-ed with each thread's timeout, then passed to
   `owner->suspend`. Proven in the SDK IR and initially preserved by ownership.
   Fixed by starting at 255, the largest byte timeout, then selecting the
   minimum; empty lists also pass 255. Host tests cover both traversal orders,
   zero and maximum timeouts, empty lists and deletion of the last thread.
   The sole current owner uses `bt_task_suspend`, which ignores the timeout:
   this removes undefined behavior but provides no evidence that the timeout
   caused the Classic sniff wedge or affected sleep timing.


4. **Classic clock comparisons barely compare.**  `bredr_utils.c`: clocks
   wrap at 2^27 (the spec's CLKN is 28 bits) and the "after/before" window
   is 69,120,000, more than half the wrap, so `bredr_clkn_after/before` are
   true for almost any pair.  Proven from the IR.  Untested what the radio
   code relies on; look again while owning `bredr_link`/`bredr_frame`.


5. **Key length reduction tables wrong for 5, 13 and 14 bytes.**
   `authentication.c`: the g1/g2 rows for L=5 repeat L=4, and L=13/14 repeat
   L=12, so Kc' differs from the spec for those effective key lengths.
   Proven (IR bytes plus the spec's K_session vectors).  Our hosts use 16
   bytes, so there's no effect today.


6. **Secure Simple Pairing copies half of U/V.**  `btencry_task.c`: the
   requests for g and f1 copy 24 of the 48-byte U/V arrays.  Proven from the
   IR.  Untested what it implies: possibly only P-192 sized values are
   supported (no Secure Connections), or a latent bug.  Request pointers
   also stay dangling after free.  Fresh SSP pairing works with the PC.


7. **Page arguments swapped.**  `lmp_commands.c` (HCI Create Connection →
   `link_page_try_start_enable`): the SDK passes the clock offset's low
   byte as the page scan repetition mode, and the repetition mode as the
   offset.  BTstack sends R1 with offset 0, so the pad pages with R0's train
   switching instead of R1's 128 repetitions.  Proven from the IR; kept
   because every reconnect test so far ran with it.  Try the spec order with
   a hardware A/B on reconnect speed and reliability (the "first page fails"
   flakiness may be related).


8. **Legacy advertising clears privacy-resolution failures before filtering.**
   `ll_adv.c`, `le_ll_adv_addr_process`: a failed `ll_resolve_peer_addr`
   sets `RESOLVE_ADDR_FAIL`, then the filter stage clears `ll_error_code`.
   With privacy enabled and no resolved entry, list filtering returns
   without marking a failure. Proven from the SDK IR; kept in owned C.
   Untested on air whether the radio independently rejects such requests.


9. **Legacy advertising data setters trust the host length.**  `ll_adv.c`,
   `ll_adv_set_adv_data` and `ll_adv_set_scan_rsp_data`: copy the length
   byte directly into 31-byte arrays without checking it. Proven from the
   SDK IR; kept in owned C. HCI input is the source, so this is a host-side
   robustness issue; malformed-command behavior is untested.


10. **White list: use after free, count overflow.**  `ll_white_list.c`:
   removal compares the address and type of a node after freeing it, and
   the "full" check is an exact `count == 25` on a u8, so nothing stops the
   count once it is past 25.  Proven from the IR.  Low impact (only the host adds
   entries).


11. **Unchecked lengths in LE HCI commands.**  `hci_ll_cmds.c`
   `vendor_handler` copies the caller's size into a 16-bit local (stack
   overrun above 2 bytes); `hci_ll_5_cmds.c` copies caller sizes unchecked
   and leaks the old scan parameters when replacing inactive ones.  Proven
   from the IR.  `vendor_manager.c` OCF 0 (CSR "harmony" query) reads and
   writes `data[7..24]` without checking the command length, and the LE HCI
   disconnect falls through to the advertising-parameters case when no role
   bit is set (`hci_ll.c`).  Host-originated only (BTstack), so it's a
   robustness issue, not an attack surface.


12. **AES-CCM waits forever with interrupts off.**  `aes_hw.c`
    `ccm_memory_v2` disables interrupts for the whole operation and polls the
    AES block's status with no bound.  Proven from the IR.  A stalled AES
    block would hang the pad; never seen.


13. **Stray ADC readings on the L2 trigger (outside the controller).**  One
    reading of 735 against a rest of 482 left L2 half pressed (fixed in
    `input_calibration.c`, e597eb2).  Source unknown; jumps of 64 or more are
    now logged (`input: L2 reading ...`).  The RF code only uses the ADC in
    factory trim mode, so the controller is not the likely culprit.  Untested.

14. **The chip never enters light sleep, even on battery.**  A temporary
    counter in `sleep_exit` stayed at 0 on USB and on battery (2 min on the
    Switch 2 in 5 ms Classic sniff, 2026-10-07).  So `bredr_low_power.c`'s
    suspend and resume (#42, register sequences identical to the SDK) never
    run, and the pad keeps the CPU and radio awake between sniff anchors.
    `platform_jieli.c` registers `pad_lp_target` with `never_idle`, which
    always returns 0: the application deliberately blocks system sleep.
    Proven in source during the #49 `le_low_power` review. Its BLE suspend
    and resume callbacks match the SDK IR, but physical sleep/wake timing
    remains untested under this policy. Worth measuring what sleeping
    between anchors would save if the policy is deliberately changed.

15. **The Classic sniff wedge happens on the Switch 2 too.**  On the
    pre-ownership baseline (fa7f562, SDK controller), slot A twice entered
    the console's 5 ms sniff, delivered no input, and dropped with 0x08
    about 12 s later; a retry then worked.  Same signature as the PC wedge
    in the benchmark notes (open problem 2), so it is in the SDK link layer
    or the console, not in owned code.  Now that LMP, the slot timers, the
    scheduler and low power are owned (#2-#43), instrument the sniff path
    (anchor misses, scheduler decisions, entry #3's undefined timeout).

16. **The Switch 2 signals sleep before going silent on Classic (handled).** With slot A
    connected, putting the console to sleep ends the link by supervision
    timeout (0x08), on the baseline and on owned code alike; on LE it sends a
    proper disconnect (0x13).  The pad then treats it as a lost link,
    reconnects, fails, tries the wake, and goes idle about 30 s later.  A
    firmware decision, not a controller bug. A subsequent pad-side LMP/ACL
    capture found output report 0x01, subcommand 0x06 with argument 0 before
    silence; the timeout followed about 12 seconds later. No HID Suspend,
    Virtual Cable Unplug or LMP detach was received in that capture. The
    profile already stopped reports on this command; it now marks the
    session ended so the controller starts ordinary shutdown on receipt, without
    waiting
    for the timeout, queueing an acknowledgement, reconnecting or waking.
    An ordinary timeout without the command still reconnects.

17. **Missing remote LE features can read from address zero.**
    `ll_events.c`, `le_read_remote_used_features_complete_event`: with null
    `rx`, the SDK passes a null pointer to the `1Hc08` event formatter.
    When the event is enabled, `link_layer.c` calls `__vsprintf`, whose
    `c08` case copies eight bytes from that pointer. Info logging also
    passes `&rx->data[1]` to `printf_buf` without checking `rx`. Proven
    from the SDK IR and formatter source during #50 review; preserved in
    owned C. The hardware effect and reachability of this null-rx path
    remain untested.

18. **BLE RSSI can index past the AGC dBm table.** `analog_ble.c`,
    `ble_analog_get_rssi`: the SDK uses `(RSSI2 >> 8) & 63` as an unchecked
    index into `agc_dbm_tlb`, whose definition in `btagc.c` contains only
    33 bytes. Indices 33..63 read beyond that definition. Proven from the
    SDK IR during #53/#54 review; preserved in owned C. Whether the radio
    can report those indices and the hardware effect remain untested.

19. **Default LE key hooks never produce keys.** `ll_keys.c` defines weak
    public-key and DH-key hooks that return zero without touching buffers or
    emitting events. `hci_ll.c` and `hci_ll_cmds.c` send successful Command
    Status for OCF 37/38 before invoking those hooks. With the default hooks,
    no key-completion event follows. Proven from the SDK IR and compiled
    callers during #58 review; preserved, including weak linkage. Direct
    invocation of these HCI commands on hardware remains untested.

20. **LE power-control and BIG-sync dispatchers acknowledge no-ops.**
    `ll_pc.c` handles low-10-bit opcodes 118..122 and `ll_bis_master.c`
    handles 107/108 by optionally logging and sending a success completion.
    They ignore the payload and perform no requested operation. Proven
    from the SDK IR during #59/#60 review; preserved in owned C. No caller
    or symbol reference beyond declarations was found in the SDK or owned
    production objects, so reachability in this firmware is unproven.

21. **Nonvolatile allocator failure spins; pool release frees the base.**
    `nv_memory.c` has a 256-byte lbuf pool. If allocation returns null,
    `nv_memory_malloc` loops forever. `nv_memory_release` passes the pool
    base to `lbuf_free_btctrler`, whose SDK implementation reads allocation
    metadata from 16 bytes before its argument (head pointer at -8), outside
    the pool. Both are proven from the SDK IR during #61 review and preserved
    in owned C. No production callers were found for these four wrappers;
    exhaustion and release effects on hardware remain untested.

22. **LE AFH hit rate is always ~100%.** `conn_rx_probe_channel_handler`
    (`ll_afh.c`) stores the incremented `rx_cnt` into `expected_rx_cnt` as
    well (one 64-bit bitfield store), then adds one more for MD. The
    per-channel hit rate `rx_cnt * 1000 / expected_rx_cnt` never sees missed
    packets, so channel ranking rests on RSSI alone. Proven from the SDK IR
    during #65 review; preserved. AFH is master-only and disabled in this
    firmware (LTO removes it), so there is no hardware effect today.

23. **FCC packet type index is unchecked.** `bredr_fcc_set_param` indexes the
    9-byte `packet_table` with the vendor command's third byte. Proven from
    the SDK IR during #63 review; preserved. Only reachable with
    `CONFIG_TEST_FCC_CODE` set, which this firmware leaves at 0.

24. **Initiator/scanner leftovers (worker-reported, unverified).** From the
    #66/#67 commit notes: `ll_init_open` and `ll_scan_open` return 58 on a
    failed radio open but leave the half-built link registered; a later
    close/destroy then walks a list that only a successful open initialises.
    `ll_init_open` computes the window offset as `random % (interval >> 1)`
    with no zero guard (urem confirmed in IR; whether HCI validation rules out
    interval < 2 is not checked). Both roles are removed by LTO in this
    peripheral-only firmware.

25. **TWS eSCO relay leaves heap fields uninitialized.** `tws_link.c`,
    `bredr_esco_tws_link_open`: allocates 80 bytes without clearing the
    block, then initializes selected fields. Timer handles, toggle bits and
    `tws_esco_tx_info` remain untouched; `esco_dump_cnt` also remains untouched
    on the non-monitor path with queued phone frames. Proven from the SDK IR
    during #77 review; preserved. TWS peer operation is unused here and removed
    by LTO, so effects on a real relay remain untested.

26. **TWS semaphore allocations are smaller than the declared type.**
    `tws_pair.c`: three paths allocate 80 bytes for `OS_SEM`, while `sdk.h`
    declares a 160-byte object. Proven allocation sizes and type layout from
    SDK IR/header during #78 review; preserved. Whether the semaphore routines
    touch beyond the allocation is untested; these TWS paths are removed by LTO.

27. **TWS sniff assumes both peer and phone state exist.** `tws_sniff.c`,
    `tws_sniff_with_phone`: a nonnull peer link permits an unchecked
    `tws_conn.lmp_conn->sniff` read. `tws_rx_unsniff_req` also uses
    `tws_link.link->task` without a null check for flags other than 0x55.
    Proven from SDK IR during #76 review; preserved. Reachability with absent
    connection state and hardware effects remain untested. The retry path in
    `tws_sniff_ap_set` does guard the connection; it is not another instance.

28. **TWS timed-call queue accepts a ninth entry in an eight-entry ring.**
    `tws_sync_call.c`, `tws_api_sync_call_func_internal`: rejects only when
    `sync_call_num > 8`, so a full queue accepts another call and overwrites
    the ring entry at `rx_index`. The count becomes 9, and the transmit
    callback copies nine records while wrapping around eight entries.
    Proven from SDK IR and the full-queue host regression during #79 review;
    preserved. Peer-visible loss/duplication remains untested on hardware;
    these timed-call paths are removed by LTO in this firmware.

29. **TWS key synchronization drops the second record.** `tws_event.c`:
    transmit can send two four-byte records with a sequence byte (9 bytes),
    but receive advances its loop counter by 12 while advancing the payload
    pointer by 4. A 9-byte frame therefore delivers only its first record.
    Proven from SDK IR and the two-record host regression during #81 review;
    preserved. Actual TWS peer operation remains untested.

30. **TWS access-address scorer compares bytes at different bit positions.**
    `tws_le_acc.c`, `ble_sync_generation`: the byte-pattern flag compares
    `(word << 8) & 0xff00` directly to masks at bits 8..15, 16..23 and 24..31.
    All comparisons can hold only for word zero; repeated nonzero bytes do
    not set that flag. Proven from raw SDK shifts/masks during #83 review;
    preserved. Other score flags still apply. The generator and its effects
    on actual peer links are untested on hardware and removed by LTO here.

31. **BLE interspace search can read beyond its eight-entry array.**
    `multilink_schedule.c`, `find_interspace_between_connected`: after sorting
    the active distances, the gap search reads `distance[i + 1]` until it
    finds a sentinel or a gap greater than 1874 us, without a count bound.
    Eight occupied entries with no qualifying gap reach `distance[8]`.
    Proven from SDK IR during #85 review; preserved. The master multilink
    configuration is disabled and LTO removes this path in this firmware;
    its hardware effect remains untested.

32. **BLE polling trusts connection bookkeeping for timing and array sizes.**
    `multilink_schedule.c`, `mult_slave_or_master_polling_mode`: the selected
    link's clock and interval start as LLVM `undef` and are assigned only
    when an active state-6/7 link matches `opt_next_id`. Subsequent timing
    comparisons and divisions can use them without finding that link.
    Both this function and `mult_slave_or_master_event_enter` size their
    clock arrays from the saved connection count, then decrement a byte
    index for each active state-6/7 hardware link without checking the
    counts agree. Proven from SDK IR during #85 review; preserved. These
    scheduling paths are removed by LTO here; inconsistent bookkeeping and
    its hardware effects remain untested.

33. **TWS eSCO removal stores a state through a possibly null link.**
    `tws_pair.c`, `tws_master_notice_slave_step_fun` (event 2, step 0):
    `esco_conn->reason` is written only when `esco_conn` is nonnull, but the
    following `esco_conn->state = 5` has no check. Proven from SDK IR
    (`if.end161` stores through the unguarded pointer) during the second
    review of #78; preserved. Removed by LTO here; untested on hardware.

34. **BLE link creation continues after allocation failure.** `link_layer.c`,
    `ll_create_conn_link`: a null allocation only produces an optional diagnostic;
    execution then stores the hardware pointer through that null result. Proven
    from SDK IR during #87 review; preserved. Allocation failure and its hardware
    effect remain untested.

35. **BLE ACL fragmentation can make no progress with oversized chunks.**
    `link_layer.c`, `ll_send_acl_packet`: each chunk length is truncated to a byte
    before sending, advancing the pointer, and subtracting the remaining size.
    A maximum of 257 and a remaining size of 260 advance one byte repeatedly,
    then stall at remaining 256 with a zero-length chunk. Proven from SDK IR
    and a host regression checking the first two callbacks during #87 review;
    preserved. Whether configuration validation excludes these values is
    untested; the stall was not exercised on hardware.

36. **BLE link deletion clears only five bytes of its procedure FIFO.**
    `link_layer.c`, `ll_delete_link`: resets both FIFO indexes, but its memset
    length is 5 rather than the five-entry FIFO size. Proven from SDK IR during
    #87 review; preserved. The link is subsequently closed and freed; whether
    any close callback observes the remaining entries is untested.

37. **BLE instant validation can return with interrupts disabled.** `RF_ble5.c`,
    `le_hw_check_all_instant_will_be_valid`: disables IRQs before checking
    `bb_hdl`, then returns immediately when that handle is null without enabling
    IRQs. Proven from raw SDK IR during #88 review; preserved. Reachability with
    an absent baseband handle and its hardware effects remain untested.

38. **A failed BLE extended ring write clears available storage.** `RF_ble5.c`,
    `loop_cbuf_write`: when the request exceeds free space, clears that free
    space but returns zero and leaves the cursors and readable count unchanged.
    Proven from SDK IR and the rejected-write host fixture during #88 review;
    preserved. Effects on DMA timing and concurrent consumption are untested.

39. **BLE TX completion can dereference a slot cleared by its probe callback.**
    `RF_ble.c`, `__hw_tx_buf_proess`: after calling `tx_probe_handler`, reloads
    the selected TX slot and accesses its `ack_callback` without checking the
    reloaded pointer. A callback that clears the slot can trigger a null access.
    Proven from raw SDK IR during #89 review; preserved. Callback clearing and
    its hardware effects remain untested.
