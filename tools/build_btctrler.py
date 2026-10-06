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
    'btctrler_task.c.o': (recover.task_queue, recover.task_lifecycle, recover.task_dispatch),
    'device_manager.c.o': (recover.device_manager, recover.device_misc),
    'hci_controller.c.o': (recover.command_dispatch,),
    'lmp.c.o': (recover.lmp_member,),
    'lmp_rxtx.c.o': (recover.control_tx,),
    'lmp_hci.c.o': (recover.hci_procedures,),
    'lmp_hci_cmd_parser.c.o': (recover.link_control, recover.classic_baseband, recover.link_policy, recover.classic_misc),
}
# Members owned C replaces completely: checked by their transforms above,
# then left out of the archive.
DROPPED = ('lmp.c.o',)


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
