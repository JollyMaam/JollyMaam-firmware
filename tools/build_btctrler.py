#!/usr/bin/env python3
"""Build the single retained BR23 controller archive used by this firmware."""
import argparse
import subprocess
import tempfile
from pathlib import Path
from btctrler import archive, fixes, qos, recover, trace
from btctrler.ir import rewrite_member

# Ordering is intentional: recover pristine definitions before applying the
# remaining fixes; validated QoS interception runs after those LMP edits.
PIPELINE = {
    'device_manager.c.o': (recover.device_manager,),
    'hci_controller.c.o': (recover.command_dispatch,),
    'lmp.c.o': (recover.recover, fixes.notify_remote_ssp_key,
                fixes.drain_detach_rx, trace.authentication,
                trace.remote_auth_stage,
                trace.detach_flow, qos.lmp),
    'lmp_hci_cmd_parser.c.o': (recover.link_control, recover.classic_baseband, qos.hci,),
    'lmp_rxtx.c.o': (trace.lmp_send,),
    'll_slave.c.o': (trace.le_receive,),
    'link_layer.c.o': (trace.le_control_send,),
}


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
    missing = set(PIPELINE) - set(members)
    if missing:
        raise RuntimeError(f'controller: missing archive members: {sorted(missing)}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cyclone2-controller-', dir=destination.parent) as name:
        directory = Path(name)
        subprocess.run([args.ar, 'x', str(source)], cwd=directory, check=True)
        originals = {m: archive.digest(directory / m) for m in members}
        for member, transforms in PIPELINE.items():
            rewrite_member(directory / member, args.clang, transforms)
        # Rebuild from scratch so stale members cannot survive SDK changes.
        temporary = directory / 'retained.a'
        subprocess.run([args.ar, 'rcs', str(temporary), *members], cwd=directory, check=True)
        temporary.replace(destination)
        archive.record(source, destination, members, originals, directory)


if __name__ == '__main__':
    main()
