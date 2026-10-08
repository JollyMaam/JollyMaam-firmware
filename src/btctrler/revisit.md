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


3. **Controller scheduler sleeps with an uninitialized timeout.**
   `thread.c`, `thread_schedule`: the suspend timeout starts undefined (LLVM
   `undef`) and is min-ed with each thread's timeout, then passed to
   `owner->suspend`.  Proven in the SDK IR and in our final LTO IR (unchanged
   by owning it).  The value depends on register allocation, so the
   controller thread may wake early or sleep wrongly.  Untested whether it
   matters; a candidate in the Classic sniff wedge and latency work.  Fix
   deliberately (start from the largest timeout) with a hardware A/B.


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

16. **The Switch 2 goes silent on Classic when it sleeps.**  With slot A
    connected, putting the console to sleep ends the link by supervision
    timeout (0x08), on the baseline and on owned code alike; on LE it sends a
    proper disconnect (0x13).  The pad then treats it as a lost link,
    reconnects, fails, tries the wake, and goes idle about 30 s later.  A
    firmware decision, not a controller bug: consider going idle (or fewer
    retries) after a timeout from a Switch host.

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
