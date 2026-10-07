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
    'hci_ll_5_cmds.c.o': (recover.hci_ll_5_cmds,),
    'll_white_list.c.o': (recover.ll_white_list,),
    'btencry_task.c.o': (recover.btencry_task,),
    'thread.c.o': (recover.thread,),
    'btcontroller_heap.c.o': (recover.btcontroller_heap,),
    'sys_timer.c.o': (recover.sys_timer,),
    'btctrler_task.c.o': (recover.task_queue, recover.task_lifecycle, recover.task_dispatch),
    'device_manager.c.o': (recover.device_manager, recover.device_misc),
    'hci_ll_cmds.c.o': (recover.hci_ll_cmds,),
    'hci_controller.c.o': (recover.hci_controller,),
    'h4_controller_interface.c.o': (recover.h4_controller,),
    'lmp.c.o': (recover.lmp_member,),
    'lmp_rxtx.c.o': (recover.control_tx,),
    'lmp_hci.c.o': (recover.hci_procedures,),
    'lmp_utils.c.o': (recover.lmp_utils,),
    'bredr_utils.c.o': (recover.bredr_utils,),
    'bredr_slot_timer.c.o': (recover.bredr_slot_timer,),
    'misc_hw.c.o': (recover.misc_hw,),
    'authentication.c.o': (recover.authentication,),
    'link_bulk.c.o': (recover.link_bulk,),
    'lbuf.c.o': (recover.lbuf,),
    'lmp_hci_cmd_parser.c.o': (recover.link_control, recover.classic_baseband, recover.link_policy, recover.classic_misc),
    'misc_hw_common.c.o': (recover.misc_hw_common,),
}
# Members owned C replaces completely: checked by their transforms above,
# then left out of the archive.
DROPPED = ('hci_ll_5_cmds.c.o', 'll_white_list.c.o', 'btencry_task.c.o', 'thread.c.o', 'btcontroller_heap.c.o', 'sys_timer.c.o', 'authentication.c.o', 'bredr_utils.c.o', 'btctrler_task.c.o', 'device_manager.c.o', 'h4_controller_interface.c.o',
           'hci_controller.c.o', 'lbuf.c.o', 'link_bulk.c.o', 'lmp.c.o', 'lmp_hci.c.o', 'lmp_hci_cmd_parser.c.o',
           'lmp_rxtx.c.o', 'lmp_utils.c.o', 'misc_hw.c.o', 'misc_hw_common.c.o', 'hci_ll_cmds.c.o')


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
    missing = (set(PIPELINE) | set(DROPPED)) - set(members)
    if missing:
        raise RuntimeError(f'controller: missing archive members: {sorted(missing)}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cyclone2-controller-', dir=destination.parent) as name:
        directory = Path(name)
        subprocess.run([args.ar, 'x', str(source)], cwd=directory, check=True)
        originals = {m: archive.digest(directory / m) for m in members}
        for member, transforms in PIPELINE.items():
            rewrite_member(directory / member, args.clang, transforms)
        kept = [m for m in members if m not in DROPPED]
        # Rebuild from scratch so stale members cannot survive SDK changes.
        temporary = directory / 'retained.a'
        subprocess.run([args.ar, 'rcs', str(temporary), *kept], cwd=directory, check=True)
        temporary.replace(destination)
        archive.record(source, destination, kept, originals, directory, DROPPED)


if __name__ == '__main__':
    main()
