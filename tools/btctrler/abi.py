"""Typed SDK-layout accessors. These are generated glue, not recovered C structs."""

def load(name, kind, pointer, alignment):
    extension = 'zeroext ' if kind in ('i8', 'i16') else ''
    return f'''\ndefine {extension}{kind} @BTCTRLER_ABI_{name}() {{
entry:
  %value = load {kind}, {kind}* {pointer}, align {alignment}
  ret {kind} %value
}}
'''


def exports():
    text = '''
define %struct.lmp_connection* @BTCTRLER_ABI_acl_slot() {
entry:
  ret %struct.lmp_connection* getelementptr inbounds ([1 x %struct.lmp_connection], [1 x %struct.lmp_connection]* @lmp_acl_link, i32 0, i32 0)
}
define i32 @BTCTRLER_ABI_connection_has_link(%struct.lmp_connection* %conn) {
entry:
  %ptr = getelementptr inbounds %struct.lmp_connection, %struct.lmp_connection* %conn, i32 0, i32 30
  %link = load %struct.link_connection*, %struct.link_connection** %ptr, align 4
  %present = icmp ne %struct.link_connection* %link, null
  %result = zext i1 %present to i32
  ret i32 %result
}
define i8* @BTCTRLER_ABI_connection_address(%struct.lmp_connection* %conn) {
entry:
  %address = getelementptr inbounds %struct.lmp_connection, %struct.lmp_connection* %conn, i32 0, i32 29, i32 1, i32 0
  ret i8* %address
}
'''
    # (name, SDK struct path, field type, load alignment)
    fields = (('connection_handle', '29, i32 4', 'i16', 4),
              ('connection_role', '28, i32 2', 'i8', 2),
              ('connection_step', '28, i32 1', 'i8', 1),
              ('connection_state', '28, i32 13', 'i32', 4))
    for name, path, kind, alignment in fields:
        extension = 'zeroext ' if kind in ('i8', 'i16') else ''
        text += f'''
define {extension}{kind} @BTCTRLER_ABI_{name}(%struct.lmp_connection* %conn) {{
entry:
  %ptr = getelementptr inbounds %struct.lmp_connection, %struct.lmp_connection* %conn, i32 0, i32 {path}
  %value = load {kind}, {kind}* %ptr, align {alignment}
  ret {kind} %value
}}
'''
    text += '''
define void @BTCTRLER_ABI_connection_state_set(%struct.lmp_connection* %conn, i32 %state) {
entry:
  %ptr = getelementptr inbounds %struct.lmp_connection, %struct.lmp_connection* %conn, i32 0, i32 28, i32 13
  store i32 %state, i32* %ptr, align 4
  ret void
}
'''
    pointer = ('getelementptr inbounds (%struct.bredr_local_device, '
               '%struct.bredr_local_device* @gbredr_local_dev, i32 0, i32 10)')
    return text + load('supervision_ms', 'i16', pointer, 4)


def default_policy_exports():
    pointer = ('getelementptr inbounds (%struct.bredr_local_device, '
               '%struct.bredr_local_device* @gbredr_local_dev, i32 0, i32 1)')
    return load('default_policy_get', 'i8', pointer, 1) + f'''
define void @BTCTRLER_ABI_default_policy_set(i8 zeroext %policy) {{
entry:
  store i8 %policy, i8* {pointer}, align 1
  ret void
}}
'''


def device_manager_exports(text):
    """Expose audited fields using the SDK LLVM type, not C struct offsets."""
    import re
    match = re.search(r'^@hci_param = global (\{[^\n]+?\}) \{', text, re.M)
    expected = '{ [8 x i8], i8, i16, i8, i16, i16, [64 x i8], [16 x i8], [16 x i8], i8, [6 x i8], [8 x i8], [16 x i8], i8, i8, i8, %struct.host_buffer_size_info, %struct.read_remote_version_paramter, %struct.disconnect_paramter }'
    if not match or match.group(1) != expected:
        raise RuntimeError('controller: local HCI parameter layout changed')
    kind = match.group(1)
    generated = ''
    for name, field, count in (('event_mask', 0, 8), ('local_commands', 6, 64),
                               ('classic_address', 10, 6), ('local_features', 11, 8),
                               ('local_extended_features', 12, 16)):
        generated += f'\ndefine i8* @BTCTRLER_ABI_{name}() {{\nentry:\n  %ptr = getelementptr inbounds {kind}, {kind}* @hci_param, i32 0, i32 {field}, i32 0\n  ret i8* %ptr\n}}\n'
    for name, field, scalar in (('hci_version',1,'i8'),('hci_revision',2,'i16'),
                                ('lmp_version',3,'i8'),('manufacturer',4,'i16'),
                                ('lmp_subversion',5,'i16')):
        pointer = f'getelementptr inbounds ({kind}, {kind}* @hci_param, i32 0, i32 {field})'
        generated += load(name, scalar, pointer, 2)
    if '%struct.le_parameter = type <{ [8 x i8], [6 x i8],' not in text:
        raise RuntimeError('controller: local LE address layout changed')
    generated += '\ndefine i8* @BTCTRLER_ABI_le_address() {\nentry:\n  %ptr = getelementptr inbounds %struct.le_parameter, %struct.le_parameter* @le_param, i32 0, i32 1, i32 0\n  ret i8* %ptr\n}\n'
    return generated


def classic_baseband_exports(text):
    """Typed addresses into Classic storage; policy stays in owned C."""
    expected = '%struct.bredr_local_device = type { i8, i8, i8, i8, i8, i8, i8, [32 x i8], [16 x i8], [4 x i8], i16, i16, i16, i16, i16, i16, i16, i16, i32, i32, i32, [3 x i8], i8, i8, %struct.bredr_local_eir, i32, [6 x i8], [6 x i8] }'
    if expected not in text or '%struct.bredr_local_eir = type { i8, i8, i8, i8, i8* }' not in text:
        raise RuntimeError('controller: Classic local-device layout changed')
    generated = ''
    fields = (
        ('flags', 0, 'i8', False),
        ('name', 7, 'i8', True),
        ('page_timeout', 11, 'i16', False),
        ('inquiry_interval', 12, 'i16', False),
        ('inquiry_window', 13, 'i16', False),
        ('page_interval', 14, 'i16', False),
        ('page_window', 15, 'i16', False),
        ('accept_timeout', 16, 'i16', False),
        ('voice', 17, 'i16', False),
        ('class', 20, 'i32', False),
        ('iac', 21, 'i8', True),
        ('num_iac', 22, 'i8', False),
        ('iac_count', 23, 'i8', False),
    )
    for name, field, kind, array in fields:
        path = f'i32 0, i32 {field}' + (', i32 0' if array else '')
        generated += f'\ndefine {kind}* @BTCTRLER_ABI_classic_{name}() {{\nentry:\n  %ptr = getelementptr inbounds %struct.bredr_local_device, %struct.bredr_local_device* @gbredr_local_dev, {path}\n  ret {kind}* %ptr\n}}\n'
    for name, field, kind in (('eir_length',2,'i8'),('eir_packet_type',3,'i8'),('eir_buffer',4,'i8*')):
        generated += f'\ndefine {kind}* @BTCTRLER_ABI_classic_{name}() {{\nentry:\n  %ptr = getelementptr inbounds %struct.bredr_local_device, %struct.bredr_local_device* @gbredr_local_dev, i32 0, i32 24, i32 {field}\n  ret {kind}* %ptr\n}}\n'
    host_type = '%struct.host_buffer_size_info = type { i16, i16, i16, i16 }'
    if host_type not in text:
        raise RuntimeError('controller: host buffer-size layout changed')
    generated += '\ndefine void @BTCTRLER_ABI_host_buffer_sizes_set(i16 zeroext %acl, i16 zeroext %sco, i16 zeroext %acl_count, i16 zeroext %sco_count) {\nentry:\n'
    for i, value in enumerate(('acl','sco','acl_count','sco_count')):
        generated += f'  %p{i} = getelementptr inbounds %struct.hci_parameter, %struct.hci_parameter* @hci_param, i32 0, i32 15, i32 {i}\n  store i16 %{value}, i16* %p{i}, align 2\n'
    return generated + '  ret void\n}\n'
