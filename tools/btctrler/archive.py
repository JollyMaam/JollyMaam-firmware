#!/usr/bin/env python3
"""Inspect retained BR23 LLVM code and record controller archive ownership.

IR is optimized SDK implementation, not reconstructed original C. Output is a
build artifact; never edits the shared SDK or copies its code into source git.
"""
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

# Functions owned C defines in place of the SDK's (each is removed from the
# archive by tools/btctrler/recover.py, against a fingerprint).
from .recover import HCI_LL_5_CMDS_OWNED, LL_WHITE_LIST_OWNED, BTENCRY_TASK_OWNED, THREAD_OWNED, BTCONTROLLER_HEAP_OWNED, SYS_TIMER_OWNED, COMMAND_DISPATCH_OWNED, LBUF_OWNED, LINK_BULK_OWNED, HCI_CONTROLLER_OWNED, H4_CONTROLLER_OWNED, HCI_LL_CMDS_OWNED, LMP_UTILS_OWNED, BREDR_UTILS_OWNED, BREDR_SLOT_TIMER_OWNED, MISC_HW_OWNED, AUTHENTICATION_OWNED, LMP_OWNED, DEVICE_MISC_OWNED, CLASSIC_MISC_OWNED, HCI_PROCEDURES_OWNED, TASK_QUEUE_OWNED, TASK_LIFECYCLE_OWNED, TASK_DISPATCH_OWNED, CONTROL_TX_OWNED, TWS_MEDIA_SYNC_OWNED, LOCAL_MEDIA_SYNC_OWNED, TWS_LL_ADV_OWNED, TWS_DATA_FORWARDING_OWNED
OWNED = tuple(HCI_LL_5_CMDS_OWNED) + tuple(LL_WHITE_LIST_OWNED) + tuple(BTENCRY_TASK_OWNED) + tuple(THREAD_OWNED) + tuple(BTCONTROLLER_HEAP_OWNED) + tuple(SYS_TIMER_OWNED) + tuple(COMMAND_DISPATCH_OWNED) + tuple(LBUF_OWNED) + tuple(LINK_BULK_OWNED) + tuple(HCI_CONTROLLER_OWNED) + tuple(H4_CONTROLLER_OWNED) + tuple(HCI_LL_CMDS_OWNED) + tuple(LMP_UTILS_OWNED) + tuple(BREDR_UTILS_OWNED) + tuple(BREDR_SLOT_TIMER_OWNED) + tuple(MISC_HW_OWNED) + tuple(AUTHENTICATION_OWNED) + tuple(LMP_OWNED) + tuple(DEVICE_MISC_OWNED) + tuple(CLASSIC_MISC_OWNED) + tuple(HCI_PROCEDURES_OWNED) + tuple(TASK_QUEUE_OWNED) + tuple(TASK_LIFECYCLE_OWNED) + tuple(TASK_DISPATCH_OWNED) + tuple(CONTROL_TX_OWNED) + tuple(TWS_MEDIA_SYNC_OWNED) + tuple(LOCAL_MEDIA_SYNC_OWNED) + tuple(TWS_LL_ADV_OWNED) + tuple(TWS_DATA_FORWARDING_OWNED) + (
    'hci_lmp_controller_baseband_cmd_handler', 'hci_controller_baseband_cmd_handler',
    'hci_informational_parameters_cmd_handler',
    'hci_lmp_link_control_cmd_handler', 'hci_lmp_link_policy_cmd_handler')
# This archive was inspected for pre-existing inline copies and call attributes.
# Changing it requires repeating that audit, not just matching one definition.
AUDITED_SDK_SHA256 = 'f63db0097b5d41096b3228ca038898f317694d6d293d1ba48beddfab85778869'

def require_audited_sdk(source):
    if digest(source) != AUDITED_SDK_SHA256:
        raise RuntimeError('controller: SDK archive changed; audit owned boundaries with controller-ir before updating the fingerprint')

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def definitions(text):
    return re.findall(r'^define [^\n]*@([^ (]+)\(', text, re.M)


def record(source, destination, members, originals, directory, dropped=()):
    changed = []
    for member in members:
        after = digest(directory / member)
        if originals[member] != after:
            changed.append(member)
    manifest = {
        'sdk_archive_sha256': digest(source),
        'generated_archive_sha256': digest(destination),
        'owned_definitions': list(OWNED),
        'changed_members': changed,
        'dropped_members': list(dropped),
        'members': [{'name': m, 'sdk_sha256': originals[m],
                     'generated_sha256': digest(directory / m)} for m in members],
    }
    destination.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

def inspect(source, output, clang, ar):
    output.mkdir(parents=True, exist_ok=True)
    for stale in output.glob('*.ll'):    # members dropped since the last run
        stale.unlink()
    members = subprocess.check_output([ar, 't', str(source)], text=True).splitlines()
    inventory = []
    with tempfile.TemporaryDirectory(prefix='cyclone2-controller-inspect-') as tmp:
        directory = Path(tmp)
        subprocess.run([ar, 'x', str(source)], cwd=directory, check=True)
        for member in members:
            if (directory / member).read_bytes().startswith(b'\x7fELF'):
                inventory.append({'member': member, 'sha256': digest(directory / member),
                                  'format': 'native ELF', 'definitions': [],
                                  'owned_inlined_locations': {}})
                continue
            ir = output / (member + '.ll')
            result = subprocess.run([clang, '-target', 'pi32v2', '-x', 'ir', '-S',
                                     '-emit-llvm', str(directory / member), '-o', str(ir)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f'{member}: unable to decode LLVM bitcode: {result.stderr}')
            text = ir.read_text()
            # Debug scopes identify embedded copies even when the old symbol
            # disappeared. Record locations, not just direct symbol references.
            inlined = {}
            for name in OWNED:
                scopes = re.findall(r'^!(\d+) = .*!DISubprogram\(name: "' +
                                    re.escape(name) + r'"[^\n]*', text, re.M)
                # Include lexical blocks inside the function's debug scope.
                while True:
                    children = [node for node, parent in re.findall(
                        r'^!(\d+) = (?:distinct )?!DILexicalBlock(?:File)?\([^\n]*scope: !(\d+)',
                        text, re.M) if parent in scopes and node not in scopes]
                    if not children: break
                    scopes += children
                locations = []
                for scope in scopes:
                    locations += re.findall(r'^!\d+ = !DILocation\([^\n]*scope: !' + scope +
                                            r', inlinedAt: !\d+[^\n]*', text, re.M)
                if locations: inlined[name] = locations
            inventory.append({'member': member, 'sha256': digest(directory / member),
                              'definitions': definitions(text),
                              'owned_symbol_references': {n: text.count('@' + n + '(') for n in OWNED
                                                          if '@' + n + '(' in text},
                              'owned_inlined_locations': inlined})
    (output / 'inventory.json').write_text(json.dumps({'archive_sha256': digest(source),
                                                      'members': inventory}, indent=2) + '\n')
    inline_members = [x['member'] for x in inventory if x['owned_inlined_locations']]
    print(f'{len(members)} members, {sum(len(x["definitions"]) for x in inventory)} definitions; '
          f'owned function inline evidence: {inline_members}')
