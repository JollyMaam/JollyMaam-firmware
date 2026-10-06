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

OWNED = {'hci_lmp_controller_baseband_cmd_handler': 'src/btctrler_classic_baseband.c',
         'hci_controller_baseband_cmd_handler': 'src/btctrler_device_manager.c',
         'hci_informational_parameters_cmd_handler': 'src/btctrler_device_manager.c',
         'hci_command_process': 'src/btctrler_hci_dispatch.c',
         'hci_lmp_link_control_cmd_handler': 'src/btctrler_link_control.c',
         'hci_lmp_link_policy_cmd_handler': 'src/btctrler_link_policy.c',
         'lmp_tx_supervision_timeout': 'src/btctrler_link_policy.c',
         'lmp_conn_for_address': 'src/btctrler_address.c',
         'lmp_hci_cmd_addr_to_conn': 'src/btctrler_address.c',
         'lmp_tx_qos_req': 'src/btctrler_link_policy.c',
         'lmp_vendor_qos_interval_config': 'src/btctrler_link_policy.c',
         'lmp_event_esco_link_setup': 'src/btctrler_link_policy.c'}
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


def binary_bindings():
    """Source call names map directly to SDK symbols through asm labels."""
    source = Path(__file__).resolve().parents[2] / 'src'
    bindings = {}
    for path in sorted(source.glob('*.[ch]')):
        for name, symbol in re.findall(
                r'\b(BTCTRLER_BIN_\w+)\b[^;]*?__asm__\("(\w+)"\);', path.read_text()):
            entry = bindings.setdefault(name, {'symbol': symbol, 'declarations': []})
            if entry['symbol'] != symbol:
                raise RuntimeError(f'controller: inconsistent binary binding {name}')
            entry['declarations'].append('src/' + path.name)
    return bindings

def record(source, destination, members, originals, directory):
    changed = []
    for member in members:
        after = digest(directory / member)
        if originals[member] != after:
            changed.append(member)
    manifest = {
        'sdk_archive_sha256': digest(source),
        'generated_archive_sha256': digest(destination),
        'owned_definitions': OWNED,
        'owned_wrappers': ['cyclone2_lmp_qos_send', 'cyclone2_lmp_qos_answer'],
        'binary_bindings': binary_bindings(),
        'retained_fragments': {'BTCTRLER_BIN_route_selected': 'lmp_hci_cmd_addr_to_conn assertion/dispatch',
                               'BTCTRLER_BIN_finish_command': 'hci_command_process assertion/error completion'},
        'changed_members': changed,
        'members': [{'name': m, 'sdk_sha256': originals[m],
                     'generated_sha256': digest(directory / m)} for m in members],
    }
    destination.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

def inspect(source, output, clang, ar):
    output.mkdir(parents=True, exist_ok=True)
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
