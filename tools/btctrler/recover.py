"""Remove the SDK definitions that owned C (src/btctrler/) replaces.

Each removed definition is checked against the fingerprint of the SDK body
the owned C was written from.  Owned C reaches SDK storage through
src/btctrler/sdk.h.
"""
import re
from .ir import extract, export, own, own_all

def link_policy(text):
    """Own hci_lmp_link_policy_cmd_handler (src/btctrler/link_policy.c)."""
    text, _ = extract(text, 'hci_lmp_link_policy_cmd_handler',
                      'i32 @hci_lmp_link_policy_cmd_handler(i16 zeroext, i8*, i32)',
                      '7a6101272ccd7846b5ad45b52b5363edb3e0446a340d9f4851f6b98fa4455c11')
    return text


def link_control(text):
    """Recover the complete Classic link-control command dispatcher."""
    text, _ = extract(text, 'hci_lmp_link_control_cmd_handler',
                      'i32 @hci_lmp_link_control_cmd_handler(i16 zeroext, i8*, i32)',
                      'dc833ea8093f6c850f397ef6ce20b12bd4da6c43ef027e8bc783d9d78eb19797')
    return text


def command_dispatch(text):
    """Own hci_command_process (src/btctrler/hci_dispatch.c)."""
    text, _ = own(text, 'hci_command_process', 'void @hci_command_process(i8*, i32)',
                  '1c2e8fc4dd223408b67f8bb083901dc052c02c5d8c33bd61c9b9c5aac0bfe5ac')
    return text


def device_manager(text):
    for name, expected in (
        ('hci_controller_baseband_cmd_handler', '7705e47d21e9065fb2d7e2a2162182a6a197dab724ccf93eec0efb869e50e887'),
        ('hci_informational_parameters_cmd_handler', '5a9366246d31c131701c0e13eb7c48d4653f39ebbc7b186f9689351bbe9ef0c4'),
    ):
        text, _ = extract(text, name, f'i32 @{name}(i16 zeroext, i8*, i32)', expected)
    return text


def classic_baseband(text):
    text, _ = extract(text, 'hci_lmp_controller_baseband_cmd_handler',
                      'i32 @hci_lmp_controller_baseband_cmd_handler(i16 zeroext, i8*, i32)',
                      '2a540812f4dd4802432b7513a7a48e91165b8b5e3e378dde1090c45bd323331f')
    return text


# lmp.c functions owned by src/btctrler/, by SDK body fingerprint.
LMP_OWNED = {
    # address.c, link_policy.c
    'lmp_conn_for_address': 'b9f882812945540261c826ed1b8dfa721ec53fdee0f960a4a16e233e4c1d785c',
    'lmp_hci_cmd_addr_to_conn': 'b67a98ba022eb9150c51c971e292654f51887ab4b54d79b5f3a2911c190912cb',
    'lmp_tx_qos_req': '339fe3b648801859e9c4512132d3d5de981379df78dafac1ae4b3045e81e1689',
    'lmp_vendor_qos_interval_config': 'aab7bdb10cf8728cc97563fdcbaa418bdd06279d7318733f377b6d94e6d2d535',
    'lmp_event_esco_link_setup': '923b32a5ea7bd01ede5dfbf26f4f3713a6c3f88aed60f84c3628a474cde9dcbe',
    'lmp_tx_supervision_timeout': '4172dc19c2e9273ac9eeef8b0761b017599bf8e4d88797803c6a159f05938bfb',
    # lmp.c, lmp_rx.c, lmp_encryption.c
    'lmp_detach_check': 'af524a60dd3d18dc17fab48ec1d5e8724f5482d1526ade439b90ad11ca96341a',
    'lmp_stage_auth_with_simple_pair_by_remote': '819e42100178ba8f38c522eb759019afbc42179f1d8770ac21210f0fe6615889',
    'lmp_acl_c_handler': '2e19e7b45ded9f79e6a6eb571aad7792f215b6321f207cdc6696e6b2f96d1835',
    # lmp_master.c, lmp_slave.c, lmp_name_req.c, lmp_inquiry_page.c
    'lmp_name_req_init': '7a8070ee3ccfcce57a71f79c09ac2e3579c4001aaeebcfe06ce598d05b66adcd',
    'lmp_name_req_machine': 'acaa75053e147577b176d56c5d1daee05cead5f3e7bcf035059886da21c205d6',
    'lmp_name_stage_conn': 'ce2c187d5f7f6912f3d8b370b070715ca19f39de1cbe50aa65fe0c1abff05636',
    'lmp_slave_init': 'a2316ffad6d5a546ec60c29cd0569ee4c58e614eef3d55461b74f4f29948b7ac',
    'lmp_slave_machine': '787c7b68485d30f61636eafa788748b770676d8ccce71c47e276c77f05b4b61a',
    'lmp_slave_stage_conn': '39d41cbd25d18ed0333ecade511df3a5730a861e7e6389c2c91f2b71f2f13ad6',
    'lmp_slave_stage_connected': 'bb53f0a9f3b86d8693210f87967169fd60ddb1d4533eb109fd7df8a35f9167d9',
    'lmp_slave_stage_role_switch_on_enc': 'c43ca0d40d2d1e5e8dac65bec74b2dc98b54bf388c3e1603748a9df875d55421',
    'lmp_slave_stage_auth_with_pin_code': 'a11c30c1779e363a8b76489e78736d5874fefe2eedfcfb1f5ed44e49f3378ec7',
    'lmp_master_init': 'd9f829c14a23f2e5163da98c2f829eb0ba42911c7316224def8ba93e3466d0f5',
    'lmp_master_machine': 'b2e32663f5fb63530fe483f38a01dad7c49e56d0b1c85f75a5063bb08b0d424d',
    'lmp_master_stage_conn': '1032bcf46e727016909b8292baf36b30c9d25e1211e201d677aca2c5c3699019',
    'lmp_master_stage_connected': '92265804ae42e9d557bb560ac163bf74b60aa755f35b7f51b8b5b54baefa5618',
    'lmp_tx_set_afh_req': 'b102440918fd0067349c56ffd6e8bcd8042d35095f173cc621924dff4658fb06',
    'lmp_scan_enable': '98083c081661e8caebab4c9fc997df5bbf0ca0dfa5735d68f03f6063694f6055',
    'page_completed': '50d186f564e8d8093db30521d511b44d7a0236e01c689d0017f78829e5f50f08',
    'link_page_try_start_enable': 'c87be08441be84f28eca835201018fac445db922a54b24f8b8f875e9223bcc3c',
    'link_page_try_timeout_enable': 'f306bfa0eaed590fe5bbaa547e28a15b7a03f480c22bf438b7f6029af3b01023',
    'link_inquiry_scan_try_timeout_enable': 'b1fb2142714aea1cbfe2fcd661371a133579425b04b03c742edac5f7fef01c46',
    'link_page_scan_try_timeout_enable': '9e4326b469322d26528b7edab81629aeb53b0f1b7f0da098414f9dddea8e5152',
    'link_page_try_start_disable': '606e9e7c51e80a724360f73db6cefe2073cef5c69de805007a86ca064b949fb3',
    'link_inquiry_scan_try_start_disable': 'cddab06ea51cc26371cc38f4b8490b895ccb662dee3b4720f0f76fc22eb309d8',
    'link_page_scan_try_start_disable': '57d461bde97ae180e6abaeeb51c492ad44fd92204317184ca7e1b178eeef4787',
    'lmp_page_scan_enable': '8dc4da4cf511f642dbbc472ae5a1f36a8027f66cf14254622ebf7b89aa9fbd73',
    # lmp_role_switch.c
    'lmp_role_switch_misc_alloc': 'fd66f4b0bd1535ce39e1e7789863f7c379b411b05d1fb3cb448bc270bc19a46f',
    'lmp_role_switch_misc_free': '6cfc466b5cab50cac4e4957235ae8990f0f542711df4745b6c93a928cf389ab2',
    'lmp_role_switch_detach': 'ceb53e0c729f708e2abc418c6cd0d4142226bbbf9855a50582bb3f897b50ea8a',
    'lmp_rx_role_switch_standby': '880675722f91f70137295f827d53bf89cd017e9ae0645cfdb2f48f54030bc6de',
    'lmp_tx_role_switch_req': 'd302831b5a2f0686dfea3f8278942027fb16a822e9bdd103965014a9f921d3ca',
    'lmp_master_tx_role_switch_req': '99794c865b9b9f824c001ddec6532157835cb2737ee3f2e4abced31429bcf068',
    'lmp_slave_tx_role_switch_req': '160f9266c5fb978352540d39b3a3aa1591a0134e773d42f6047d000056b0d30e',
    'role_switch_req_timeout': '3d7375020db0f0404c3548c10065fcea2bdeda40c345cb9316da0615a1c493f0',
    'lmp_no_accepted_switch_req': '228a6e92615ca372efddd560cf7a66e5ef30c4c6f2992bf47d147f8a7dd5a26e',
    'lmp_rx_switch_req': 'ad0ed3c2e771c303a465b50c1d86f7a72b64d8480b0cb20c89dee618a031fa38',
    'lmp_rx_accepted_switch_req': '4c7b9be16f500013a0ec72fe667576ee2301d013cfde3145c10cdf3e6dd98ff3',
    'lmp_rx_not_accepted_switch_req': 'db105d0929b632e2de6331dfda8c63d5532f6987d1a6e1272a42641f203b01e4',
    'lmp_rx_slot_offset': '9e49fd800854caf4387dbccb549d96f50af39c80e219112561b197d5b177c7fd',
    '__role_switch_probe': '4c61217c98938b0ea880525c1406edbe0401ec75db17e0620c37b1d47359afd5',
    '__role_switch_post': '167c7c8499a68e75d37700b84f196b318cc38cb70b40d60c07983a8cdc436453',
    '__role_switch_schdule': '6a4f2bd0f51b482968a5eb3b970926e1a057e1b0fa3d5fdc1fe6b559483ac1cb',
    'lmp_role_switch_completed': '84d8039f4349cddb3e2aebc635bf94776babb2ea2d7f883bf6c0003a34a0549e',
    'lmp_role_switch_machine': 'adec2377b4d8e8a6413d4182e4638f9c71ec08fed4565e61ae6fe80cac0a43eb',
    'lmp_master_stage_role_switch_on_enc': '26dce942f5dd2d99a14be535cb69e10c531b59f549b65956085c390c72e98a4a',
    # lmp_sniff.c
    'lmp_sniff_misc_alloc': 'c76bac0b8909dfc33dd52d792ec0afffa9a72f431503b6f7bc3041d66606f293',
    'lmp_sniff_misc_free': 'e66650742d6f94f90fd09c73da77efe1b9981bc2daa492a4787686385e78d535',
    'lmp_sniff_detach': 'fa8b642b448659d894aa48c71e8d59ceb13925eb7072af536415da760db959f1',
    'lmp_rx_sniff_standby': '3f067d0a7ac73b2edc12b89883a42fecfa0add97cf8cface6eacbd786165c0e0',
    'lmp_sniff_wakeup': '6f94c3488ef668d12dd643246d76435bea6b4fe56e6e9d5da7800e4435fcd050',
    'lmp_sniff_is_the_main_sniff': 'd66b290bc06e196955f99db8b281876911df048e27ab6c7d2024c9f42a9100b5',
    'lmp_tx_sniff_req': 'c32f26d6fa3011f4c26d76197c1b3438330743e33e54c2ea375ff64cb4431c6f',
    'lmp_sniff_t_slot_attemp_reset': '31a8622369fb34f4cd7c3c981ecc7489977894998f680ebaf04bd9d5562825e9',
    'lmp_sniff_subrating_cnt': '4e6084b8f03bbf4bb42dbfc8a4581de3e4f43d56a900c01650697ca24e1f3908',
    'lmp_sniff_and_afh_offset_ali': '80bda2f62f06a1d80f8291dcfd6e934b314c4919dd812629aca36506d28e1eab',
    'lmp_sniff_cal_offset': '9877ba147ba4620f6d04c19d9cef581a98e7556adc732923f0f2317f3d7c19c2',
    'lmp_sniff_cal_other_D_sniff': 'a199b58d7b7cdeb6524470cd13772fd13b1f2d5c33f3000e6a5eba3cec783438',
    'lmp_sniff_pre_anchor_point': 'e004358dfa5d72bf4f402a251f462fe044f17142a277976ba3aa8f349689e8ee',
    'lmp_sniff_anchor_point_first': '111558bb559e15e0ea6b818f9af5fb8377cae8a1731007430bf79c0f7b90f047',
    'lmp_sniff_anchor_point': 'e9bfe424ee708171be81101d3f99da5c3ba939568afd4ea15bc7d58221186f91',
    'lmp_sniff_anchor_point_set': 'f07a765da7f341d8f8900526bdca1efc361e8362cf3c84a2088c34aebc01cb3f',
    'lmp_sniff_anchor_point_preset': '3e024a32b508740d706704d397b4a1b314a0cd8df0f8cefaaa02c1aa33a65f17',
    'lmp_sniff_anchor_timeout': 'ec39497456a5b61c700f16cf61776bd53abbac4e401178343e2aeba4014306da',
    'lmp_hci_sniff_mode': '62c0b828dbb2e88e7f26ba5d548cc467e79ab03aa663a5acffdbf0a38cc4882e',
    'lmp_hci_exit_sniff_mode': 'd28faf6d0efc81aeb47a89537711eebce5fe2e4951e134900ebefcc23fccbc71',
    'lmp_tx_unsniff_req': '88478cdaa5083ced9c626601547fbcc0a263da791f9bea505fe547e0688393d4',
    'lmp_rx_sniff_req': '4ccb764e2b04eb2e4c8d4654345ecc5d5721e23b509731a61f8bcb7016b9e346',
    'lmp_rx_unsniff_req': '23bc2bd1f0f35104c7cf2e1fbdf3186e386274439fc425fe7b457bbee40b2d52',
    'lmp_rx_accepted_sniff_req': 'b5aa18ff29f9bb30701f35e8572278a4e312dd33f0c8583ad55bd53455e2e850',
    'lmp_rx_accepted_unsniff_req': '0e605f8f90384e766ba4d6fcf12520a85f8a273c08d60a653114db01665fdb8e',
    'lmp_rx_not_accepted_sniff_req': '30caf55d0232dbf29d7cae4a364da150057f7b11fe39181934490e969ee0e169',
    'lmp_rx_not_accepted_unsniff_req': 'd2c65d471ba945077629e8a98bb7af47531449fea2561ac98f8634e2612c4035',
    # lmp_encryption.c
    'lmp_free_encrypt': '20492e51a850dc2ae9837e3beb2cf3f73e9e0e79d12bac93f33b8108643ae870',
    'lmp_tx_au_rand': '3d165633028c2131e11515b0f57f788476df0a36d036e154a7f22f26cf9acd27',
    'lmp_tx_encryption_mode_req': '10575a9c2b589cf0b8abbe4aa0782cb0eed01625a538c0bb56794770b72549ec',
    'lmp_tx_stop_encryption_req': '0cdfb4beb1544bbfec8d5407d731e1cd3dfba537b0f8c1f3abe0f75750a54c43',
    'lmp_rx_not_accepted_in_rand': '572c6c114728e75b07831d7049942a8fb222fc64e6329bc28a75b63e6ee1d079',
    'edr_vendor_keep_connect_for_keymiss': 'fcd59b378b3b7b9a66bd317c749151d82f5be685fbca676d42219af37fed5039',
    'lmp_rx_accepted_stop_encryption_req': '2d915484ad55b13f64bb3a46175f7b6096aadecbc203e749ab4e26f87091cd98',
    'lmp_rx_stop_encryption_req': '4ff6caaa0308762f58def32c6509d74f9570e2ea2171f252d4299c66ddfaaef6',
    'lmp_tx_pause_encryption_req': 'fb292cb470a792dc1c041ce045629f831697624d0d6aed59f9f3636e91149a5b',
    'lmp_tx_in_rand': '4c0cc8e875cac0e468f569724fbc81b5259b7ab8d7f3897f9a976e32916b15ff',
    'lmp_in_rand_res': 'b422398ddb963695783bcc798f8543675429bb739bc1f0cb5620ecfcddcf88ff',
    'lmp_rx_accepted_start_encryption_req': 'e60e236d1a4fd14bada0464d27f6e7a5704fb28a916ab5e38c4585e849bb1146',
    'lmp_rx_comb_key': '4297ceeb5235bdb13b69539fbd173cb2a04c953bc1c9a7916cf1e02358b83739',
    '__calc_and_send_sres': '18eea83756e6e2b1d441c7a2e2f7a9f7be1ef60ad7ab21b8e50400a16c683d06',
    'lmp_simple_pair_enbale': '9c2de83388fa70049f2124a1640c44f19ce2893ee7d862734cfa77f4f256a4f6',
    'lmp_tx_encryption_mode_req_dly': 'aadc0ed6e84345f2ad81ba9807a4e700aa4e2d36dcdcbf865cbd83bdac3adb96',
    'lmp_rx_in_rand': '008d78abec2fd9b0fdfb5dd2c5cf432060370cec72e0382c546a22980dad8ea4',
    'lmp_rx_not_accept_au_rand': 'ce12cfb518cbc447a6fc5a93ef760f78cbbb2c05dd04c46572d5296fa640c108',
    'lmp_tx_comb_key': 'bfdc6188b47ded501510eb88b397b1b7167d4ae5d2c147a5a75aa041df1d6b7a',
    'lmp_rx_pause_encryption_req': 'd28f6291a50b44631416e7184821e208b586b92b735d92e7b735f17ce03e2285',
    'lmp_rx_resume_encryption_req': '91178e2f14a8a611414d903cd7e90da32323126aeb6e4ed44190114888f66d52',
    'lmp_tx_start_encryption_req': 'df719cf6b41622c2a4e5448f95a124f8345716a73acf31bc0b04b579fc78fc5b',
    'lmp_response_comb_key': '0814edab713c9a59aae99cd389902c3968aae1389f8b73b3c6de1f1e7a45d4d3',
    'lmp_stage_calc_link_key': '4a104a063805445c56dd078ca24121ff5bcdb97002c81259566dbd26071e4e50',
    'lmp_rx_sres': '9bb7f89ad0dac492a3601597d8f4dd2cdcfe2a440bd8ea48183f7e18e637e61d',
    'lmp_rx_au_rand': 'fbab4132191f538221162eb8a4a5b85bf38bd2357ac887fc3d07c752efa8dade',
    'lmp_master_stage_refreshing_the_encryption_key': '769185c3ab027a0d121155ce3ddb12551f85b1879c4e46b4401f64119a9761df',
    'lmp_slave_stage_enc_start_by_remote': 'baac0003b172b36c9dac2addae07326f1fd43d61e5e44b3c3257099c96435b9b',
    'lmp_stage_auth_with_link_key_by_remote': '0acda6a7c2865b5dcd7337bcc28c0401f26387cff04f80f0fe6c3f52dec31691',
    'lmp_master_stage_enc_start_by_local': '2c619183bd510a2d4454ae4d9dcf630596052d933e903066b2f73e334851c86d',
    'lmp_stage_auth_with_pin_code': '7b08a199a5a452ff41cce0b2e1fbc591f0dcb82a0bf694e52841cec58884a08a',
    'lmp_stage_auth_with_link_key_by_local': 'a7d14b9e72a886de23466c0760074b583b2b8f617c99a69c7e64ff4f8b5dcd4b',
    'lmp_stage_auth_with_simple_pair_by_local': '5148f1c98e2f0c1b766f9d9565f55736b25c2f1a6d90a69c45893d21c98523d7',
    'lmp_dhkey_check': '7175ffac73748c2b78069703a3b360dc99f304abaf2f6fb181f24911390b732c',
    # lmp_escape3.c (refused), lmp_esco.c (no voice links)
    'lmp_escape3_command_process': 'f9171a07a717e930a8ccffb56065a6dfb01d287e553bde555525130d0685fc87',
    'lmp_rx_sco_link_req': '1d30319db16fd840490cea3a3a41c05e27f13b0bd792aacdf6e3c764a6b5affe',
    'lmp_rx_esco_link_req': '5c87de607cc20927cb241109c00c889712e1cd6e6f1d87403218c84e2b315e64',
    'lmp_slave_sco_conn_by_remote': '7df11274e7fe76a13eea4ccce61a9708a1ddc4dd345761ac893bbf95cf4afaff',
    'lmp_slave_esco_conn_by_remote': '1932aee072bb1cf31a33db6347675fb35389e6930200f22f36b12f4bc9d70f7f',
    'lmp_esco_link_removed': '7c944f73285cad65fcfdec812816c2152a899ef1678ec59bdd35469f2170c2b9',
    'lmp_get_esco_conn_statu': '196ffa60a36cc7f1b82dbb0db2865f01471512772d8a0e5508d8953abd0e3857',
    'lmp_esco_conn_for_handle': 'b7c0e401c2a4dc4bb83fcc8f21f122b4db3921bcbbd4e28c1701c9503f143318',
    'lmp_connection_esco_open': 'c6f87d54388732ce6110e416405bb512806ec7c1d3cc8f18c2e401f0615d31c1',
    'lmp_hci_disconnect_sync_connection': '59536135244e9616ad627a0f139e64577ed40ce224f5ff6b5ad91e523526fca1',
}


# lmp.c statics that owned C calls: external linkage, C calling convention.
LMP_EXPORTS = ('lmp_rx_handler', 'lmp_send_acl_u_packet_to_host', 'lmp_dhkey_check_accept',
               'lmp_cmd_is_match_size', 'lmp_send_aclu_en', 'link_conn_rx_remote_max_power_res')


def lmp_member(text):
    """lmp.c.o: remove what owned C replaces, link what it uses."""
    text = own_all(text, LMP_OWNED)
    for name in LMP_EXPORTS:
        text = export(text, name)
    # The ACL slot is file-static; owned C uses it.
    slot = '@lmp_acl_link = internal global [1 x %struct.lmp_connection]'
    if text.count(slot) != 1:
        raise RuntimeError('controller: expected exactly one SDK ACL slot')
    text = text.replace(slot, '@lmp_acl_link = global [1 x %struct.lmp_connection]')
    # Owned C keeps the QoS poll interval (link_policy.c); nothing may still
    # read the SDK's copy.
    if len(re.findall(r'@set_qos_poll_interval\b', text)) != 1:
        raise RuntimeError('controller: set_qos_poll_interval is still used')
    return text
