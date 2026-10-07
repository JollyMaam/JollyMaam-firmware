"""Remove the SDK definitions that owned C (src/btctrler/) replaces.

Each removed definition is checked against the fingerprint of the SDK body
the owned C was written from.  Owned C reaches SDK storage through
src/btctrler/sdk.h.
"""
import re
from .ir import extract, own_all


# misc_hw_common.c.o duplicates the three functions in misc_hw.c.o byte for
# byte at the function-body level.  This table checks the duplicate before
# dropping the member; it does not register another owned C implementation.
MISC_HW_COMMON_DUPLICATE = {
    'crc_get_32bit': '18106245b18f3ec48fb97b567f5ad32892bfda7b00f622952ff2a64b05892b77',
    'crc_get_16bit': '436dfae2d1465df8f340e12303de3ea0beea351b991e5e5f3689a23bcfe2288a',
    'pseudo_random_genrate': '4a331edfa504d74d287638d468c6eab958cc537190dea15c61d0fc4bc7b17de3',
}


def misc_hw_common(text):
    """Check the duplicate common misc_hw bodies before dropping the object."""
    return leftovers(own_all(text, MISC_HW_COMMON_DUPLICATE),
                     'misc_hw_common.c', set())

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


# hci_controller.c host packet entry and command dispatch (hci_dispatch.c).
COMMAND_DISPATCH_OWNED = {
    'hci_command_process': '1c2e8fc4dd223408b67f8bb083901dc052c02c5d8c33bd61c9b9c5aac0bfe5ac',
    'h4_hci_push_data': 'a1913c4b712c28e5f208960afcaad15e3e259e366cdf8f3a187dcaf67311c3df',
}




# The rest of hci_controller.c (hci_transport.c): event senders, the HCI
# thread, start and stop.  The SDK's host-side senders and voice/ISO
# delivery have no caller.
HCI_CONTROLLER_OWNED = {
    'hci_send_acl_data': '70714d3e8a29da450227ff5d47f663dbb37d05d68d2fe551b99cd25e3570301e',
    'hci_send_event': 'bed0bf39fe7416019c0c3093c5310563f20c097f34d17543c15d105f98d9e5b6',
    'hci_send_ext_event': 'd3c149dcdc6be2e744cc3f1650bd42aaf6c91c4ebe20e1bf3de9d9e04b74c2e6',
    'hci_send_event_payload': 'f30083a0e642dd46234ca47f20d553e3dca4de6a8d541e75178c077cca6f5a95',
    'hci_send_event_cmd_complete': 'd39dffda84e66c94748aa16b199363495d209b3333adc9594005d8ba07e71167',
    'hci_send_event_cmd_complete_payload': 'ba008e43e912ac7182a17c6494e1d4169f9a6a2112657e14e0701184065ef23b',
    'hci_interface_setup': '84c9383762ce56fda85f68fa1cfe43b20b326f4a060eeb0a919f90ad9a5ce0de',
    'hci_controller_run_loop': 'f62b9ea064e362adcc9b2dc69be6b14c96a359b99353dd4a64ca9c5ba1f16b3e',
    'hci_controller_init': 'ed5e2e25e4840c6b1d52c1af7daf56a4c6b2c437a2952d1fe9b982051b8e1dbf',
    'hci_controller_destory': '1f9845a42260f075e79d54634c749128e8fe6fe5cffe9ee7cbcaff689e8797c7',
    'h4_hci_packet_handler': '5221cd5db3b5b96ce96d4f67f5ac2474234a08cd30d87c0ff00cffdf8fcef015',
    'hci_h4_download_data': '80177117fef2e68678ab3d885475aab09f1bd0073e70060764dd988e5f65d017',
    'hci_thread_process': '5dc049a796696a125c0e51277bcd419002b73bd3c20afd7811bb5b06a6131f84',
}


def hci_controller(text):
    """hci_controller.c.o, one own_all: owning hci_command_process rewrites
    the call in hci_thread_process."""
    text = own_all(text, {**COMMAND_DISPATCH_OWNED, **HCI_CONTROLLER_OWNED})
    return leftovers(text, 'hci_controller.c', {
        'cpu_reset', 'hci_send_cmd', 'hci_send_cmd_payload', 'le_hci_send_acl_packet',
        'hci_send_acl_packet', 'hci_send_iso_packet', 'hci_send_sco_data', 'hci_send_iso_data'})


# The in-chip host transport (hci_transport.c).
H4_CONTROLLER_OWNED = {
    'h4_controller_can_send_now': '23fd8612020b1c23a9e8d2181981aaae8a5894aba58c3c7886aae7f066b82055',
    'hci_transport_h4_controller_instance': '86b8a3183dd3257a4f1db1684d324f87b1df02bd31e08c19e8e36d9f42de9e2a',
    'h4_controller_init': '67cda0029f0ebf25280c2bb29da8493fe97d84721f91c2814cb6019be353ddd4',
    'h4_controller_open': '1c0a58e02edb5e59b65445ea5460324caf73cf87f899fb3f6c87803b714c5df4',
    'h4_controller_close': '8954a228db1e57da4a43adc1fd785512451c267ea48ad9386a85ebf7ad2e1156',
    'h4_controller_register_packet_handler': '9a0b85fc0c1bbedff89e9a8ef392b0c76697c0afd8aeade42d29544033f29a4b',
    'h4_controller_send_packet': 'a15b9850d30b625c23789f8d2c11a8cbad7cbf6d4f685e43e432ec7f26c95629',
}


def h4_controller(text):
    return leftovers(own_all(text, H4_CONTROLLER_OWNED), 'h4_controller_interface.c', ())


def device_manager(text):
    for name, expected in (
        ('hci_controller_baseband_cmd_handler', '7705e47d21e9065fb2d7e2a2162182a6a197dab724ccf93eec0efb869e50e887'),
        ('hci_informational_parameters_cmd_handler', '5a9366246d31c131701c0e13eb7c48d4653f39ebbc7b186f9689351bbe9ef0c4'),
    ):
        text, _ = extract(text, name, f'i32 @{name}(i16 zeroext, i8*, i32)', expected)
    return text


# BLE command handling (hci_ll_cmds.c).
HCI_LL_CMDS_OWNED = {
    'll_push_acl_data': 'e98e297db59480254962063bebc2cbdbdf030d167aaa3a6c7d51695144b894ce',
    'hci_le_controller_cmd_handler': 'ff6a1f52dea4d30bd2330f95a5796e8c676bad5a9b2959130771576ef4c8f867',
    'le_controller_handler': 'a57c2f0f7045767f13a62e9a32916463a4b6d04bfae838cb28e590c8ae648e7d',
    'le_controller_init': 'c48868ed6621bee0a5d477d5cddbccf7378536dcfafc21e9b90006f72e7a9a77',
    'le_controller_set_mac': '134105988b6930e8d208c718d1bcab7f26ef3a64ce4430e7f1057ee31de0d01a',
    'le_controller_get_mac': '6e7b265dbbc92263f594b0bb92fb5e2efecf74cb4364212e44843a771f6a14c9',
    'le_controller_set_random_mac': 'b4dde14e09651f7ec1c5e0bfb6b357a71178edad185b57d833a590fccdfcff7c',
    'le_controller_get_random_mac': '8e629dc09de8de9897a7662353a26249d2756cc02240c7c4b0dfd4a7c6de9354',
    'vendor_handler': '30992ab995b5a0e4874a15a41c47cc6302152cd0242debe93384d6b4e76c8b5d',
}


def hci_ll_cmds(text):
    return leftovers(own_all(text, HCI_LL_CMDS_OWNED), 'hci_ll_cmds.c', set())


def classic_baseband(text):
    text, _ = extract(text, 'hci_lmp_controller_baseband_cmd_handler',
                      'i32 @hci_lmp_controller_baseband_cmd_handler(i16 zeroext, i8*, i32)',
                      '2a540812f4dd4802432b7513a7a48e91165b8b5e3e378dde1090c45bd323331f')
    return text


# lmp.c functions owned by src/btctrler/, by SDK body fingerprint.
LMP_OWNED = {
    # lmp_ssp.c
    'lmp_ecdh_publickey': '54b2e9baeb86783121c95e444e2b13a2a778bf9e4d063e657f13e34fc85cf371',
    'lmp_rx_encapsulated_payload': 'a19f3816b3c741960f1c371bb79528be764d66db3044b6e9d684bf16467a2360',
    'lmp_f3_function': '9d16f9189492f4efcc43fcfaefdae29c806a2533775abffeb6af00963ec6f99a',
    'master_first_dhkey_check': 'da1193e12d93f0b69986ded20322fa1dd840f5dc0a4a6136cdc970141b27908f',
    'lmp_dhkey_check_accept': 'c0125cd57b6681f4bbc51c95401a78b36790ab8aa2c3c6fc0cce9fc0016fc57d',
    'lmp_master_check_c': 'db88eae46dfd2560e7982c962f73358ecd3df20e97c6fa4633755f9347ed5c33',
    'lmp_tx_user_passkey_request_reply': 'bade7f8896576d28d97fbcf56ba2022e2e2878b5c4699ed4539337c17ba57240',
    # lmp_pairing_support.c
    'make_rand_num': 'a99f7c5108cde493b39d419ded476b9ee0034f38e080c9fe2b071c5900d673dc',
    'make_xor': 'd1e6d744b7502999ed1b1967dbcf387d6c740ce9621910e9f8c5bb206f2ad169',
    'lmp_secure_connections_enbale': '025ac7514e93f342da9eb7a88e4c5e6801bd45dbba1833d0699b8633b60ffab0',
    'lmp_rx_not_accepted_encryption_mode_req': 'ecbe31f2513ca9bcac807d9365d090c55ee306080f77b8e41747c461e45d7068',
    'lmp_rx_accepted_encryption_mode_req': '6bc84097b981958eba7ba16029abca9bebfa2f56f1cd03d481cf154164935550',
    'lmp_tx_encryption_key_size_req': 'd88227602bf8038fc51b533b354600b60d6d7e3c359c498de1a5ad68858d6366',
    'lmp_rx_accepted_encryption_key_size_req': '73b2545000cc512db305b8e8ab1de3a6c0d60f12d58f134b73be687a605f78a6',
    'lmp_tx_resume_encryption_req': '3283deb13a60684c24cc729e76d8550a2cf82839f2c74991e160a195ea856159',
    'lmp_rx_temp_key': '39871c9ba39c52da969fae2b7466dc4a08c92182997dfb71faec13924c0de8d5',
    'lmp_tx_keypress_notification': 'ccf6c3cf0f442f861ba248e1649cb488ab6a0e584f7295b0712a1b565b3e0222',
    'lmp_authenticated_by_using_one_of_three_algorithms': '06189c326cb25bed83a73e10c81cf6b31d09ef1c32d4ee9fbc9f25b39fc6fe3d',
    'lmp_io_capability_init': '4a0aef42fc9c6962c8ffff5a8e14f39511b7fcda47c61cfd16dbd6dbb847c04d',
    'lmp_rx_io_capability_req': '1540589b9fd0bbd6e8a4958095a9b0ec04f00842d4afc42f03f014ab3baee1ab',
    'lmp_rx_pairing_confirm': '1627ec179a5843e6204a9070b31c76b5f1198c2044b4f78b1a422d429f8e6af7',
    'lmp_rx_pairing_number': '2347307aa826c569b79b5f2ff7d4c2af0fe0f45cb8054e17a4a77f9913fede2e',
    # lmp_power.c
    'link_conn_rx_remote_max_power_res': 'f84816a096fe4ea0e3aa4528a95360ce483116346d78c23dbf649cdffaee67c6',
    'link_conn_rx_remote_min_power_res': '743e640b2c7144822f778f3fc5931a37ceab165a85d5ad14ccf980c916bfb42d',
    'lmp_rx_max_power': '900d02da7b8c424c5e70dc933e2d2835cea7b83efb18859b93dc0d48ca4b2adb',
    'lmp_rx_min_power': '43cc531de30870523b25a561e4043181c7e620d4441049e0cd777e6ce9818f45',
    'lmp_set_fix_power': 'c560c229bda5c6de62ac524158d4b8239e527727ad06e24f5fc9b729d6eebd6c',
    'lmp_rx_incr_power': 'f774d6be436fe7e686cd8fa2a9e3f65a64678a535fe02bf02dbccac15e4567ab',
    'lmp_rx_decr_power': 'c6cd8b088faee0c1ce68693d1aeea64f4600844e99d69c49ec80a3862a5918d3',
    'lmp_tx_power_req': 'f1f879173187cc78dee82651c69dd823f7fb8bc16da5da436bd02234b734720f',
    # lmp_clock_setup.c
    'lmp_rx_timing_accuracy_req': 'badd837bc85a3327d3c4c1c03dba5b5e3f8c84e17a263fd0a501ed93eaa5d3c4',
    'lmp_rx_timing_accuracy_res': '8d5c978f5c93f3cbc81279b96dd3236dcf1dc8c5174bae428599261625be2248',
    'lmp_tx_timing_accuracy_req': '9a431630a8c165c150b13505bb7ffd487cc6e31d3ba35584db47dab4a5dc13e6',
    'lmp_tx_clkoffset_req': '9e89ae2bf29409cd311df6043afb84e393195463f6b2d74f45c7c8bfe906132b',
    'lmp_rx_clkoffset_req': '6c68bd4f78ad5497eb6a2d029cf4c97b4d3575498450358b53f51801aaf181f6',
    'lmp_rx_clkoffset_res': 'c9449fad5129cb5ea7766bd9010476bcb72af680591372b2debd8128f48bf8fa',
    'lmp_rx_supervision_timeout': '93f9c0d91df2364938a1bc78b92175d0ba52156e8c86d822b7173122f4c52eef',
    'lmp_rx_quality_of_service_req': '3e6a4c5c3d79d592b53def6e162b6f5ace3ad4441385796da6b94a0ba54440e6',
    'lmp_rx_detch': 'db98461e95e58d463c9c1dd5ce7d6e53bd08bde44411aebe21741a5b97f33f73',
    'lmp_rx_host_connection_req': '529b1414eda88ede4f1d3c1af9264822206680a19e1f66a1de0e5fcd75a932c8',
    'lmp_rx_setup_complete': '92f8afe7c0d91911ed3c5af156934bb0778c7e59b619fb13cecc3c897615df96',
    'lmp_rx_accepted_host_connection_req': '1ae53e05b27a798beab36f7e7781f1e487772654100f44ea004d17a752415040',
    'lmp_rx_not_accepted_host_connection_req': '93d36ad6878345e3d1f4b9441b87d8801da13758c91d75fbe73ca41ebcfbfc12',
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
    # lmp_queries.c
    'bredr_get_rssi_for_address': 'cbecbbe77176b1b99004819743977a55f22d4648354e23fab25bb6ca2295cd55',
    'link_conn_get_rssi': '92937b01bb107ce61bae2d251c8a37d7544a2f1f44b544430ce5ab0d9fe79f96',
    'get_rssi_api': '78e8efc9144adca767d956463139eb7bf9bbd0ee7cc5b5f1db1c41c2a91e8764',
    'lmp_private_remote_addr_for_handler': 'ed8871c3c0727cc642062355acfe64f13ed7c05a632be41b0956e4e47465807d',
    'lmp_private_handler_for_remote_addr': '2e53995eee1f99a6ca8fe2ee01b6ecbc060e8934efa236a8eccd191971492859',
    'lmp_get_tws_phone_addr': 'a820dac4bf5ccb03828ae18d241ce36e84fe87052c31a2c0c3a508b4bd65bfca',
    'link_conn_set_max_slot': '56cac7efcded2df042f786392020a72a1b8a25cf2c49a826083103ad54a866b5',
    'link_conn_get_max_slot': 'eae6ef9fafe4b2ceabef1ed1fef2284f860e7c79073195b3dead83831bf33ba7',
    'bredr_set_dut_enble': 'd3c85e71321e527d290468873a9c0b799d83091475e826732d346f98670ff281',
    'bredr_get_dut_enable': '35f4551728a7bf60d350b3c59bca1fe358d95bf97de578cb97b1da0333c98803',
    'get_page_remote_name': '874b13889686e3bd6eb8e6b085378779403b1f8d28f81a5da917bb289fca593e',
    'get_updata_conn': 'a9676c6a5cd81b3587e9b6fb3fd4bcef2542bc0b1c3c23b6f0d6444a0b675257',
    # lmp_commands.c
    'lmp_hci_cmd_handler': '17ee480f2dbff3686f11027e7be6e5d084c71a16ae9bd10be75840d98e40b14b',
    'lmp_hci_cmd_handler_to_conn': 'ca26661983ea85f4776db58a44d5738f660fdf9831aadf6d36c14c9f92b2c93d',
    'lmp_hci_cmd_to_conn_handler': 'dba4d7dd9451deb3c7d6547c861bed8f27f70c39957bc4e0d2e19cbb6731c475',
    # lmp_acl.c
    'lmp_send_acl_u_packet_to_host': '1ca311a7ab84acaec6b852b68b11ad4515b6e405450c89e694916366888c122b',
    # lmp_suspend.c
    'lmp_tx_suspend': '70bb433a0c9dc41b993e46e539982e856a1026c178cdab08d04977cce5a5144e',
    'lmp_conn_suspend': '014d1d897388fcb90b8a2f227ba602145ebdcf21d8a5c21115534cca7b5308b7',
    'lmp_conn_resume': '185e34cf8138bb9dbba91396d42a88435f690fa54b3e7e44e7e84f7c35227fc3',
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
    # lmp_sniff_support.c (slot reset shares lmp_sniff.c defaults)
    'sniff_achor_point_hook': 'ed1d483f1992fcdb5d1af255e60d2d325f2ac2b1d44306e2cd0b19651606d25b',
    'sniff_hook': '48f51ac096eb90b3b6a2a1a0e2750a7307ce4da28a337df835eaeb7c9c240506',
    'sniff_negotiation_hook': 'b982fbb0b0fb76327f68ccacb1e7def3bfbc9913f3044a8451bb77d972f8c4d1',
    'lmp_rx_sniff_req_check': 'e07f8443a68c3978db37e3f44b7f7c16ced35249708be308dc8a1460d462a37b',
    'lmp_set_sniff_establish_by_remote': 'cd225a5c3525ca2d4bac4ba7d7ea7aae75006cbba203da316925afd95967e73a',
    'lmp_set_sniff_disable': 'ec134e0da3119382163efff9fc4f2724077c24a9c28ae78227d52ecff4ebf968',
    'tws_ble_sniff_slot_reset': '48cf417c8c19907233842051711a71b437340c4abe10a9099a06c3e85e4e5234',
    'get_1to2_conn_exit_sniff': 'f4172ad273e144237660870ded861cd9103bb2272ac4eeda0e5b551e03b4b6c8',
    'lmp_in_sniff_mode': '063fee56d3fc52c0cc6aa7df245e470a340de3edbe4acb7b024b08f1fb231d52',
    'lmp_sniff_pre_anchor_point_reset': '20322998433d1992e3a5857d9f26f86fb39921933637b8bf6f0055f2cfb5c39d',
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
    # lmp_state.c
    '__bt_updata_lmp_connection_resume': '79b68b0a6b542dee88873f6149840c70ffdc1ba4c87a2b2925b96a62782c0c3a',
    'a2dp_rx_notice_to_decode': 'bbb36e69a7b0818c4e977116e1c01f3d472a5e4042f515180092ca6e91c29d89',
    'acl_u_packet_analyse': 'ac9e9e169ab265396041f417d00de96191c1fce3d51d07eecaad57bde0d5c7d0',
    'bdhw_enable_afh': '874af7bdaef09ca0b6699e0459fb0c4fac2ea13ad856172146bb0daafadb98a5',
    'get_random_number': '88f32f040c935ef00d584e2947982e8a2c05f18c3fc5d63a6858701e6ed6ee18',
    'lmp_cmd_is_match_size': '0187d6c77fb80e16aba18d790491d2665167d7d498dfb9a8c6f9fbd7f490052c',
    'lmp_conn_idle': '6caa73d8d66c7cafb9b9f640b35da55c9237d3770867859d6ed9cbfe3d2a708c',
    'lmp_send_test_key': 'f49bde56acb285087f849ed61f6b741d9c1e19197eda330bef9d950e52cc78cc',
    'lmp_send_user_info': '303e3421779883afff60bdfc8adb0afae6531cb7074eebe4e0410a1aeef6413a',
    'wifibox_bt_freq_finish_post': 'd3392d6970810a9c2f46d154d6e5d7e81095cc39fdfa5f05b74a2cf2aa7d20fa',
    'is_call_now': '81535a3413420ef75559f0dab494fd313f6f86cf8c4016521e6d27c61510e51e',
    # lmp_esco.c
    'lmp_esco_sniff_check': 'b24105b1efa2de14ee9bb7191185908893eaa69df83c3917eea49f17ebaf47fa',
    'lmp_private_clear_sco_packet': '14993ae56430842adb01bf7ddd617534e1a01c704d77d6accefa790970b877a5',
    'remove_esco_link': '76fe51af977891170ccbee88eea0bb4b0116e0a73f3050830a7e09cc0e254152',
    'tws_fast_setup_esco_link': '9458cd474fdaea0e2f71306213923521b76ddfb813e76cf82e63bd559ec1023a',
    # lmp_acl.c (A2DP)
    '__lmp_private_clear_a2dp_packet': '82d462382d74d355fc2df60a88a7e01a1adb54aac0171fcd9ae78133010694d5',
    'lmp_private_clear_a2dp_packet': '396bbdb9a6981876664673931244edf03226d24a85d66c41c6603a14eb45a115',
    'lmp_private_free_sbc_packet': 'fd8e78428b0b9580ca3d730e6c19fa4df9848afa971b270dfcdf99c5be06e379',
    'lmp_private_get_sbc_data_len': '8385534808fe5e9ee7b79f7bf4bfc1093ee93cb5d6eda1ab57e3e89e867e3f72',
    'lmp_private_get_sbc_packet': 'caec794bf3537f1649d11e1667eb8c4d4407796071174824b5e9e8a78531df75',
}


# What is left of lmp.c once owned C replaces the above: nothing in this
# firmware calls it.  Firmware update over Bluetooth (bt_updata_*, uboot,
# lmp_update_*), test-box information and escape-3 requests, the A2DP and
# eSCO media APIs and their TWS helpers, and file-static helpers of those.
# The build drops the member, so a caller would fail the link.
LMP_UNUSED = frozenset("""
    INIT_LIST_HEAD __1to2_test_putchar_for_addr __bt_updata_save_connection_info
    __get_last_a2dp_seqn_from_list __get_rtp_header_len __list_del_entry
    __lmp_get_a2dp_packet_num bt_get_chip_key_crc bt_get_flash_id bt_get_sdk_ver_info
    bt_get_uesr_info bt_testbox_ex_info_fill_buf bt_testbox_ex_info_get_handle_register
    bt_updata_clr_flag bt_updata_control bt_updata_get_flag bt_updata_set_flag
    bt_updata_tx_detch bt_update_link_close check_1to2_conn_exit_sniff
    check_enter_esco_rx_bulk_change check_exit_esco_rx_bulk_change cpu_reset
    creat_esco_forward_standard esco_media_get_packet_num ex_info_total_len_get
    link_conn_a2dp_channel_close link_conn_a2dp_channel_open link_conn_disable_afh
    link_conn_pll_comp_reset link_conn_run link_conn_run_enable link_conn_run_slots
    link_conn_set_acl_u_tx_enable link_conn_set_afh_enable link_conn_set_esco_lt_addr
    link_conn_set_first_anchor_point_timer list_add_tail list_empty
    lmp_create_esco_hci_handle lmp_escape3_acl_u_s_send lmp_escape3_fre_offset
    lmp_escape3_get_remote_addr lmp_escape3_get_remote_addr_res lmp_esco_conn_malloc
    lmp_esco_negotiation_parameters lmp_esco_rejust_establish lmp_get_a2dp_data_cache_hi
    lmp_get_a2dp_data_cache_low lmp_get_conn_step lmp_get_sbc_remain_time_form_list
    lmp_private_a2dp_channel_exist lmp_private_close_sbc_channel lmp_private_dump_rx_time
    lmp_private_esco_suspend_resume lmp_private_fetch_sbc_packet lmp_private_fetch_sco_packet
    lmp_private_free_esco_packet lmp_private_get_esco_conn_exist lmp_private_get_esco_conn_num
    lmp_private_get_esco_data_len lmp_private_get_esco_packet
    lmp_private_get_esco_remain_buffer_size lmp_private_get_sbc_packet_num
    lmp_private_get_sbc_remain_time lmp_private_is_clearing_a2dp_packet
    lmp_private_open_sbc_channel lmp_private_sbc_source_slot lmp_private_send_esco_packet
    lmp_private_set_a2dp_channel lmp_set_features_req_step lmp_setup_esco
    lmp_tx_get_remote_addr_req lmp_update_exit lmp_update_init lmp_update_rx_handler_register
    lmp_user0_command_process reg_revic_buf_addr testbox_get_key_action_test_flag
    tws_check_sbc_data tws_lmp_clear_a2dp_packet uboot_rx_handler
    wait_enter_esco_rx_bulk_change
""".split())


def lmp_member(text):
    """lmp.c.o: check every SDK definition is owned (by fingerprint) or
    known unused; build_btctrler.py then drops the member."""
    return leftovers(own_all(text, LMP_OWNED), 'lmp.c', LMP_UNUSED)


def leftovers(text, member, expected):
    """For a member the build drops: what owned C does not replace must be
    exactly the expected (unused) definitions."""
    left = set(re.findall(r'^define [^\n]*@([\w.]+)\(', text, re.M))
    if left != set(expected):
        raise RuntimeError(f'controller: {member} leftovers changed: {sorted(left ^ set(expected))}')
    return text


# Remaining common and Classic HCI boundaries (hci_misc.c).
DEVICE_MISC_OWNED = {
    # device_manager.c
    'set_bt_full_name_event': 'bf968e99652177005dabd0f8a1269468a891e00f23260cddba9520176226681e',
    'set_bt_need_enc': '2b95a01253bffe34f2accb76cfa1fba66a7dac10754d9184f1b55379ca9467d6',
    'set_bt_version': '0cdfd1bf9fc1ce3935ad4f762fd1822aac14608fd82343e9efc43864b61f214c',
    'set_bt_afh_classs_enc': '09cfbfd2623e3c2e7c2584b0c2bd59533d502b91e71e48babee78c166109d49f',
    'set_bt_enhanced_power_control': 'bcc3b919feec641b53b9aaf2120e162c36a5a4485d2aa7ec51b51a97f208a625',
    'set_bt_data_rate_acl_3mbs_mode': '58938c5c23c80de79c7f647323674a2f438af9a721f80fbf58b0e5732e397445',
    'set_bt_data_rate_acl_2mbs_mode': '3a1b90cd81ba562d1db36893b10564283b9d2d9ceb3c31ca069373045d4f9d36',
    'hci_read_remote_version_information_show': '0f977d31db7b124f81ce33cd07c0937577eb9d8607fbf2d2e5654c4f4834ae35',
    # hci_misc.c
    'hci_link_control_cmd_handler': '62f3c9ba9d259dc6343af6d8d1fd524d18a83de8c95e7d6b3f2724e1130cebb6',
    'hci_link_policy_cmd_handler': '4589a777f3d37fb0f8158cb4789fda40721dcbfd48efd86d8fef7ba4401ed662',
    'hci_status_parameters_cmd_handler': '2bddaf215c4f586c4e034dfefb68e477b76040832d6d824ed9a8f92e535fda42',
    'hci_testing_cmd_handler': '1d50a9019c8da7083cb854634b9ad60401d4810031672aa876d04f235ae78094',
}
CLASSIC_MISC_OWNED = {
    'hci_lmp_vendor_cmd_handler': 'd8dfd1ea6bc975a2885a6ce969e6663e8e0ccddff9c39b76f8aa531ac537ceee',
    'hci_lmp_informational_parameters_cmd_handler': 'f395dc5adb4a2a5811ec09a2c67833db7d9a6ab4399a2bae7f0719143de52d69',
    'hci_lmp_status_parameters_cmd_handler': 'ed89d10902bbcf2262b18051331d4fe1a3192471724574dbf6ea82cb833e4625',
    'hci_lmp_testing_cmd_handler': '62c269d8ca8b38ed378bd5861862304d856c361805bb1cf11a5c6c378de5dba4',
}

def device_misc(text):
    return leftovers(own_all(text, DEVICE_MISC_OWNED), 'device_manager.c', ())


def classic_misc(text):
    return leftovers(own_all(text, CLASSIC_MISC_OWNED), 'lmp_hci_cmd_parser.c', {'cpu_reset'})


# lmp_utils.c: what bredr_link.c and the pairing code use.
LMP_UTILS_OWNED = {
    'endian_change': 'ce004fd52165d5aa9b33b370ce33488390c4561c9aaf69a34551f7e8e2576382',
    '__get_access_addr': '20d892fb935170a14e8b5cff4758432131e9166a42c8a416fd30e4cc2abf9a58',
}


def lmp_utils(text):
    return leftovers(own_all(text, LMP_UTILS_OWNED), 'lmp_utils.c', {
        'lmp_bt_store_16', 'lmp_bt_store_32', 'lmp_net_store_16', 'lmp_net_store_32',
        'lmp_bt_flip_addr', 'lmp_bt_flip_access_addr', 'lmp_char_for_nibble', 'lmp_bd_addr_to_str'})


# misc_hw.c: CRC wrappers and the BR23 random register reader.
MISC_HW_OWNED = {
    'crc_get_32bit': '18106245b18f3ec48fb97b567f5ad32892bfda7b00f622952ff2a64b05892b77',
    'crc_get_16bit': '436dfae2d1465df8f340e12303de3ea0beea351b991e5e5f3689a23bcfe2288a',
    'pseudo_random_genrate': '4a331edfa504d74d287638d468c6eab958cc537190dea15c61d0fc4bc7b17de3',
}


def misc_hw(text):
    return leftovers(own_all(text, MISC_HW_OWNED), 'misc_hw.c', set())


# bredr_utils.c: Classic clock arithmetic.
BREDR_UTILS_OWNED = {
    'bredr_clkn_after': 'adef4c01adbd5a7099a33030e495fc477d40b40b508c7a4eaee5b0857447443c',
    'bredr_clkn_before': 'ef85a3de9e2d6356b5a6d397e8f94d5b3141f89c1219ad4a7e3c0cc4d15dbaf1',
    'bredr_clkn2offset': '1fe0bff49ecea1cf55a5e24bf7de3c25980087eb4c5c17836f2a717ecc60df37',
    'bredr_offset2clkn': 'b8135cbdbd04cce807390e55c9a454308f2176d1ec242fd085679e5843bfec2a',
}


def bredr_utils(text):
    return leftovers(own_all(text, BREDR_UTILS_OWNED), 'bredr_utils.c', set())


# ll_white_list.c: BLE address white-list storage and traversal.
LL_WHITE_LIST_OWNED = {
    'll_white_list_match': '28b04ca428804366dc911987c3fc9d887124754589a8f128ad6938bffbc1e472',
    'll_white_list_init': '034e6a72a88f8735ef36a9777589b68193be58ad21ef32864738d55a4e741e15',
    'll_white_list_add': '2a31cca707c32f7430bdc725dbf4326dac36cff4d2e281a2db19f3850e82b2aa',
    'll_white_list_remove': '5951f15c43cdd8462eb2be5d3fe843e888221c23f7ca83d1d0eb15d705566151',
    'INIT_LIST_HEAD': '8f6571b9ca7a19a8015a579f91cd412ce8b4274d88272402666e5f6c1275656a',
    'list_add': 'd40ee0538c80a33e346cefe5dabcafb49394d5005e21216ba362e3154618bc74',
    'list_del': '9c990afcdc09fe6d3bdaa1f587360ae9026eaa8f8777258b2b6b2d92c6a05639',
}


def ll_white_list(text):
    return leftovers(own_all(text, LL_WHITE_LIST_OWNED), 'll_white_list.c',
                     {'cpu_reset'})


# bredr_slot_timer.c: the Classic baseband's eight slot-clock timers.
BREDR_SLOT_TIMER_OWNED = {
    'slot_timer_irq_get_time_out': 'c38110c0ea8e8c8b534add8d5a89a44b899328b8634b3059af54331db16c1cb8',
    'slot_timer_init': 'b24ce8ae53efc3fa204ca4d7d54d9dcba1476cbc92921717edd574b5b1e00340',
    'slot_timer_uninit': 'fabf00456baa75e6fa2f078c145db96ee9e212315c446738205aec01f65bb196',
    'slot_timer_get': 'b8d4edb22013dc65daa8a18e8310eddfa9163ea821441c3ebe8b0ddcbad83e8b',
    'slot_timer_info': '3bdbcd3dbbb6cbf9ea50fad3b49f544d54d79f83e68c6b1a720de38284d5c444',
    'slot_timer_set_ext': '0435fe76e617e040fd41315c7384485638b93f6b8272cc495144c30595343781',
    'slot_timer_set': '15490ab32e8a170759c89987186efa08895a79af096794d3e295c0aa3b236229',
    'slot_timer_get_func': '6a99f7ba0c81493bdfbdf0cdb55b3f25ca00831b2cfbe49c5941755aaf873c34',
    'slot_timer_reset': 'd7868e57d3a286aba5492d2b3d24d0cc8f4edfc50619a2009f13f51dbdebb2a9',
    'slot_timer_put': 'f1c5a42e0b8cd4628a4516edba933e945b87949ede347391cf6b6cd5b34ef95a',
    'slot_timer_disable': '86f195e332ccc0686e78a056ee55eadb8317477ea3a5e5b6637fb411bb01bdf7',
    'slot_timer_enable': 'c0c70ead125e193a8a308bae9e2e226febe454658deef4233a454e2282b2bc30',
    'slot_timer_remain': 'c67ac1514478fb6c71b77ff9385098b90a8104aa9946d142f3225f09817ed22d',
    'slot_timer_handler': '5c0940d7141914546df890f083d2078be1f34d0d520348d48c075d116231998b',
    'slot_timer_oneshort_set': '314f5adca4d46bbbdd6ec2a2bea2a485acb1ea109d120543d6a0911b07a3ff2d',
    'slot_timer_irq_handler': '5ad6c8401ac7e56260e09f53169f102cb43f1b49318f84030b617dae1d640e02',
}


def bredr_slot_timer(text):
    return leftovers(own_all(text, BREDR_SLOT_TIMER_OWNED), 'bredr_slot_timer.c', {'cpu_reset'})

# ll_ext_init.c: BLE 5 extended initiating (LE Extended Create Connection link setup).
LL_EXT_INIT_OWNED = {
    'll_ext_init_rx_probe_handler': '0b99387c6988184d2c9b24fdce519245e0c21f14a7694099d6dd9bb29a760408',
    'll_ext_init_event_probe_handler': 'ef9f23ea172c70e135b1bb3c73abb8117446aa7c1c225cbbcab140f284fc275f',
    'destroy_le_ext_init': '199a808c7e1c0655754aaaeb9c9bed0f897bd01855516e17dbc03df250d14f9b',
    'll_ext_init_close': '34192947c6c9b0fafe47903e16d06668ad0e387ee43b2d320dd1639ab868399b',
    'll_ext_init_rx_post_process': '4e6dbf7836e359a3ba036cb285aced7d0648c61a35b537f13b2b9155b9df5363',
    'll_ext_init_rx_post': '64c797d70c87a0a80397d52aff34577e60ad0e4e9cfb1396d408d21218225dd8',
    'll_ext_init_open': '2e8880f337e686379b12c483553190f9bf2f6028d6d1dff40a867e1ec97de5e8',
}


def ll_ext_init(text):
    return leftovers(own_all(text, LL_EXT_INIT_OWNED), 'll_ext_init.c',
                     {})


# ll_extended_advertising.c: BLE 5 extended roles' shared link plumbing.
LL_EXTENDED_ADVERTISING_OWNED = {
    '__set_ll_ext_local_addr': '16516d4012920427528cdf5c2d7d48c9c5435eddb8f2ffc5286bf5f8011ecdcc',
    '__new_ext_link': 'afd64386df711f81d7bd620c9b9337d7f02fc754e3c86b65e60f61f371e1c28d',
    '__ll_ext_link_close': '0ab33f5cb2d2d33b9c541d35cea29df14bcf41b0bbea8019fb690d2eab4f83e8',
    'll_ext_create_legacy_connection': 'b431ccf25522ba3c5f0da1e8e57ba61bb44ac5b53d830110fb233811587b106a',
    'll_ext_destroy': 'b9af8c6da7534a2a73ac2abe98142026a84e80432b6cb825ea134dfcb628e568',
    'll_ext_rx_post_process': 'a0f3675dbe3b0cad2e029f134a870c0c91415aae13ecba44a3665195bcec84cd',
    'll_ext_handle_malloc': '24c24cd3b598277487852ce0b80ff34170f9a1ee1002c554a246183a2b690927',
    'INIT_LIST_HEAD': 'e9d9f58427a30a55e2633701c13130cc33bae32dd3a8b36c9122f95b50c48977',
}


def ll_extended_advertising(text):
    return leftovers(own_all(text, LL_EXTENDED_ADVERTISING_OWNED), 'll_extended_advertising.c',
                     {})


# ll_master.c: BLE master-role receive handling (connection complete, data and LL control PDUs).
LL_MASTER_OWNED = {
    'master_rx_conn_post_handler': '66f9b5c9bd26805964438eabe775b074783d903be04f020c5e9bbb01a65d04ee',
    'conn_master_rx_post_handler': '0be85a8394dff0e863f35dcf1ed51cfe8a35d802b9c88216d5b1fe3c6f4bb301',
    '__conn_master_rx_post_handler': 'a44b9b0a3333f12f4f04d1587a8af9548486b077ef09589fb473d8305c6a6287',
    '__master_receive_terminte_ind_done': '3823c9f2989de87df7457272da259beb8350ad054c09a884c4820bc1483a3a07',
    '__master_ll_receive_start_enc_req': '5e938cf79c1ee4e55ba1955981787e47678ee150c3a7d2bb5ee1a904f187bc83',
    '__master_ll_send_conn_update_req_auto': '72308eab31c57902f34f34403cc0e17af7550195d1f7e3f86c356095b9080d52',
    '__master_ll_send_conn_update_req_auto_done': '3f1c707ddd7a2a3c5a11b35d0fee11d2682b3b97b46f5f396990e69373677410',
}


def ll_master(text):
    return leftovers(own_all(text, LL_MASTER_OWNED), 'll_master.c',
                     {'cpu_reset'})


# ll_data_extension.c: LE data length extension tables and per-link effective lengths.
LL_DATA_EXTENSION_OWNED = {
    'll_read_maximum_data_length': '923d30a9f68b0464c1fbf73eb65e86bf15bd526a7aa760e2d04c8ad732fec8b6',
    'll_read_suggested_default_data_length': 'de123ee5bc9dbc6bbfcbe8893272bbbe375eee1c6c6f67674792904460c1d080',
    'll_write_suggested_default_data_length': 'fe8068bc0356ff0c19e031df78184add683654791289f5d4caf988f6c89dc618',
    '__set_ll_effective_data_args': '5b1b1f740290008ac385d9b6388f7debc4b8562655ae4e38e4713883eafeb8d4',
    'll_data_extension_init': '47770f228f03fbaeac410e401c9042cf92c1fb90da1aa6ba4301b9de16b9aa76',
    'link_data_extension_init': '86a9c5b64f57c640ba60e7b754e558967a0008784fedc24590b6f5bd105439d3',
    '__ll_receive_length_req': 'ac938a980153c824f86eb6013816b16481c899eaf08b3460480585725679c7d2',
}


def ll_data_extension(text):
    return leftovers(own_all(text, LL_DATA_EXTENSION_OWNED), 'll_data_extension.c',
                     {})


# ll_common_iso.c: LE isochronous common layer (HCI ISO data entry, header unpack, CIS fragmentation, BIS codec send).
LL_COMMON_ISO_OWNED = {
    'lctrIsoSendCodecSdu': 'b04789321618bf2edd25b625534e0e94b6ca428a7e6853b46e9a99e4a171e85b',
    'hci_push_iso_sdu': 'b214a9f22b28c397803a230ffd1f1780eb53894a311dfaa4e7b4811d5d6be581',
    'll_iso_unpack_hdr': '1356cdf51846773aac0df58daf758f2b084cf3de6ece6ef826b279fb47491e42',
    'll_iso_cis_sdu_fragmentation': '3ed488e47f47e08d0c8f0b43c5b4182daa738adcaf45fabbad2c83838d095b2c',
}


def ll_common_iso(text):
    return leftovers(own_all(text, LL_COMMON_ISO_OWNED), 'll_common_iso.c',
                     {'ll_iso_pack_hdr', 'cpu_reset', 'le_cmd_set_iso', 'll_bis_process', 'll_iso_process', 'll_iso_init', 'll_iso_destroy', 'll_iso_cis_sdu_recombination', 'll_iso_cis_sdu_segmentation', 'll_iso_cis_sdu_reassembly', 'll_isoal_test', 'll_isoal_cis_fragmentation_test', 'll_isoal_cis_recombination_test', 'll_isoal_cis_segmentation_test', 'll_isoal_cis_reassembly_test', 'list_add_tail', 'll_iso_pack_seg_hdr'})


# ll_bis.c: LE broadcast isochronous (BIG/BIS) bookkeeping, data path and group session key.
LL_BIS_OWNED = {
    'll_bis_by_handle': 'dcbae612fd34e1e204573a918dbc6531154bff69b379fc6e766f52228a132070',
    'll_bis_handle_alloc': '1e5beabbacb6b25da4fd070ff1f5a6b7cec9117445a5e808c26006558b4d822a',
    'll_bis_alloc': '502c957b008770f5a000386b0e158b334fa665186c92f3528927bb6b9a256e84',
    'll_bis_free': '1b3be34198ecc5b280342ba1ac12324a60a6a2dfab5c6635b94f4148ae7b8209',
    'list_del': '684348cc3ee335f04c3ab7778ddaab0fcfadfa8c9c53496c26c8010c053b01f1',
    'll_big_by_handle': '6d6137d0701378e7a6ed7d732505d600d552d1eed4f981289add7a5ceb870cc7',
    'll_big_alloc': 'b832bd47014dfdea8ba590bb4f28e7ea63ca6ffb9580faa16ab1654f0ff1aad9',
    'INIT_LIST_HEAD': '8f2efb5fe248cb951b50d49edb15fbcd095bdf2322ffb3cfa6f076bb4f94fcda',
    'list_add_tail': '44a82dcb47fb64420126d37ae0e8ef3e79cb17c8afb37ad391e816bb2d49544b',
    'll_big_free': 'a304fe14df3bb75dcf347226070d6f90ab5adbf7344cfffdff87ed1b7ff8ea11',
    'll_bis_setup_iso_data_path': 'f6b48f76252fb643f0e3bca8227b10d921bbdbb754278c6cba7458bb7c07a39e',
    'll_get_number_avail_bis': '805c70b9ef0c9b83d8ba5577a39c5f2fac158679fa58556adc30d9711f96f6f0',
    'lctrBisCalcGroupSessionKey': 'e9b5a994f84ec57cb6b16994e8edaff5b86497b97b3dc5cb47a89a94ba154ab4',
    'll_bis_setup': 'd3a3c6cd24b3ecf12a00bf8fd867c08b7f75fd6cc453e755b515130529210692',
}


def ll_bis(text):
    return leftovers(own_all(text, LL_BIS_OWNED), 'll_bis.c',
                     {'cpu_reset', 'lctrComputeBisAccessAddr', 'll_bis_tx_test', 'll_bis_rx_test', 'll_bis_read_test_counters', 'll_bis_init', 'll_bis_msg_handler', 'll_bis_close', 'list_empty'})


# ll_cis.c: LE connected isochronous (CIG/CIS) bookkeeping, lookups and sub-event durations.
LL_CIS_OWNED = {
    'll_cig_by_id': '8e685c71b90c24e8ff24166bf5c3e98922e459322846446f7bb0e1d6a45c8a4e',
    'll_cig_alloc': 'bfc81edd31ce8492af76890a6b18b782ff7f01b118d502351ed577164a34b310',
    'INIT_LIST_HEAD': '085a4cfd7bb506cd47fdbaea6bfa84a5b4b9d89eb0da3c4715fe74cc4d4e9cf8',
    'list_add_tail': '61d69e0b56666a3f4f0f8c76323852eb56dc8fcea331bf870e4e1d6da2a3fd4d',
    'll_cig_free': 'd567ab18e7b8834b8edde1566dfaebc52d08bdb81c057ed82aa8f9e55e293fd8',
    'list_empty': 'b7bb2330b62cb936192f1809021da4a278f9e5a7cd17d1a82e2b098279cbfd71',
    'll_cis_free': '120f90030181e0b06b0f78134afabbd5cf4022ab479b4170f8b9ff169eaef602',
    'list_del': '321c1354175bdecf8f7e938ab79a610cc1ef409646320ae6286ee227400b0fee',
    'll_cig_add_cis': '1715ae8dccefd3833726dd082bd9a09eb03c018b7c55b92eac73ab5d16a684e9',
    'list_add': '2be4e010559225613a2910df62910e1bfdec1b255c63aabe2d927052ade74526',
    'll_cig_add_cis_tail': '09443049ee2fa4c2add84b9a6f6da85d4258f6a0864bf6f094687d72c259573e',
    'll_cis_by_handle': '46350f27182e212b9ce3903fc0e08fc1987062e7348c7c40b284ac268c029af5',
    'll_cis_handle_alloc': '210f5195be216a91d4816897e1689146af763457cd31ad09ea8d0ccde04c08fa',
    'll_cis_alloc': '0f329ed96a4cfd3eb58305030315bf079f970f0d8cddbf48c43bf7d67680f13e',
    'll_cis_by_id': '8cd95b83e97a1c4a0e5c1ea521434775b34203759b85ad531e667d8044be47db',
    'lctrCisIsHeadCis': 'aeaefc9de411a2ae21e82ff4c72074f5e5bc7431318dec23ac0ba2249a6cefd5',
    'list_is_head': '9af85aa289963055242d54194e67ec9f9a855f6cd7853388a8daa96e84c8162e',
    'll_cis_destory': '5a0dd94ace96f984b625309f0dd62bedfbe11d8a6bed77452b10a264fe95bd39',
    'll_cis_is_established': '31ec8b143a6ab11faae974be5d9130f710874033cf7d2ef701af95e806fc7131',
    'll_get_number_enable_cis': 'c9c1561e7d55b6e2a8cf19504117459e3345ea9191f828119452cbc027bf7395',
    'll_get_number_avail_cis': '2d648a225df68d2c6496f3809fa303276c359f2a0ba6ff2c80e33d3b643ceaae',
    'll_cis_calc_subevt_sequential_us': '998f84c55301b0863d033041509defd8f22375175ea4566b399874fb951404fc',
    'll_cis_calc_subevt_interleaved_us': '8fd39d5a810e8e8062162e149fe707b56cf935c9740df36d178e7fc2556123a0',
}


def ll_cis(text):
    return leftovers(own_all(text, LL_CIS_OWNED), 'll_cis.c',
                     {'ll_cig_remove_cis', 'll_cig_get_head_cis', 'cpu_reset', 'll_cis_established', 'll_cis_established_fail', 'll_get_number_enable_cis_test', 'll_setup_channel_param', 'll_cis_tx_test', 'll_cis_rx_test', 'll_cis_read_test_counters', 'll_cis_init', 'll_cis_msg_handler', 'll_cis_close', 'hci_le_cis_established_event'})


# authentication.c: the Classic legacy security functions (SAFER+).  The
# static helpers are owned too; only cpu_reset, the assertion helper, is left.
AUTHENTICATION_OWNED = {
    'function_E1': 'a72b789192c9fac78da59d296d23bcd2213e4fb34090d06f88195e1c00de52d4',
    'function_E13': 'd3df5aa80ebe4a3d458445fea7a85d12695853325a48b5efa02ccecc3142eca7',
    'function_E21': 'e214c66dc302415fa70de1afe98b8983a8080be7c8dee248afd23a9be4785cfe',
    'function_E22': 'af6972b7df8a2e805c257c5592096f3f6bdb023609787eb3f1e3f148980c8c83',
    'function_E3': '02afb26fd07205263c9f481c49e91a051b040588afe1a5b1948baad9b26fca55',
    'function_Ar01': 'e17bdc61a123d1efb6a99c43e5b12cfa8053760683483ff059bad94314307b49',
    'roundkeygenerate': 'cc82a5e7ff979a56d7bd0cf346c6e98ef1e399ddc759289978c8252c55aeebfe',
    'Entrypt_Key_Length_Change': 'b3bb752e63672c7b6c657d88b0517363c7e652019d2b99e37a36ea15ea483a1d',
    'pht': 'b35ea99347f8976bd5edfbdef7dc0cbc27b2c9f640270bbe7af40a05f2ec76fd',
    'premute': 'e0f4b36fb348a66093eab0cc937df3e6204928d93b53c72ce31b2da8188fcba3',
}


def authentication(text):
    return leftovers(own_all(text, AUTHENTICATION_OWNED), 'authentication.c', {'cpu_reset'})


# Public Classic HCI procedures (lmp_hci_procedures.c).
HCI_PROCEDURES_OWNED = {
    # lmp_hci_control.c
    'get_tws_internal_addr': 'aa55524c3e5bb0bf4736a9fabcd1aa384a3116150f132b20d547099a3573204a',
    'lmp_hci_write_local_priv_version': '6e4c2f7942ac16eeaeeeef7e1e39a80be09b981e2721d74a1fc8d2fa4d66b4b3',
    'lmp_hci_remote_name_request': 'f74afdd4004901142d260bfe410bb1ea76876fa0aea00c044afe74cf11278cd9',
    'lmp_hci_create_connection': 'acbaaa024d0a3a6c7b6d8031db2b84148e87b969ec313332c016ddb63079c934',
    'lmp_hci_cancel_page': '7f2fd2f561d4d42459a800b68c47fc6bcca168602a92bcf0d7f470d436b91c31',
    'lmp_hci_connection_cancel': '98781936babdbcac260d430e246bd560faa2a015fce9f9fc5dbb2264ffc7c951',
    'lmp_hci_inquiry': '6ffe63f399a79f259bbf299cbd1924415451d85688576ee31df15ead2c289be1',
    'lmp_hci_cancel_inquiry': '356402f1262f8c86d853c0742c0eeeae00e2ab8c00b95c259915a18e9f373e96',
    'lmp_hci_remote_name_request_cus': '26c2a0c1ea038133dfb4f9850644cb20708beea8ec9dd9300bacf607c7bb4f47',
    'lmp_hci_tx_channel_chassification': '702d4820023f80bdf7e951c89a41e3bfed791c0c889893ae1830d2e960e58ccb',
    'lmp_hci_setup_sync_connection': '998ce5b78f70c5e9fdf26826eafbdec86dcb9ebb49e51a86917cec3c36dfa6b9',
    'lmp_hci_accept_sco_connection_request': '6659c3657c59f9483fb49799e7c187f30e2429986d018697fcdb79bc8ba8c3fb',
    # lmp_hci_pairing.c
    'lmp_hci_io_capability_request_reply': 'b4cad9061c3982c36977b46c8c997f5615f919f6720ebef87466e2600011f936',
    'lmp_hci_user_confirmation_request_reply': 'e0dcab7ea64cc1c6ae53f2fc635f20c3243fb81ac80040c0d407b747be3914b0',
    'lmp_hci_user_confirmation_request_negative_reply': 'e80fc256a8a9cd841684b77ea650fad789fc50d5bf9927fea73689720883a5ef',
    'lmp_hci_pin_code_request_negative_reply': '84436de594a7644750a3f88db057df42cc8059e4b560edf3970106f6363d2d14',
    'lmp_hci_link_key_request_negative_reply': '087333d729320ba59d67ecb74d91b2087ef535587a9f9d0c7c01e19a4b816b26',
    'lmp_hci_accept_connection_request': '982a8e141a251ebef08e3fdff388ecc15453f48c50ba113e4d6226a06089a506',
    'lmp_hci_reject_connection_request': 'ecb362f2db03728ffacb84636572bd27e1c9db614abc979428762dd160ababf2',
    'lmp_hci_send_keypress_notification': '8af762b4bc74daa59c479bb8133f3ab43796e92cf8f92d75831b68099e515f75',
    'lmp_hci_user_keypress_request_reply': 'e080a949ea072f287dc7a4315b6e0aef9d23b5c0daa3330e49196f758d0f1713',
    'lmp_hci_user_keypress_request_negative_reply': 'af64a68d626f77710e825bf72be3520b95d632c162c4971bfa5183e6ec7c1f23',
    'lmp_hci_link_key_request_reply': '7743276f3f31a16dd0890f557ffb81bd7d39b34dc2b019a1d6b8c07a6a0a51b3',
    'lmp_hci_pin_code_request_reply': 'e18fe122e4405a41f82926b2d75848afbcc234854d86ea383145ba3246c4d1ae',
    'lmp_hci_switch_role_command': '06c38d43a534ac713ff5faae46a7718a293963afa436a5acacd52311fc304c56',
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
    return leftovers(own_all(text, HCI_PROCEDURES_OWNED), 'lmp_hci.c', {'cpu_reset'})


TASK_QUEUE_OWNED = {
    'lmp_hci_cmd_to_conn_for_addr': 'd88e93d1006482ef0c99b32c5c6b985e554e620f4071e0cb6e61452c3938ccd3',
    'lmp_hci_cmd_to_conn_for_handle': '173d4cc85514763cc762ad4a304bff7697b738efbb647444bf2680e2ea4cad9f',
    'lmp_hci_cmd_to_conn': '095fe6b2f73d1ae460c444aac7a6cab67417950453728f19d5387019e33d7530',
    '__cmd_to_lmp_conn': '0c4164d9c7f9cbdd7c3d38531e07611ef0763bbe765ac8b93b1cf91a6be12fe3',
    'btctrler_hci_cmd_to_task': 'daefe5137ee2a25a3e1dee986c2665c0186f739c1011af4bad76dee30393a1d8',
    'bredr_link_event': 'f4c84a84241404690a3d3c11accff94ab6009a88914b98b9f9aa6803c19dfc10',
    'btctrl_msg_qfull_handler': 'cf10ddef85a004df1c0126d66357cfb97763925a9c4c96ac31e636c0abbf0468',
}


def task_queue(text):
    return own_all(text, TASK_QUEUE_OWNED)


TASK_LIFECYCLE_OWNED = {
    'btctrler_task_ready': 'd8699f271c88e96b0c02257e587ed8604255d5bbb7ef340c593db2574cefa3cc',
    'btctrler_resume_req': '553816fd92319c0f880a8ac5cb4d66929536a0e9ac039f5f42f148d015f9c48b',
    'bredr_bulk_change': '6ef78110a1911ad83d33a369fd8168a64855728fd2e54c31cf1334aa86f4ce96',
    'btctrler_suspend': 'e3b925e4363cfc833833a77c1030492105b628c8aa28ffe61a40111cf0b9e5a5',
    'btctrler_resume': 'c3b3deb84fad54ad33ea6e6f3f71b22aa83c7ad27620cccf703e2c6dd85d0401',
    'bredr_bulk_change_rx_bulk': 'e9b75932916bd773fb705e832bc1120b2a501d10c5f95d38bcfac343c4012753',
    'btctrler_task_init': '6a71eee6342ad4665d615eecf514c06dc1c2153f6e9b49d90801c32f1ced54a3',
    'btctrler_task_exit': '90453da9077e40f3d7c3fe1526106c9a2e013d60b1491956f77dc66cd7451836',
    'btctrler_task_close_bredr': '2fbbe6fae1dcc2f51df1583ee3607619dc878787e2da11192dc1ea9ae6e6cc4f',
    'btctrler_task_init_bredr': 'c9117126b1a45ea973a530e7729aae03c0c1add893a7344f1331a805c3235a74',
}


def task_lifecycle(text):
    text = own_all(text, TASK_LIFECYCLE_OWNED)
    return text


TASK_DISPATCH_OWNED = {
    'btctrler_task': '351593bc57a870325c99de59fb20ca73b684817ad4be833e66d6889ddca58caf',
    'btctrler_testbox_update_msg_handle_register': '650df8b3bb626ccde86bfa0d6970b770bca99235f158fd96f1d63df462ccf8ba',
    'bt_task_create': 'a4045a715cfbbbbe67aeea437b3c0ad30dc8ca72ea6f12b2858ef40cdf27e37a',
    'bt_task_delete': '9496249c52e60f49545185ada6a542fa5d05197e94a216186344881c3c94a13b',
    'bt_task_suspend': 'ae718038a9e0d24ef0ecdc0ec3903789990811a67f4673368e4f65ab698b3f6f',
    'bt_task_resume': '2acb0467e07ce48208491696e54f7b75beea7ba9d2a8646fbdf89fb6a2a39683',
    'btctrl_msg_qfull_hook': 'b501532b8d7b1e23d18c709987be7d062ad57c6441b567353d3b7024563643f5',
}

BTENCRY_TASK_OWNED = {
    'btencry_task_init': '48e8802756d6196a85bf125171cea8eec51c479ff9554599b40fd993a3306181',
    'btencry_task_exit': '3b889a228102c58fe22c830aeec9b533176b93f48efbde2cda87178978587ea7',
    'btencry_msg_to_task': '3551ec88a94a83ac077ad5a7d374c1b98248a2c154a08167c3a78dd0debba32f',
    'btencry_function_E1': 'bd38321ce5d939d80f022110b8c609da5fd131880d4bd23740bbf00b0d973723',
    'btencry_function_E21': '6fd6c43cdc66d715804742634b369568f26b3d5c4775b72548634f9d67b17dde',
    'btencry_function_E22': '3dba748e8cb6f9ce07d17ecf2b58ccb99ace2af5c94474fef179334ffa4e29b0',
    'btencry_function_E3': '6cc1b1339012660e3b4d7fd1d8b0e954acdfa2cdb54e6923e8ba526ed5040867',
    'btencry_g_function': '2543b07bce8c2cde0525022fdcd22042d56375d474c3cfc2212ff3491f310d9b',
    'btencry_f1_function': '14d2aeff9fc14814b99c90794efb25724c5741f68ac8e601ca6929055b2fd0ed',
    'btencry_f2_function': '6594756a053eabfdb548a6816ae288d7264ddc5c7e6faf4fdf2aa2bd7629dff3',
    'btencry_f3_function': 'bbf0c3140057872b08452bd0612d61885e790b5969d1753ef76c7d9a35635ecf',
    'btencry_uECC_compute_public_key': '58a4c3ea98db3bbe427b1900a00abdb8550938e2288a92e4b44cb33595bb7f85',
    'btencry_uECC_shared_secret': 'd7dd493c06a92d85a926f9cb6f8308ffeed96259c0e34845228afa41758463ab',
    'uECC_shared_secret_result': '656e5585476b205b5de1a001712278520ad72018427efb8a6dc002b14fdd055e',
    'function_E1_api': 'd1cb10aad0cdf5afbddf4d4648159e546b58361b64f923afdc0429d73e671751',
    'function_E21_api': 'a2e936b75ef03a95812f06f8de51feece9998b7014c6a5035827af19c564dfad',
    'function_E22_api': 'ecaa42d20a0dc6e9eca6833ef645141527cd6b6872b035d486f68536645bc45f',
    'function_E3_api': '767d6fa03ed651f60e949e955b2cbdcd01228733e9feee3f23498d19d6760612',
    'g_function_api': 'fb294811b38c4ec0f7d60498929bba55cca7663648c87b98785d99750669e0dd',
    'f1_function_api': '4eebb28f0d67c8f45dab03a7b9303e3e7c43b3a9ca78cb18e7ef5def2c6ae1c2',
    'f2_function_api': '1e567f307e592abdc5568137b50bc18823b6259ff6fad369270f16a6935b2bd8',
    'f3_function_api': '760a9341a2d04c11fb856052174fd78a3956b531e24b4a7cea098412f0b82984',
    'uECC_compute_public_key_api': '8a17b1ff662f06cbd43be1217ab28532e45a30820ae5d8a36cd99644c0593af3',
    'uECC_shared_secret_api': '125fb2f98eb63886de54b89084b8fb1d7a282be5da2c3b002c913923b72af97c',
    'btencry_task': '6527b5a672435f9cadd27942b4d64d2b97a2df4db731d1e1719901483150c0e9',
}


def btencry_task(text):
    text = own_all(text, BTENCRY_TASK_OWNED)
    return leftovers(text, 'btencry_task.c', {'cpu_reset', 'cpu_in_irq', 'cpu_irq_disabled'})


def task_dispatch(text):
    text = own_all(text, TASK_DISPATCH_OWNED)
    # cpu.h's inline probes and cpu_reset (the SDK's ASSERT) are all that is
    # left; task_queue.c has the probes.
    return leftovers(text, 'btctrler_task.c', {'cpu_reset', 'cpu_in_irq', 'cpu_irq_disabled'})


CONTROL_TX_OWNED = {
    'lmp_format_packet': '59caad9b99e355813e32f6448f6b4efd291b2775c83950382099740c906f5fcd',
    'lmp_request': '672931262a86f1323ceded46178da2dd5830b4a0fa2ecd4f7857d4184ee4c4e1',
    'lmp_response': 'c666b949c5d2a762cbb4609ab1b6d91cd416848d0f0b636a1cd38d65abb79b8d',
    # lmp_acl_tx.c
    'lmp_private_get_tx_remain_buffer': '6af15c8ef6405097514e84320eddbb1950c6b6bdd79ced80dce2827312e7fdb9',
    'lmp_private_get_tx_packet_buffer': 'fa40cbc638eae4c24280a30f41bf8f66fcde85f107df854905581d8ce8f5ec32',
    'lmp_hci_send_packet': 'dced742298cafb009b8d7a1a77345ebf8362fb74c2dc12d42ca87e6309657b13',
    'lmp_hci_send_packet_standard': '1e5228f42c35684a62ba5b9c2f1bf417f6fef2211cabf2da8d0ff84e930eeed3',
    'lmp_hci_private_try_free_acl_packet': 'c3f0b20c568ea88dd01f876571aad1b9c770a35c31b23138ea02553f234fe9ff',
    'lmp_hci_private_hold_acl_packet': '36a3039b6541fb720e1681a65df6c8934171b9ded1dfc14f1b1fda00836173af',
    'lmp_hci_private_free_acl_packet': '675d33928e1b25e642370f53dd7c13781928ac6a47f802c3dadb0038e746f64b',
}


def control_tx(text):
    return leftovers(own_all(text, CONTROL_TX_OWNED), 'lmp_rxtx.c', {'cpu_reset'})


# hci_ll_5_cmds.c: Bluetooth 5 extended advertising, scan and PHY commands.
HCI_LL_5_CMDS_OWNED = {
    'le_controller_handler_for_ble5': 'ea079993d43b47d8c068cb608f76a6fc2a9cbf224cc4136140f709efff2fff9b',
    'list_add_tail': '5adc6457019bf108d035d9232ee14c1e490c73b8aae0d345e886f4483de55525',
    'alloc_periodic_sync': 'be98cee67c85b258e61acb58fcb42ae76233bb452dbb52cf87aabea8c08efbe3',
    'get_if_phy_support': '87fb1a7c3b123c458948a9566af7582fe12ab36f4811b46139c4c98e9d627194',
}


def hci_ll_5_cmds(text):
    return leftovers(own_all(text, HCI_LL_5_CMDS_OWNED), 'hci_ll_5_cmds.c', set())


# tws_media_sync.c: straggler member of the SDK's TWS code; only what other code
# still references is owned (the rest is unreferenced and goes with it).
TWS_MEDIA_SYNC_OWNED = {
    'get_bt_slot_time': '7ff75b9059f72720e3a7bedbde3637038efaf108fd2dffec0e735c4b897b3004',
    'read_slot_info': '68aee508efc050bcd7f65df3f9f56d69223cb3dcd6a6f10a55ac6a6df3b841ce',
}
TWS_MEDIA_SYNC_LEFTOVERS = {
    'dac_rd_flag', 'bt_audio_sync_nettime_select', 'bt_tws_master_slot_clk',
    'test_dac', 'read_local_audio_us_time', 'get_remote_clk_bitoff',
    'bt_send_audio_sync_data', 'bt_media_sync_set_handler', 'bt_media_sync_open',
    'list_add', 'bt_media_sync_master', 'bt_media_device_online',
    'bt_media_sync_close', 'list_del', 'sbc_sync_packet_len',
    'tws_media_clear_msg_pool', 'tws_media_sync_tx_msg', 'tws_media_sync_rx_msg_and_tx_msg',
    'tws_media_sync_rx_msg', 'tws_media_sync_event_handler', 'list_empty',
    'tws_media_sync_tx_msg_suss', 'tws_media_other_offline',
}


def tws_media_sync(text):
    return leftovers(own_all(text, TWS_MEDIA_SYNC_OWNED), 'tws_media_sync.c', TWS_MEDIA_SYNC_LEFTOVERS)


# local_media_sync.c: straggler member of the SDK's TWS code; only what other code
# still references is owned (the rest is unreferenced and goes with it).
LOCAL_MEDIA_SYNC_OWNED = {
    'tws_api_local_media_sync_rx_handler_notify': 'd3a93f1be8b0de249628192ad9a63b16af0f5e9d613c8b9d6df3e0f0dd1ca4b3',
    'tws_api_local_media_packet_cnt': '3868761e7bd02bf3e1c4b070b72a0f2a8924a40e77f5052b9b8368ecd8f59017',
    'tws_api_local_media_trans_get_total_buffer_size': '990fceb98e63f84072683ab04077c9c5e02096f50a8eae66bae6c007fd43e348',
    'tws_local_media_sync_rx_bulk_alloc': '7a8c99794f005c7a7d4641bee4c6f5cd6e78a9b60ac37161c912e5999674ee80',
    'tws_local_media_sync_rx_handler_in_irq': '8fd3ce7aa96dd3f244704f0a7133b9d6765d79a4ee3841cdcb0ad7e171fa16e1',
    'list_add_tail': 'c961bff247246c7b0fd9dba04dbf1b06819359514f6f27010c826350ad78e91e',
    'tws_local_media_sync_resume': 'a9fb7c5e7372f3aac08871a893152f8454268a2a2852b156b0bc23004278ba2b',
}
LOCAL_MEDIA_SYNC_LEFTOVERS = {
    'tws_local_limit_send_num', 'tws_local_media_event_handler', 'tws_local_media_req_and_res',
    'tws_local_media_req', 'tws_local_media_res', 'tws_api_local_media_trans_set_buf',
    'tws_api_local_media_trans_alloc', 'tws_local_media_buffer_free', 'tws_api_local_media_trans_free',
    'tws_api_local_media_trans_push', 'tws_api_local_media_trans_check_total', 'tws_api_local_media_trans_pop',
    '__list_del_entry', 'tws_api_local_media_trans_fetch', 'tws_api_local_media_trans_start',
    'tws_api_local_media_trans_stop', 'tws_api_local_media_trans_packet_del', 'tws_api_local_media_trans_check_ready_total',
    'tws_api_local_media_trans_clear', 'tws_api_local_media_trans_clear_no_ready', 'tws_api_local_media_set_limit_size',
    'tws_api_local_media_trans_check', 'tws_local_media_req_suss', 'tws_api_first_media_frame',
}


def local_media_sync(text):
    return leftovers(own_all(text, LOCAL_MEDIA_SYNC_OWNED), 'local_media_sync.c', LOCAL_MEDIA_SYNC_LEFTOVERS)


# tws_ll_adv.c: straggler member of the SDK's TWS code; only what other code
# still references is owned (the rest is unreferenced and goes with it).
TWS_LL_ADV_OWNED = {
    'tws_ll_adv_set_params': '808a30df09498d7c25e1aa5e7f614c72a2bca2def81b8d3b88ddf6e438476914',
    'tws_ll_adv_set_scan_rsp_data': 'e64e8aaf2a43039bf7550502a526637f89dd57be034ee520bc3ce271d407eb95',
    'tws_ll_adv_open': 'c3cf47f4356de268a365df90ec1ab4a70c7abf96d7da2f367f6cd31b9cc86f38',
    'tws_ll_adv_close': 'f0cf3be24d8cfe3724a3a7d0abe4a439c722ca50807389f540f8f7b02146b3e2',
    'll_adv_init': 'db477c92600f6b6b818763ba51558010d0ef97df81a0ed86cffbb6714076afe9',
    'adv_rx_probe_handler': '6502818ec961fbedb8c954a4b94d0a64e0fa9cce320b0e4b19bc52618afd8e90',
}
TWS_LL_ADV_LEFTOVERS = {
    'tws_dump', 'tws_ll_adv_link_get',
}


def tws_ll_adv(text):
    return leftovers(own_all(text, TWS_LL_ADV_OWNED), 'tws_ll_adv.c', TWS_LL_ADV_LEFTOVERS)


# tws_data_forwarding.c: straggler member of the SDK's TWS code; only what other code
# still references is owned (the rest is unreferenced and goes with it).
TWS_DATA_FORWARDING_OWNED = {
    'tws_data_sync_clear_abandon_num': '071aaf997e0412a71a0435a165414fd7d5bc1305dc0f27f8c8b66e9c2bf3f9c1',
    'tws_data_sync_resume': '514aa8ab26d331b59ec0929c78c64f70cd19b1f81800529b55e2939066b07615',
    'tws_data_sync_rx_bulk_alloc': '1b6abc33511d13913525207986f72e95d0a25c7ca7418e8a1b07cfb17c9b44fc',
    'tws_data_sync_init': 'f16881b227a3725d4cfd9fd8acfe1872af778986c9877054ac1123f18cd3b5b3',
    'tws_data_sync_channel_open': 'fbdd82ce86e5d65fb88aeb824b2c375a5af1a45b3aa82d74ab67ff3573c6c4b6',
    'tws_data_sync_channel_close': '9e9d13092458b2259e549658cb62fb67294c89124d592ab2c2320182b94235ea',
    'tws_slave_data_abandon': 'cd0a4fd5df437c70558585ba1c8e942593de42bab731e1f541f9c336bcb81d61',
    '__list_del_entry': 'bea6fd8c585e67f75e44639eac798781673072474fa21d053edf33e02a23f764',
    'list_add_tail': '79fdf05d3dabf8781e969787763740058a3aabf87fedbe7fca725226e0d14cac',
    'list_empty': 'a80c163cba6fbdaf00e6bdb966af06beb9837bcbbca328adb7caf1da75cca213',
}
TWS_DATA_FORWARDING_LEFTOVERS = {
    'tws_data_sync_event_handler', 'tws_master_tx_bulk_suss', 'tws_master_sync_data_to_remote',
    'cpu_reset', 'tws_data_sync_get_data', 'tws_data_sync_tx_data_list',
    'tws_data_sync_rx_data_list', 'tws_data_sync_channel_state', '__slave_abandon_bulk_check',
}


def tws_data_forwarding(text):
    return leftovers(own_all(text, TWS_DATA_FORWARDING_OWNED), 'tws_data_forwarding.c', TWS_DATA_FORWARDING_LEFTOVERS)


# link_bulk.c: the Classic packet buffers (link_bulk.c).  The heap-pool
# option and its unused helpers go with the member.
LINK_BULK_OWNED = {
    'bredr_rx_bulk_empty': '9806c77e0a7597bb656eaa78adfb5465731e81c54b303698fb96aac8daccb8f9',
    'bredr_rx_bulk_state': 'adaa383157207c2b757e0b17820d64b4e544d8a2320110ab665db914f4f024a1',
    'bredr_rx_bulk_total_size': 'cdc054c51b2ef870d0851c6a9ad13059a14991edc9d0abcd2130983206ae4133',
    'bredr_rx_bulk_remain_size': '440a8ab13a891cd563dd51e4b2b8012066f49e295a0107c7e4b4ae0528c8e6ed',
    'bredr_rx_bulk_set_max_used_persent': 'ba3023c4fa03ac8072d3cfd32c69520ae156e065818ca8752f974e8eec5d665f',
    'lmp_malloc': '9d190b579a33186eb4f2422f31f80e5f66abae0c3bd6a7544a74a098117b451e',
    'bt_malloc': '36b53984adb1383074f77bbf9906253619e354aacd5e2f9ce475a01395256e97',
    'lmp_free': '61640666584acf2980ac5e0a999958f1b72a037b47c8de300264cd5d196b23fe',
    '__bredr_rx_bulk_alloc': '639b4e7c0ab36345acfcc2c8090bbdcfde5069e3a4765870119d6e82a9bfeb18',
    'bredr_rx_bulk_alloc': '23f6b2e40efc7cfce0eee1821cb92733d834c9dd5e59098e5e128ed96249fd3f',
    'bredr_rx_bulk_alloc_for': '8fb3c55fe55c39825696af52f35a7bd3edaf4063d3a39befad1ccfc7481ab009',
    'bredr_rx_bulk_push': '8fc227ea6351073fdbc8568c649397a5043f7982164f6665a8b9095af9d6d823',
    'bredr_rx_bulk_pop': '7c6ec3079db7743b18d40c73797a4f84170f7e819fbf3a80bf959b1d0d4756ca',
    'bredr_rx_bulk_free': 'c886d84ac6eae2f24a74887ce17a328c3bd77f8fa266e12700f2162c8afb3929',
    'bt_free': '6406fc99d96faeb1ff6f33da82bc4f6ccda780490d684353e6d36983dd8b543f',
    'bredr_tx_bulk_empty': '547d3b020875b2f666de3decf054a0e6196b63e6c17de9dee03c39edf591675b',
    'get_bredr_tx_remain_size': 'a9e92dddb073bd119418d7b2f3f6cfd52b50a845355df1e6b2486c8470fb12db',
    'bredr_tx_bulk_alloc': 'cc852e302aefd54b89a93f55f867a797dc82023c8c9798510b525969595f26e5',
    'bredr_tx_bulk_realloc': '5d286a815b971746cd134bf4a000e1942a98983eacc4592e93e367dd917cbf4c',
    'bredr_tx_bulk_push': 'c38ab923cff7930aefc3e2041d130e4393a24924aad111552e1bda3225c46264',
    'bredr_tx_bulk_pop': 'dc207d4867a50c1808bf2abce5281153dd7a4ff6472c8d6b075ac771f534facd',
    'bredr_tx_bulk_free': '0e28a332ae728de44ace5b51431bb22cc58ef9c57b4ac2c2dba4835426f4fa4f',
    'bredr_goto_background': '40beb56f9454d4e04d448b9e7a37001c8bd1b06beb6b970f3ba3dde22148f1da',
    'bredr_goback_from_background': 'a884096c387f2d5a1728a30574d518a4f406aae603f32a7cbb4836eb6a8d2e05',
    'bredr_rx_bulk_suspend': 'b5a563b1785680977b79d74772fb80c81906a4db183dca47bbc5731a561c86cd',
    'bredr_rx_bulk_resume': 'ebfbd1b6223e01d49e3ddc842db50728e07a83a58ea994d8c8fe094aa1fb7cfb',
    'link_bulk_init': 'd44dfc6d9fad5abf97e83e178ab94ca903521c112355b81bbdf2fe04fb28ecfd',
    'link_change_bulk_init': '75eab3218d22e836a0afa069714e5dc623d06274d137588ab310cbff5fb2709b',
    'bredr_rx_remain_bulk_empty': '0c2fb7be6cbc3a9da68ff274baf38883004c04ee3769674a1589e378fdbf3324',
    'link_change_rx_bulk_size': 'c81d9cede84b0efe75e413e93bdbde17763f30b235749d975c68bbe52ec9d36b',
}


def link_bulk(text):
    return leftovers(own_all(text, LINK_BULK_OWNED), 'link_bulk.c', {
        'bredr_bulk_malloc_enable', 'bredr_rx_bulk_resume_wait', 'bredr_rx_bulk_used_reset',
        'bredr_rx_bulk_alloc_reserve', 'bredr_rx_bulk_inc_ref', 'bredr_rx_bulk_get_suspend',
        'zalloc_mux_rx_bulk', 'free_mux_rx_bulk', 'cpu_reset', 'cpu_in_irq', 'list_add_tail',
        '__list_del_entry'})


# lbuf.c: the controller's block allocator and queues (lbuf.c).
LBUF_OWNED = {
    'lbuf_init_btctrler': '680abdac8fb32648dcf93afed6a8608b62885f80f1f28d9114a9cdbc30369e9f',
    'lbuf_alloc_btctrler': 'a1a2a7ff05a96fd857a32d2eff62fe1954a7e833dc0c9093e82a2004a443b7f6',
    'lbuf_realloc_btctrler': '7d4850e7ef864dc76a47e6b998baa0a8394585f5650b54df0e0f91f279d63331',
    'lbuf_free_btctrler': '1fa352934800cf6b50106a43589794ae1d430e8669c92a9f3c24a6246f7a92f4',
    'lbuf_empty_btctrler': '6537ca5336dfc842c7c0b05a19c3259f9536e116f5358824e87ea37ade5dd947',
    'lbuf_clear_btctrler': '3712044c3419f2431b7b9f46327cabea8c50f3f1f29be544fbf96fbaa3f5952d',
    'lbuf_push_btctrler': '0ea202779cb6e8590eb1aa99b06e25b98baf15a80e066e7826936aab88395090',
    'lbuf_pop_btctrler': 'e085182890c7e464d917ed5be3d12b95c0a5971472cafbf053fe087216532acc',
    'lbuf_debug_btctrler': '0b6ad5eb34a042402886b6f312abb08badebc1bd871d9b2ba3e3ddeedc3ea0db',
    'lbuf_remain_len_btctrler': 'b588da4fa4eb83b9c4c426bd527c4c857b0d79e68616314776b59d7bfdd5ec17',
    'lbuf_peek_btctrler': 'af6a4933791e57aa60559cd078f44f61db3f2e73b741841b1af5240031c36b7b',
    'lbuf_push_queue_btctrler': '309ffd240c70f78017219955810676aa6d7c6c13b6719211584a2c03922021fa',
    'lbuf_pop_queue_btctrler': 'edafae53e85aec0e879265fe35eb1025b306b94c36da4b90916dc8b00493f686',
    'lbuf_peek_queue_btctrler': '0b34a056ac1c27e559b7e639c2d31b8278ba02b1220b21e323eff9480fcf3e23',
    'lbuf_clear_queue_btctrler': '3b28c57113e10dc8febb5bf0f88cc6d365cab888b7ef5064c80bc5d7123ec40f',
}


def lbuf(text):
    return leftovers(own_all(text, LBUF_OWNED), 'lbuf.c', {
        'lbuf_get_chunk_num_btctrler', 'lbuf_more_data_btctrler', 'lbuf_query_used_btctrler',
        'lbuf_info_btctrler', 'cpu_reset', 'list_add_tail', '__list_del_entry', 'list_del'})


# sys_timer.c: controller timer adapter.
SYS_TIMER_OWNED = {
    'sys_timer_get_time': '20808f724594292e96cb131a98de49405525d7159cd563b362be1e04f81e742d',
    'sys_timer_register': '13eba4ce33b62ae700d623adda5a2b179c54b1b7e7aa4d162be31969d51a1cb7',
    'sys_timer_register_periodic': '4340f19b2d45d858dbf5cf1c56917745bbd161f0fd1659b301169c3b524a220e',
    'sys_timer_remove': 'ab3139909a46c37e180b62d133ffa7f7c30591727c8b2bc4c96f1f92987247ca',
    'sys_timer_reset': '3685dc118683d04c607644ff3eae94561e1e85f223e1951774f022010b3d0dc7',
    'sys_timer_change_period': 'a7b46312714459b0b811ac7729fe2045c128acbcc7887865646dc04501abc81c',
    'sys_timer_set_context': 'acb2f9df0227e6c3bfda9e91886217690b9f7279befe7511c3e2fcc7cd3bcd19',
    'sys_timer_get_context': '63feea11f8dad767fb274bd2cfc32e9ce0f6969e1dfe4ba2b276b86095a85036',
}


def sys_timer(text):
    return leftovers(own_all(text, SYS_TIMER_OWNED), 'sys_timer.c', set())

# btcontroller_heap.c: system-heap NV wrappers. Unused Classic functions
# are checked leftovers.
BTCONTROLLER_HEAP_OWNED = {
    'btctler_nv_memory_apply': 'a4dedfa3a4f5647e5ad70a36d19a412341c2e501f805c3203da78b02722b7b26',
    'btctler_nv_memory_malloc': 'e4662c3cdc654f9b14bd506a37205bc1529b7044296e8dea0ed4c105ef9ff45b',
    'btctler_nv_memory_free': '4b5e3b14adfd1a59289ac7b8acd51011b2c0cf694eb8d8eda73242d45c4e3545',
    '__bt_malloc': '5a77b0f3fe771def7d880d98293089bb7ab517bf76e5eb7a13b13b9f78919022',
    '__bt_free': '1293d9671a1d35b27db3ce63bf14c304a4aaf14b7becdc1ed5d977be4b367fca',
}


def btcontroller_heap(text):
    return leftovers(own_all(text, BTCONTROLLER_HEAP_OWNED), 'btcontroller_heap.c', {
        'bt_classic_malloc', 'cpu_reset', 'bt_classic_free',
        'btctler_memory_apply', 'btctler_memory_release',
        'btctler_nv_memory_release', 'btctler_nv_memory_debug',
    })


THREAD_OWNED = {
    'thread_owner_init': 'e8eb5dd0bb89c0696a1657f8940840920a897763e6323e00c169576b9e0da388',
    'thread_create': '6d12b829d41e03ca0a9197cdde446821eb351d34ea6b6537a35b61f12dc39e50',
    'thread_schedule': '782e024965fedaefb9b17247f98dd3666024f41c45fc72708dad9d9d837c292a',
    'thread_suspend': 'b3d1fa825af725fb8c17c5c9b18e086a7d4a88efbc36a3fa87aeed9d2d410753',
    'thread_resume': 'd531539940c33ca17f49839b60355fb04aa51ffbe82a506db364e060d9c076be',
    'thread_delete': '2e593ccaa2183e2a1ed234c2bc626596f36d580dee3a1aa60dbd011e1a22a679',
    'thread_run': 'de73b7faec0def87e4257c7b06229a6aff78a56b63a5ac05d373a3180d3776a3',
}

def thread(text):
    return leftovers(own_all(text, THREAD_OWNED), 'thread.c', {
        'thread_msg_alloc', 'thread_msg_free', 'thread_msg_send',
        'thread_msg_recv', 'thread_msg_peek'})


# vsprintf.c: the binary packet serializer and its length counter.
VSPRINTF_OWNED = {
    '__vsprintf_len': '486015ba0a5a4a7cb337fe80fa42c723e0549798bd77edf19de5e62de5c2ec5b',
    '__vsprintf': 'df3d5a14fda1f17f703d91aaa0b6b4a56495a0168e008cefb94135d8c7535d72',
}


def vsprintf(text):
    return leftovers(own_all(text, VSPRINTF_OWNED), 'vsprintf.c', set())


# vendor_manager.c: the vendor HCI command group (OGF 0x3f).
VENDOR_MANAGER_OWNED = {
    'user_bredr_fcc': '420469d3ca529c5a031b9de8bf9cac8e6ba18da34578b080a4ec31df71e7d928',
    'hci_vendor_cmd_handler': '2ca8c2115e204b3a4e186acc00147930313000e0db243381e0f2719173cdaec3',
    'vendor_handler': '58f327179296929657226c9e3fbe28199321f0aa5bb6b8cb46da656648a512c9',
}


def vendor_manager(text):
    return leftovers(own_all(text, VENDOR_MANAGER_OWNED), 'vendor_manager.c', {'vendor_manager_init'})


# aes128.c: the AES-128 entry point; the SDK self tests and cpu_reset have no caller.
AES128_OWNED = {
    'aes128_start': '362bcce8d10899eec5a7a34fd90f7b1721ed1a00e5e4ab02a7cdbd8c20e3152c',
}


def aes128(text):
    return leftovers(own_all(text, AES128_OWNED), 'aes128.c', {'aes128_test', 'aes_ccm_test', 'cpu_reset'})


# aes_hw.c: the AES hardware driver.
AES_HW_OWNED = {
    'aes128_start_enc': 'b2633aea31c8887bd9536ccc4cd9b3e50e023a17f632807fb336b993662b8700',
    'ccm_memory_v2': 'ed0ec5f4ac7e4ea19f9e9942c0332dbcf58cf6353b349fdab266ec13aa0d2ac6',
}


def aes_hw(text):
    return leftovers(own_all(text, AES_HW_OWNED), 'aes_hw.c', {'aes_init', 'aes_irq_handler', 'aes_hw_test'})
