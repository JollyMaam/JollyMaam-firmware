"""Small BR23 archive hooks for src/btstack_qos_jieli.c.

The HCI link policy definition is removed in favour of owned C. Validated
ordinary LMP receive dispatch and small opaque SDK layout accessors remain.
Match the vendor IR strictly: fail on SDK drift.
"""
import re


from .ir import function, extract
from .abi import default_policy_exports


def hci(text):
    # Replace the complete dispatch boundary, not a single optimized block.
    # No old optimizer attributes are carried to the external implementation.
    start, end = function(text, 'hci_lmp_link_policy_cmd_handler')
    body = text[start:end]
    expected = (2051, 2052, 2055, 2057, 2059, 2060, 2061, 2062, 2063,
                2053, 2054, 2064, 2065)
    actual = tuple(int(n) for n in re.findall(r'i16 (\d+), label', body))
    if set(actual) != set(expected) or len(actual) != len(expected):
        raise RuntimeError('controller: link policy command set changed')
    if 'HCI_QOS_SETUP is NOT supported' not in text:
        raise RuntimeError('controller: original QoS stub changed')
    # The SDK stores only the low byte of default policy. Keep that ABI.
    field = ('getelementptr inbounds (%struct.bredr_local_device, '
             '%struct.bredr_local_device* @gbredr_local_dev, i32 0, i32 1)')
    if body.count(field) != 3:
        raise RuntimeError('controller: default policy layout changed')
    text, _ = extract(text, 'hci_lmp_link_policy_cmd_handler',
                      'i32 @hci_lmp_link_policy_cmd_handler(i16 zeroext, i8*, i32)',
                      '7a6101272ccd7846b5ad45b52b5363edb3e0446a340d9f4851f6b98fa4455c11')
    return text + default_policy_exports()


def lmp(text):
    start, end = function(text, 'lmp_acl_c_handler')
    body = text[start:end]
    anchor = '  br i1 %tobool32, label %sw.epilog, label %if.then33'
    if body.count(anchor) != 1:
        raise RuntimeError('QoS: validated LMP dispatch changed')
    replacement = '''  br i1 %tobool32, label %sw.epilog, label %cyclone2_qos_rx_valid

cyclone2_qos_rx_valid:
  %cyclone2_qos_handle_ptr = getelementptr inbounds %struct.lmp_connection, %struct.lmp_connection* %conn, i32 0, i32 29, i32 4
  %cyclone2_qos_handle = load i16, i16* %cyclone2_qos_handle_ptr, align 4
  %cyclone2_qos_role_ptr = getelementptr inbounds %struct.lmp_connection, %struct.lmp_connection* %conn, i32 0, i32 28, i32 2
  %cyclone2_qos_role = load i8, i8* %cyclone2_qos_role_ptr, align 2
  %cyclone2_qos_payload = getelementptr inbounds %struct.bredr_rx_bulk, %struct.bredr_rx_bulk* %rx, i32 0, i32 8, i32 1
  %cyclone2_qos_consumed = call i32 @cyclone2_qos_rx(i16 zeroext %cyclone2_qos_handle, i8 zeroext %cyclone2_qos_role, i8 zeroext %0, i8* %cyclone2_qos_payload, i16 zeroext %50)
  %cyclone2_qos_handled = icmp ne i32 %cyclone2_qos_consumed, 0
  br i1 %cyclone2_qos_handled, label %sw.epilog, label %if.then33'''
    dbg = re.search(re.escape(anchor) + r", !dbg !(\d+)", body).group(1)
    replacement = replacement.replace("i8* %cyclone2_qos_payload, i16 zeroext %50)",
                                      f"i8* %cyclone2_qos_payload, i16 zeroext %50), !dbg !{dbg}")
    # Preserve the original branch's debug metadata on the final branch.
    body = body.replace(anchor, replacement)
    text = text[:start] + body + text[end:]
    # These layout fields are also used by the vendor's event and request
    # routines; refuse to guess an ABI after a vendor archive update.
    for marker in ('i32 0, i32 29, i32 4', 'i32 0, i32 28, i32 2', 'i32 0, i32 28, i32 13'):
        if marker not in text[:start]:
            raise RuntimeError(f'QoS: connection layout changed ({marker})')
    return text + '\ndeclare i32 @cyclone2_qos_rx(i16 zeroext, i8 zeroext, i8 zeroext, i8*, i16 zeroext)\n'
