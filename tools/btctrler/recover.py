"""Recover complete controller policy boundaries before other IR patches.

This removes original definitions, exports opaque layout accessors, and keeps
SDK assertion/role dispatch as a retained helper. Policy is implemented in C.
"""
import re
from .ir import extract
from .abi import exports, device_manager_exports, classic_baseband_exports

BODIES = {
    'lmp_conn_for_address': 'b9f882812945540261c826ed1b8dfa721ec53fdee0f960a4a16e233e4c1d785c',
    'lmp_hci_cmd_addr_to_conn': 'b67a98ba022eb9150c51c971e292654f51887ab4b54d79b5f3a2911c190912cb',
    'lmp_tx_qos_req': '339fe3b648801859e9c4512132d3d5de981379df78dafac1ae4b3045e81e1689',
    'lmp_vendor_qos_interval_config': 'aab7bdb10cf8728cc97563fdcbaa418bdd06279d7318733f377b6d94e6d2d535',
    'lmp_event_esco_link_setup': '923b32a5ea7bd01ede5dfbf26f4f3713a6c3f88aed60f84c3628a474cde9dcbe',
    'lmp_tx_supervision_timeout': '4172dc19c2e9273ac9eeef8b0761b017599bf8e4d88797803c6a159f05938bfb',
}

def recover(text):
    bodies = {}
    for name, expected in BODIES.items():
        signature = {
            'lmp_conn_for_address': '%struct.lmp_connection* @lmp_conn_for_address(i8*)',
            'lmp_hci_cmd_addr_to_conn': 'void @lmp_hci_cmd_addr_to_conn(i8*, i32*)',
            'lmp_tx_supervision_timeout': 'void @lmp_tx_supervision_timeout(%struct.lmp_connection*)',
            'lmp_tx_qos_req': 'void @lmp_tx_qos_req(%struct.lmp_connection*)',
            'lmp_vendor_qos_interval_config': 'void @lmp_vendor_qos_interval_config(i16 zeroext)',
            'lmp_event_esco_link_setup': 'void @lmp_event_esco_link_setup(%struct.lmp_connection*)',
        }[name]
        text, bodies[name] = extract(text, name, signature, expected)
    global_interval = re.compile(r'^@set_qos_poll_interval = internal unnamed_addr global i16 0[^\n]*', re.M)
    if len(global_interval.findall(text)) != 1:
        raise RuntimeError('controller: QoS interval storage changed')
    text = global_interval.sub('@btctrler_qos_poll_slots = external global i16', text)
    text = text.replace('@set_qos_poll_interval', '@btctrler_qos_poll_slots')
    if '@lmp_acl_link = internal global [1 x %struct.lmp_connection]' not in text:
        raise RuntimeError('controller: expected exactly one SDK ACL slot')
    # Preserve the SDK's assertion/reset and role-machine dispatch verbatim,
    # after owned C has selected the connection. No guessed assert ABI in C.
    route = bodies['lmp_hci_cmd_addr_to_conn']
    anchor = '  %0 = load i32, i32* @config_asser'
    if route.count(anchor) != 1:
        raise RuntimeError('controller: route assertion boundary changed')
    retained = 'entry:\n' + route[route.index(anchor):]
    retained = '\n'.join(line for line in retained.splitlines() if '@llvm.dbg.' not in line)
    retained = re.sub(r', !dbg !\d+', '', retained)
    retained = re.sub(r'%call\b', '%conn', retained)
    text += '\ndefine void @BTCTRLER_BIN_route_selected(%struct.lmp_connection* %conn, i32* %msg) {\n' + retained + '\n}\n'
    return text + exports()


def link_control(text):
    """Recover the complete Classic link-control command dispatcher."""
    text, _ = extract(text, 'hci_lmp_link_control_cmd_handler',
                      'i32 @hci_lmp_link_control_cmd_handler(i16 zeroext, i8*, i32)',
                      'dc833ea8093f6c850f397ef6ce20b12bd4da6c43ef027e8bc783d9d78eb19797')
    return text


def command_dispatch(text):
    """Own dispatch with C ABI calls; retain SDK assertion/error completion."""
    text, body = extract(text, 'hci_command_process',
                         'void @hci_command_process(i8*, i32)',
                         '1c2e8fc4dd223408b67f8bb083901dc052c02c5d8c33bd61c9b9c5aac0bfe5ac')
    # The original is internal fastcc. Both callers must become ordinary C
    # calls; no wrapper or duplicate dispatcher is left in the archive.
    pattern = r'(tail call) fastcc (void @hci_command_process\([^\n]+)'
    if len(re.findall(pattern, text)) != 2:
        raise RuntimeError('controller: HCI dispatch caller boundary changed')
    text = re.sub(pattern, r'\1 \2', text)
    # Keep the SDK's fatal contract and Unknown Command completion verbatim.
    # Replace only the old dispatch-result phi with the supplied C result.
    retained = body[body.index('do.body:'):]
    retained = '\n'.join(l for l in retained.splitlines() if '@llvm.dbg.' not in l)
    retained = re.sub(r', !dbg !\d+', '', retained)
    phi = r'^  %match\.0 = phi i32 \[[^\n]+'
    if len(re.findall(phi, retained, re.M)) != 1:
        raise RuntimeError('controller: HCI completion result changed')
    retained = re.sub(phi, '  %match.0 = add i32 %match, 0', retained, flags=re.M)
    retained = re.sub(r'%conv4\b', '%opcode', retained)
    # Numeric SSA identifiers must start at zero in each new function.
    # Name retained values instead of carrying numbering from the old body.
    retained = re.sub(r'%(\d+)\b', r'%sdk_\1', retained)
    text += '\ndefine void @BTCTRLER_BIN_finish_command(i32 %opcode, i32 %match, i1 zeroext %bad_group, i8* %cmd, i32 %size) {\nentry:\n  br i1 %bad_group, label %do.body, label %sw.epilog\n' + retained + '\n}\n'
    return text


def device_manager(text):
    for name, expected in (
        ('hci_controller_baseband_cmd_handler', '7705e47d21e9065fb2d7e2a2162182a6a197dab724ccf93eec0efb869e50e887'),
        ('hci_informational_parameters_cmd_handler', '5a9366246d31c131701c0e13eb7c48d4653f39ebbc7b186f9689351bbe9ef0c4'),
    ):
        text, _ = extract(text, name, f'i32 @{name}(i16 zeroext, i8*, i32)', expected)
    return text + device_manager_exports(text)


def classic_baseband(text):
    text, _ = extract(text, 'hci_lmp_controller_baseband_cmd_handler',
                      'i32 @hci_lmp_controller_baseband_cmd_handler(i16 zeroext, i8*, i32)',
                      '2a540812f4dd4802432b7513a7a48e91165b8b5e3e378dde1090c45bd323331f')
    return text + classic_baseband_exports(text)
