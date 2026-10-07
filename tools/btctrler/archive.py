#!/usr/bin/env python3
"""Inspect retained BR23 LLVM code and record controller archive ownership.

IR is optimized SDK implementation, not reconstructed original C. Output is a
build artifact; never edits the shared SDK or copies its code into source git.
"""
import hashlib
import json
import re
import subprocess
from pathlib import Path

# Functions owned C defines in place of the SDK's (each is removed from the
# archive by tools/btctrler/recover.py, against a fingerprint).
from .recover import LL_KEYS_OWNED, HCI_LL_5_CMDS_OWNED, LL_WHITE_LIST_OWNED, BTENCRY_TASK_OWNED, THREAD_OWNED, BTCONTROLLER_HEAP_OWNED, SYS_TIMER_OWNED, COMMAND_DISPATCH_OWNED, LBUF_OWNED, LINK_BULK_OWNED, HCI_CONTROLLER_OWNED, H4_CONTROLLER_OWNED, HCI_LL_CMDS_OWNED, LMP_UTILS_OWNED, BREDR_UTILS_OWNED, BREDR_SLOT_TIMER_OWNED, BREDR_LOW_POWER_OWNED, LE_LOW_POWER_OWNED, LINK_TASK_OWNED, BREDR_FRAME_OWNED, MISC_HW_OWNED, AUTHENTICATION_OWNED, LMP_OWNED, DEVICE_MISC_OWNED, CLASSIC_MISC_OWNED, HCI_PROCEDURES_OWNED, TASK_QUEUE_OWNED, TASK_LIFECYCLE_OWNED, TASK_DISPATCH_OWNED, CONTROL_TX_OWNED, TWS_MEDIA_SYNC_OWNED, LOCAL_MEDIA_SYNC_OWNED, TWS_LL_ADV_OWNED, TWS_DATA_FORWARDING_OWNED, LL_EXT_INIT_OWNED, LL_EXTENDED_ADVERTISING_OWNED, LL_MASTER_OWNED, LL_DATA_EXTENSION_OWNED, LL_COMMON_ISO_OWNED, LL_BIS_OWNED, LL_CIS_OWNED, VSPRINTF_OWNED, VENDOR_MANAGER_OWNED, AES128_OWNED, AES_HW_OWNED, BREDR_DUT_OWNED, BTCONTROLLER_UTIL_OWNED, HCI_LL_OWNED, HW_DMA_OWNED, LL_SLAVE_OWNED, LL_MULTIPLE_PHYS_OWNED, LL_ADV_OWNED, LL_EVENTS_OWNED, DIRECT_TEST_DUT_OWNED, BREDR_LINK_OWNED, BTAGC_OWNED, ANALOG_BLE_OWNED, TWS_TX_SYNC_OWNED, LL_PC_OWNED, LL_BIS_MASTER_OWNED, NV_MEMORY_OWNED
OWNED = tuple(LL_KEYS_OWNED) + tuple(HCI_LL_5_CMDS_OWNED) + tuple(LL_WHITE_LIST_OWNED) + tuple(BTENCRY_TASK_OWNED) + tuple(THREAD_OWNED) + tuple(BTCONTROLLER_HEAP_OWNED) + tuple(SYS_TIMER_OWNED) + tuple(COMMAND_DISPATCH_OWNED) + tuple(LBUF_OWNED) + tuple(LINK_BULK_OWNED) + tuple(HCI_CONTROLLER_OWNED) + tuple(H4_CONTROLLER_OWNED) + tuple(HCI_LL_CMDS_OWNED) + tuple(LMP_UTILS_OWNED) + tuple(BREDR_UTILS_OWNED) + tuple(BREDR_SLOT_TIMER_OWNED) + tuple(BREDR_LOW_POWER_OWNED) + tuple(LE_LOW_POWER_OWNED) + tuple(LINK_TASK_OWNED) + tuple(BREDR_FRAME_OWNED) + tuple(MISC_HW_OWNED) + tuple(AUTHENTICATION_OWNED) + tuple(LMP_OWNED) + tuple(DEVICE_MISC_OWNED) + tuple(CLASSIC_MISC_OWNED) + tuple(HCI_PROCEDURES_OWNED) + tuple(TASK_QUEUE_OWNED) + tuple(TASK_LIFECYCLE_OWNED) + tuple(TASK_DISPATCH_OWNED) + tuple(CONTROL_TX_OWNED) + tuple(TWS_MEDIA_SYNC_OWNED) + tuple(LOCAL_MEDIA_SYNC_OWNED) + tuple(TWS_LL_ADV_OWNED) + tuple(TWS_DATA_FORWARDING_OWNED) + tuple(LL_EXT_INIT_OWNED) + tuple(LL_EXTENDED_ADVERTISING_OWNED) + tuple(LL_MASTER_OWNED) + tuple(LL_DATA_EXTENSION_OWNED) + tuple(LL_COMMON_ISO_OWNED) + tuple(LL_BIS_OWNED) + tuple(LL_CIS_OWNED) + tuple(VSPRINTF_OWNED) + tuple(VENDOR_MANAGER_OWNED) + tuple(AES128_OWNED) + tuple(AES_HW_OWNED) + tuple(BREDR_DUT_OWNED) + tuple(BTCONTROLLER_UTIL_OWNED) + tuple(HCI_LL_OWNED) + tuple(HW_DMA_OWNED) + tuple(LL_SLAVE_OWNED) + tuple(LL_MULTIPLE_PHYS_OWNED) + tuple(LL_ADV_OWNED) + tuple(LL_EVENTS_OWNED) + tuple(DIRECT_TEST_DUT_OWNED) + tuple(BREDR_LINK_OWNED) + tuple(BTAGC_OWNED) + tuple(ANALOG_BLE_OWNED) + tuple(TWS_TX_SYNC_OWNED) + tuple(LL_PC_OWNED) + tuple(LL_BIS_MASTER_OWNED) + tuple(NV_MEMORY_OWNED) + (
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
    from .inspection import inspect as inspect_cached
    inspect_cached(source, output, clang, ar, OWNED)
