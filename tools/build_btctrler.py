#!/usr/bin/env python3
"""Build the single retained BR23 controller archive used by this firmware."""
import argparse
import subprocess
import tempfile
from pathlib import Path
from btctrler import archive, recover
from btctrler.ir import rewrite_member

# Each member's SDK definitions are removed from pristine IR: the
# fingerprints are of the SDK's bodies.
PIPELINE = {
    'nv_memory.c.o': (recover.nv_memory,),
    'tws_data_forwarding.c.o': (recover.tws_data_forwarding,),
    'tws_tx_sync.c.o': (recover.tws_tx_sync,),
    'tws_ll_adv.c.o': (recover.tws_ll_adv,),
    'local_media_sync.c.o': (recover.local_media_sync,),
    'tws_media_sync.c.o': (recover.tws_media_sync,),
    'll_cis.c.o': (recover.ll_cis,),
    'll_keys.c.o': (recover.ll_keys,),

    'll_pc.c.o': (recover.ll_pc,),
    'll_bis.c.o': (recover.ll_bis,),
    'll_bis_master.c.o': (recover.ll_bis_master,),
    'll_common_iso.c.o': (recover.ll_common_iso,),
    'll_data_extension.c.o': (recover.ll_data_extension,),
    'll_master.c.o': (recover.ll_master,),
    'll_slave.c.o': (recover.ll_slave,),
    'll_afh.c.o': (recover.ll_afh,),
    'll_multiple_phys.c.o': (recover.ll_multiple_phys,),
    'll_extended_advertising.c.o': (recover.ll_extended_advertising,),
    'll_ext_init.c.o': (recover.ll_ext_init,),
    'hci_ll_5_cmds.c.o': (recover.hci_ll_5_cmds,),
    'll_white_list.c.o': (recover.ll_white_list,),
    'btencry_task.c.o': (recover.btencry_task,),
    'thread.c.o': (recover.thread,),
    'btcontroller_heap.c.o': (recover.btcontroller_heap,),
    'sys_timer.c.o': (recover.sys_timer,),
    'btctrler_task.c.o': (recover.task_queue, recover.task_lifecycle, recover.task_dispatch),
    'device_manager.c.o': (recover.device_manager, recover.device_misc),
    'hci_device_manager.c.o': (recover.hci_device_manager_unused,),
    'hci_ll_cmds.c.o': (recover.hci_ll_cmds,),
    'hci_controller.c.o': (recover.hci_controller,),
    'h4_controller_interface.c.o': (recover.h4_controller,),
    'lmp.c.o': (recover.lmp_member,),
    'lmp_rxtx.c.o': (recover.control_tx,),
    'lmp_hci.c.o': (recover.hci_procedures,),
    'lmp_utils.c.o': (recover.lmp_utils,),
    'bredr_utils.c.o': (recover.bredr_utils,),
    'll_vendor.c.o': (recover.ll_vendor,),
    'bredr_slot_timer.c.o': (recover.bredr_slot_timer,),
    'bredr_low_power.c.o': (recover.bredr_low_power,),
    'le_low_power.c.o': (recover.le_low_power,),
    'bredr_link.c.o': (recover.bredr_link,),
    'link_task.c.o': (recover.link_task,),
    'bredr_frame.c.o': (recover.bredr_frame,),
    'misc_hw.c.o': (recover.misc_hw,),
    'authentication.c.o': (recover.authentication,),
    'link_bulk.c.o': (recover.link_bulk,),
    'lbuf.c.o': (recover.lbuf,),
    'lmp_hci_cmd_parser.c.o': (recover.link_control, recover.classic_baseband, recover.link_policy, recover.classic_misc),
    'misc_hw_common.c.o': (recover.misc_hw_common,),
    'hci_ll.c.o': (recover.hci_ll,),
    'll_adv.c.o': (recover.ll_adv,),
    'll_events.c.o': (recover.ll_events,),
    'btcontroller_util.c.o': (recover.btcontroller_util,),
    'bredr_dut.c.o': (recover.bredr_dut,),
    'bredr_fcc.c.o': (recover.bredr_fcc,),
    'btagc.c.o': (recover.btagc,),

    'analog_ble.c.o': (recover.analog_ble,),
    'direct_test_dut.c.o': (recover.direct_test_dut,),
    'aes_hw.c.o': (recover.aes_hw,),
    'aes128.c.o': (recover.aes128,),
    'hw_dma.c.o': (recover.hw_dma,),
    'vendor_manager.c.o': (recover.vendor_manager,),
    'vsprintf.c.o': (recover.vsprintf,),
    'interrupt.c.o': (recover.interrupt,),
}
# Members owned C replaces completely: checked by their transforms above,
# then left out of the archive.
DROPPED = ('bredr_frame.c.o', 'link_task.c.o', 'bredr_low_power.c.o', 'le_low_power.c.o', 'bredr_slot_timer.c.o', 'tws_data_forwarding.c.o', 'tws_ll_adv.c.o', 'local_media_sync.c.o', 'tws_media_sync.c.o', 'hci_ll_5_cmds.c.o', 'll_white_list.c.o', 'btencry_task.c.o', 'thread.c.o', 'btcontroller_heap.c.o', 'sys_timer.c.o', 'authentication.c.o', 'bredr_utils.c.o', 'btctrler_task.c.o', 'device_manager.c.o', 'h4_controller_interface.c.o', 'll_ext_init.c.o', 'll_extended_advertising.c.o', 'll_master.c.o', 'll_data_extension.c.o', 'll_common_iso.c.o', 'll_bis.c.o', 'll_cis.c.o', 'll_keys.c.o', 'hci_controller.c.o', 'lbuf.c.o', 'link_bulk.c.o', 'lmp.c.o', 'lmp_hci.c.o', 'lmp_hci_cmd_parser.c.o', 'lmp_rxtx.c.o', 'lmp_utils.c.o', 'misc_hw.c.o', 'misc_hw_common.c.o', 'hci_ll_cmds.c.o', 'vsprintf.c.o', 'vendor_manager.c.o', 'aes128.c.o', 'aes_hw.c.o', 'bredr_dut.c.o', 'bredr_fcc.c.o', 'btcontroller_util.c.o', 'hci_ll.c.o', 'hw_dma.c.o', 'll_slave.c.o', 'll_multiple_phys.c.o', 'll_adv.c.o', 'll_events.c.o', 'direct_test_dut.c.o', 'bredr_link.c.o', 'btagc.c.o', 'analog_ble.c.o', 'tws_tx_sync.c.o', 'hci_device_manager.c.o', 'interrupt.c.o', 'll_pc.c.o', 'll_bis_master.c.o', 'nv_memory.c.o', 'll_vendor.c.o', 'll_afh.c.o')

# Members the firmware link never pulls in: no symbol they define is
# referenced.  Left out without translation; if owned C ever references one
# of their symbols, the link fails with an undefined reference.
UNLINKED = ('access_addr.c.o', 'au_law_codec.c.o', 'audio_codec.c.o',
            'bredr_RF.c.o', 'btcontroller_linked_list.c.o',
            'cte_data_algorithm.c.o', 'esco_codec.c.o', 'h4_uart_hw.c.o',
            'h4_uart_interface_loop.c.o', 'h4_usb_descriptor.c.o',
            'h4_usb_interface.c.o', 'hopping_channel.c.o', 'll_bis_slave.c.o',
            'll_cis_master.c.o', 'll_cis_slave.c.o', 'll_cte.c.o', 'll_msg.c.o',
            'tws_ci_data.c.o', 'tws_low_latency.c.o')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('destination', type=Path)
    p.add_argument('--clang', required=True)
    p.add_argument('--ar', required=True)
    args = p.parse_args()
    source, destination = args.source.resolve(), args.destination.resolve()
    if source == destination:
        raise ValueError('controller: destination must be a build copy, not the SDK archive')
    archive.require_audited_sdk(source)
    members = subprocess.check_output([args.ar, 't', str(source)], text=True).splitlines()
    missing = (set(PIPELINE) | set(DROPPED) | set(UNLINKED)) - set(members)
    if missing:
        raise RuntimeError(f'controller: missing archive members: {sorted(missing)}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cyclone2-controller-', dir=destination.parent) as name:
        directory = Path(name)
        subprocess.run([args.ar, 'x', str(source)], cwd=directory, check=True)
        originals = {m: archive.digest(directory / m) for m in members}
        for member, transforms in PIPELINE.items():
            rewrite_member(directory / member, args.clang, transforms)
        kept = [m for m in members if m not in DROPPED + UNLINKED]
        # Rebuild from scratch so stale members cannot survive SDK changes.
        temporary = directory / 'retained.a'
        subprocess.run([args.ar, 'rcs', str(temporary), *kept], cwd=directory, check=True)
        temporary.replace(destination)
        archive.record(source, destination, kept, originals, directory, DROPPED + UNLINKED)


if __name__ == '__main__':
    main()
