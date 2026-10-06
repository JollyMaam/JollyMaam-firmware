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
    # lmp_receive.c
    'lmp_rx_handler': 'a20b1d2123b3ccf9cb2f6f1871ae70ec4cb91a660bfd676ffadee61c747b2bb2',
    # address.c, link_policy.c
    'lmp_conn_for_address': 'b9f882812945540261c826ed1b8dfa721ec53fdee0f960a4a16e233e4c1d785c',
    'lmp_hci_cmd_addr_to_conn': 'b67a98ba022eb9150c51c971e292654f51887ab4b54d79b5f3a2911c190912cb',
    'lmp_tx_qos_req': '339fe3b648801859e9c4512132d3d5de981379df78dafac1ae4b3045e81e1689',
    'lmp_vendor_qos_interval_config': 'aab7bdb10cf8728cc97563fdcbaa418bdd06279d7318733f377b6d94e6d2d535',
    'lmp_event_esco_link_setup': '923b32a5ea7bd01ede5dfbf26f4f3713a6c3f88aed60f84c3628a474cde9dcbe',
    'lmp_tx_supervision_timeout': '4172dc19c2e9273ac9eeef8b0761b017599bf8e4d88797803c6a159f05938bfb',
    # lmp_events.c, lmp_info.c
    'lmp_send_event_encryption_change': 'fdb8b05e6ce83c5da4a27b1445979bc38f9994fd935190b832797523f24fbcbf',
    'lmp_send_event_role_change': '1acabfae67d8d6dea77e859adc40b6ee87c2e4e2fc7e3501c167dcb08048aca1',
    'lmp_send_event_link_key_notification': '3170c30944dd2f99ac6debe9c4dc89facaaeb157d885b90c193ae79c57781c82',
    'lmp_send_event_connection_request': 'c68595dc7c3f58b6aab54df026f1069a87e2817b7c7ee7053f7d4253ea4a9f67',
    'lmp_send_event_link_request': 'f6f3019a0edda10460782e6e45622291a9429be18b964dd9d75e7e0baa77559f',
    'lmp_send_event_auth_complete': 'cdcdbd7be9b190070021936ddce694dda3dcd8403db1f8a4492c9abc67fb1988',
    'lmp_send_event_connection_complete': '7787a5494066c9eb9155bdbe40ca9fea6cc2151c9d32cabe299f82f9370e9d53',
    'lmp_send_event_mode_change': 'abdf82d37eb2abf19d5b4b3fc6046c9fa1b60005c604aa24eb281d6aa53bf235',
    'lmp_setup_complete': '3e965dd502e396cca82c7c3261df1851186b140f54bfe93729aa9c30d72f91c4',
    'lmp_tx_version_req': 'a284221e9e872f89e1665ce139733f352db92137843781c9946d740c94b6fd64',
    'lmp_rx_feature_req': 'b15210705a582334eb951dbf354875e6eb0779297598d194513776bf901d1c06',
    'lmp_rx_features_res': '85dadcb6e4e011aa5d4c48f0dcf4e6550e90e9a484c0b03b0583924bd3342357',
    'lmp_rx_version_req': 'f0048697387e4bd697230f4aaef18bd46f75b5e40f42ced25c772ad1cc531e28',
    'lmp_rx_version_res': 'f9639b28f6a6a0606df08c447582ebd9f8cafe622be496e7bf5ece003c65642b',
    'lmp_rx_name_req': '2db94a1da615ed3a46d4d1ccdb6b72de44bd0a99337037a99ca22c40b0340a34',
    'lmp_rx_name_res': 'f57554a513225158a87e726aff5feb3dd4f79811405f4254e7de95a5066eb5c7',
    'lmp_rx_features_req_ext': 'a296aba450c57d5dc08a595380d1ee7713e8e5c573e37899b9566d72ef325b83',
    'lmp_rx_features_res_ext': '06995095b47208557a42932c344948c1235d03dbe0a299d4a66bc0e3e8bec786',
    'lmp_tx_features_req': 'ed2cc59e35cccafd27fa8c3006b0f3a34f65c3b08e2667a0f96a24ac54af576c',
    'lmp_tx_features_req_ext': 'de70d04338f3a69de53d9ec5ccfec7cedf8dbff25ef5a4695be2181dc2a1b492',
    'lmp_tx_name_req': '5cad91c950b85fbd30f43b1bd831701950a3dee27592831cf23f44f59db7833a',
    'lmp_tx_connection_req': '6fedd26899eb9bb7b7ef4cd5ea0478f764f78642328503326a4db569661965dd',
    'lmp_tx_setup_complete': 'aef2ae1419bb1f1da7b6ef5f3ce0f50c2773212f1cdfe988dfffa6fcf4120b2e',
    # lmp_packets.c
    'lmp_tx_max_slot': '77ffd34736e21ebd62cdaa90fb54caaccccffa4bb7a3c4be4dfc12af3a922dcf',
    'lmp_rx_max_slot': '157fd592c06f2e1cf059849c88dd933e73be65d85007893b2805b8e4e0141585',
    'lmp_rx_max_slot_req': '89d47da26d6e495326af3972f2638ecfa5ef755241336fe25649914aeabb2767',
    'lmp_rx_accepted_max_slot_req': 'b7f918b09fe803559dd8fbb2fde9547718929d72d80de31928a3334cd030da20',
    'lmp_rx_not_accepted_max_slot_req': '0189a2a36967f53710315959381979a3b6ef05cbc6c289b4e391f7fd5b51d5bc',
    'lmp_tx_packet_type_table_req': 'a5d99f847a064bdc5b338bebb5fadc7b05904f42a67826286fb3245a281be86c',
    'lmp_rx_packet_type_table_req': 'c007642fc86036e1ac4d753a9ad2a6941d34c4c010224ae689a9b72a8902616d',
    'lmp_connection_ctl_slot': '30f3698cd869aadb51d028b898847b011460ea3ed69e56c513199ebe3a70546a',
    'lmp_send_max_slot': 'abbb1385ea2e64650a9ec53a89291980000806e8c919a4a9ac68412639279b91',
    'lmp_tx_max_slot_req': 'f41228c2e73d2475c18cc9f6b90e2523520bb619a2b175b3038d3f6ba57a63f0',
    'lmp_tx_auto_rate': 'e6c6a4434a492fa649030286b3b6f7427ec0faa8b45a67d255b9d7bc655d00bd',
    # lmp_afh.c
    'lmp_tx_channel_classification_timeout': '7997604c4f6aff90a04a4389ab43ca7b05328df3a99884ace6af011022282e20',
    'lmp_tx_channel_classification': '08d82e377fdabf6665917cbec3e9dad069de64c455344893dcec7125433c8d76',
    'lmp_master_tx_set_afh_user': 'c197890c660235c94c37cb260237977c6ebe15b2ad42a6222180ad74e874ee5d',
    'lmp_rx_channel_classification': 'c50064fcb2f20f539febf80506ddbf1cedd730dabe64dc86892963d9ca8f3411',
    'lmp_tx_channel_classification_req': 'dffdb50f9a77e4c63ea249265ebd9d51f4ccd7eda3c94ae30f5ce48c32966646',
    'lmp_rx_channel_classification_req': 'b842a94c0108bee52033de8a9413a95522fb51fe1944a24809c8ed27c16042a8',
    'lmp_channel_classification_close': '3c756cd56f7024db9bf3cc97c9e622c94ebb1130a55ef942ba0bafc0220e96a8',
    'lmp_rx_not_accepted_afh_req': '3361ee9043faee5ab68f29433e94659a22de55f7f25c7ef011675d24098e74c5',
    '__lmp_enable_afh': '0cc6c7c55fc07f56e72c20b72d6250d61ce3d001ce3e69286931969ba1f2bcd7',
    'lmp_tx_set_afh': 'b708a87085841fed647dfe9cd2df55e886e8369093755111a4771da7db58c7ce',
    # lmp_connections.c
    'lmp_init': 'bb217885d6242c2c65b1154cf37b1f89273ceb56b8e22d5b8e5e07a046d5f063',
    'lmp_create_conn': 'cbcac5f9eabb8b236d8cf41fadd77e5c2ad89953ec83a19fa670323c2f73e3c4',
    'lmp_conn_for_handle': '9be908fa94fb6545e5c9e23448ef93e2046e1351a67d13d26666377f61fc7cff',
    'lmp_conn_for_link': '8014b1c991c55d441be2ffed55958b537b494ea1d41beb54b66cee0a1fa6282f',
    'lmp_get_other_conn': 'a9d0f84d6e3f75f2437d5de26dad3869c6251d51e22ed7fd6e43f841cef39430',
    'lmp_conn_remote_address': 'cc85878cde54791faba2d30496b8c32f9710c6350271175690b8816e81ad58c8',
    'lmp_conn_for_active_state': 'c1415b000da23b231cdc86bc5b3bc7e4e755a4fb1791d72dfd7e21591dca193e',
    'lmp_get_conn_num': '80be9553e62537137c84bfa01e280a352c14de128d0c9dc1b4cdefc43ff5d7d0',
    'lmp_role_machine_handler_register': '0de74ce2f6aa0d31e1e1450babd0d0e9b8e78c369ca8dd0c328c9acc81f6b547',
    'lmp_role_machine': 'e9b87acc6c19b5f8cd68d3daaf053e75a9201b3c4f1252e926b849cf2c1b46c0',
    'lmp_tx_detch': '1d1ae111881f5b1d7a2599c8d8e85f0e512760989a37facd1481382d726141dd',
    'lmp_tx_disconnect': 'f5eb22829ead86f0ac93f716416a0f74ae2ce99b6ca5fda67ee69d6929dffc69',
    'lmp_set_supervision_timeout': '4fc5a8302861a688ca947b927ab6ca8d837ffc5de3236612c15b04788b55af4d',
    'lmp_connection_timeout': '49ab2ca8075f0f784c8cd3ea6ea862012f32f928bc4b5de0e972fbeb2d2ee957',
    'lmp_connection_start': 'e6d47b96a5d51de21da23aa0dd8afc7a0f83c3dec4cd176e0098c068b2191d7f',
    'lmp_close_all': '6984419a73504611d9f11b49746a5c1b3bb93acb838c61276715095cbd606545',
    'lmp_reset': '969cc4eef6a72d60ffc257942ae625b6dcdd0943c076cc018ec7618e1297a4f4',
    # lmp_commands.c
    'lmp_hci_cmd_handler': '17ee480f2dbff3686f11027e7be6e5d084c71a16ae9bd10be75840d98e40b14b',
    'lmp_hci_cmd_handler_to_conn': 'ca26661983ea85f4776db58a44d5738f660fdf9831aadf6d36c14c9f92b2c93d',
    'lmp_hci_cmd_to_conn_handler': 'dba4d7dd9451deb3c7d6547c861bed8f27f70c39957bc4e0d2e19cbb6731c475',
    # lmp_acl.c
    'lmp_send_acl_u_packet_to_host': '1ca311a7ab84acaec6b852b68b11ad4515b6e405450c89e694916366888c122b',
    'lmp_private_abandon_sbc_data': '4dcb002e6432f6d509e9983efafff61ff3c455851ce194ffd18ae636f3741ac2',
    'lmp_send_aclu_en': '0a3ade18bfb90d8a8fd4aefe7cec6bfb0f616a0b1ab3aa6696b799366fb7d206',
    'lmp_private_get_rx_buffer_total_size': '0ebf2b88469c0c8f0297216ee7a596bebe842cf8aea39ca104ca34c3db3e5f32',
    'lmp_private_get_rx_buffer_remain_size': '4a5f91031c148044127ac8893c7d394f941e72a238b5dc223434a9b6791fa13c',
    'lmp_private_set_max_rx_buf_persent': '6053fca61d055fc781aa933d0dd2cc0cdf873d5b127d74d06eba6c0669bb05f6',
    # lmp_link_events.c
    'lmp_cmd_handler_in_irq': '7bbcbda0aeab9ae66d2fcc9eaf3a2e80410fae78b6c0aad24fca356c91e82bda',
    'lmp_event_handler': 'eba36890cbc40a6e0050f967caba2a5e5e46fde4954b18050d3d8bfbb32fb1ec',
    'bredr_close_all_scan': 'd4e4489fcc3fa0db6e8a600a1ccf85268e49b4b324e5943b37cf845d46b70037',
    'check_lmp_detch_over': 'f4c24fafb9149bfcddd62f68d4acab5690396fe4ce012786dc5d1832760a1f2a',
    'lmp_disconnect_all_standard_connect': '94be8cd62ffaf174593cf123c52a020a9764971cfde326ab8c815448dfb3f90f',
    'lmp_standard_connect_check': '4b7bd45ae82057be95d994a8be7791c8220fc4de0d0fd6cf922753226f827c23',
    'lmp_timeout_irq_handler': '8f9dd1ed415844462118bcf8f79029330dba39d973c2d50c35c31fb804e0d25a',
    'lmp_authentication_requested': 'f9dc32119eb0185fc5b12909d0c9603a90853c049da79a5c285b17957887221a',
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
LMP_EXPORTS = ('lmp_dhkey_check_accept',
               'lmp_cmd_is_match_size', 'link_conn_rx_remote_max_power_res')


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
    # Receive dispatch shares the SDK's DUT and update-routing state.
    for name, kind in (('lmp_dut_en', 'i8'), ('lmp_dut_phone_en', 'i8'),
                       ('lmp_update_rx_handler', 'void (i32, %struct.bredr_rx_bulk*)*')):
        static = f'@{name} = internal unnamed_addr global {kind}'
        if text.count(static) != 1:
            raise RuntimeError(f'controller: expected exactly one SDK {name}')
        text = text.replace(static, f'@{name} = global {kind}')
    # Owned C keeps the QoS poll interval (link_policy.c); nothing may still
    # read the SDK's copy.
    if len(re.findall(r'@set_qos_poll_interval\b', text)) != 1:
        raise RuntimeError('controller: set_qos_poll_interval is still used')
    return text


# Remaining common and Classic HCI boundaries (hci_misc.c).
DEVICE_MISC_OWNED = {
    'hci_link_control_cmd_handler': '62f3c9ba9d259dc6343af6d8d1fd524d18a83de8c95e7d6b3f2724e1130cebb6',
    'hci_link_policy_cmd_handler': '4589a777f3d37fb0f8158cb4789fda40721dcbfd48efd86d8fef7ba4401ed662',
    'hci_status_parameters_cmd_handler': '2bddaf215c4f586c4e034dfefb68e477b76040832d6d824ed9a8f92e535fda42',
    'hci_testing_cmd_handler': '1d50a9019c8da7083cb854634b9ad60401d4810031672aa876d04f235ae78094',
}
CLASSIC_MISC_OWNED = {
    'hci_lmp_informational_parameters_cmd_handler': 'f395dc5adb4a2a5811ec09a2c67833db7d9a6ab4399a2bae7f0719143de52d69',
    'hci_lmp_status_parameters_cmd_handler': 'ed89d10902bbcf2262b18051331d4fe1a3192471724574dbf6ea82cb833e4625',
    'hci_lmp_testing_cmd_handler': '62c269d8ca8b38ed378bd5861862304d856c361805bb1cf11a5c6c378de5dba4',
}

def device_misc(text):
    return own_all(text, DEVICE_MISC_OWNED)


def classic_misc(text):
    return own_all(text, CLASSIC_MISC_OWNED)


# Public Classic HCI procedures (lmp_hci_procedures.c).
HCI_PROCEDURES_OWNED = {
    # lmp_hci_settings.c
    'lmp_hci_write_class_of_device': '611d2f778e9dce51d977352a97c474c259d621b75af7874a8a00960793edf6d9',
    'lmp_hci_write_local_name': 'bcf27db9ff369df2b3d9c3f075d6020b8d2b7eac0fd56ddd5a9aa1572c1f658d',
    'lmp_hci_write_local_address': 'c071a0fbcad97680cf7963669d6ed3948ed1d90c286012e6d045dd8abf02243b',
    'lmp_hci_read_local_address': 'a100ef394618f04b25ee36a52f842e330b89a0d9113d14ecf1000fbdb928f10a',
    'lmp_hci_write_simple_pairing_mode': 'cbcca9439c8ef03f05524e084401f685084eed9fe0d10963ba57464d4bac9cf1',
    'lmp_hci_host_num_of_completed_packets': '3728e188a05eb9b2e4d0a766c67cbff5c7e9c1ff6df130e198a2314987991931',
    'lmp_hci_write_le_host_support': '05aa0efdbf403953dcd27250625b7459ba00316bc053d37d1fba6369f3a1737a',
    'lmp_hci_write_page_timeout': '80ac408c28f48d7ca210192993d982982a6da5c8a3f5fc003748ff5ee8dac1f0',
    'lmp_hci_write_super_timeout': 'f06f883f46bed6c3ee979cbed809540a17417e064c47dbe3ab68e85a4a1ed10e',
    'lmp_hci_write_connection_accept_timeout': '5d3b8efe4f582298b6b715194fd1eba56ee6c9ce1cf110a7b8d57966befcec9f',
    'lmp_hci_read_pin_type': 'c7497ef34448094c0851583b267caa437337c39d9cba92310d07ecea563ac71a',
    'lmp_hci_write_pin_type': 'bbe75cd11be8b1cc8c2b0561baa5b9adc845bab30455a64c62743c8960b9dad0',
    'lmp_hci_set_role_switch_supported': '9ebef49dad83c81d3e0b0056744c180ca3e53a6020e7d19ab60bb0d526babcf8',
    'lmp_hci_read_local_supported_features': '5737d881ad84050951fce71d04b50fc6233fae40a4b992314ad94740dd7f3a8e',
    'lmp_hci_write_local_supported_features': '031eebd91984aa5620218e220d38964e7844570dda012492c8e1e211605f199f',
    'lmp_hci_reset': 'e63dcbcbd10ef0fa369348f75082a7ab1da63f827f8bc80bedafe6d177e5f3eb',
    'lmp_hci_write_scan_enable': '262b08c3c47b0d00008a6b6d7ba7bb1bc21290533f067a9a6833cee4ee1b313b',
    'lmp_hci_set_connection_encryption': '990eff357ea802308784fdc6ed812ba874904be1b129aa3a916d517b7911db03',
    'lmp_hci_sniff_mode_command': '15a08c40c24eb981b20f901de209a7beb90d8794187f73b64ab4ce410dc1e336',
    'lmp_hci_exit_sniff_mode_command': 'd2592437fa656fa5fb209657fe5a4f839b2355fcf017355623a861baa30bfecd',
    'lmp_hci_authentication_requested': '6667d06da6aa85dac324e3dd4bdf13aa4a5e5401fd490f34ae5863ae008edebd',
    'lmp_hci_write_link_supervision_timeout': '7f679bddcca1bedd0a31670da7ed1375d6517f25313abb49c158a82140c89575',
    'lmp_hci_disconnect': 'b0cc2b2ec6cdaba52c8f8d5e07d457374027297ea75d3281619941934050be8e',
    'lmp_hci_test_key_cmd': '80d773b6af7f212c4fa09c424969c1f0f8602a256ebbf549300b502c585b1a63',
    'lmp_hci_send_user_info_cmd': 'dd4349b9f10a5583609b3f3c43580265d330c48029b92aa8db5e8e94f5569375',
    'lmp_hci_read_remote_supported_features': '5c0a0b6f58753f788599f3e9b9772170b9c9f56081b635924ebbf5b2e6106403',
    'lmp_hci_read_remote_extended_features': '0a672e4aefa107ee596fd943a610dceb622baa1c2593489afc63d198d8698949',
    'lmp_hci_read_clock_offset': '9fc1b3b2e498781facdf2c6027bf27e66f8231b392a6a8fb1d445dd37ea29fd6',
    'lmp_hci_read_remote_version_information': 'b42ba97c1e86e0c3de112c2150112e071ed9b33421537c4e6d4f112ab49970d4',
    'lmp_hci_refresh_encryption_key': '3747b8f769fe7067908e1491a0d730f0cc422fb5715ca57e6afd9bacffb0991f',
    'lmp_hci_role_discovery': 'a21e8ddccab39877b5f43d5a441b4bf4ae9ace5a46a151a643b242e38cc0feaa',
    'lmp_hci_read_link_policy_settings': 'f5fb09ac02d5325a9d80dfd55f531198f5d971e4ae53de867786cc971102785e',
    'lmp_hci_write_link_policy_settings': 'd329ce099530b288ebd0bed89ee672a07e2d76cdef4cabf60317b79cd563e1a1',
    'lmp_hci_read_default_link_policy_settings': 'b43400e4157596ddec63d7cabd81f14bd081ef023404e4b627d23a979aa38fcb',
    'lmp_hci_write_default_link_policy_settings': 'a7c3c234d3789167ee658257ad4559d530b556c94a03c06215f3b62b6359375c',
}


def hci_procedures(text):
    return own_all(text, HCI_PROCEDURES_OWNED)
