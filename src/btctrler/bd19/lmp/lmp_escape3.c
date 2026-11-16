/* Classic/lmp_escape3.c (included by lmp.c): Jieli vendor escape-3 PDUs:
 * test box, OTA update channel, frequency-offset trim, ex-info report. */
#undef LMP_FILE
#define LMP_FILE "lmp_escape3.c"

void free(void *p);
void *malloc(unsigned long size);
u16 get_chip_id();
int clk_get_osc_cap();
char *sdk_version_info_get(void);
int norflash_ioctl(void *dev, int cmd, int arg);
int user_send_cmd_prepare(int cmd, u16 len, u8 *param);
void doe(u16 key, void *buf, u32 len, u32 addr);
u16 CRC16(const void *ptr, u32 len);
typedef u16 unaligned_u16 __attribute__((aligned(1), may_alias));
typedef u32 unaligned_u32 __attribute__((aligned(1), may_alias));

void LMP_CODE bt_updata_set_flag(u8 bit)
{
    if (!(config_update_mode & 1))
        return;
    bt_res_updata_flag |= 1 << bit;
    if (lmp_ch_update_resume_hdl)
        lmp_ch_update_resume_hdl(NULL);
}

void LMP_CODE bt_updata_clr_flag(u8 bit)
{
    if (config_update_mode & 1)
        bt_res_updata_flag &= ~(1 << bit);
}

u32 LMP_CODE bt_updata_get_flag(void)
{
    if (config_update_mode & 1)
        return bt_res_updata_flag;
    return 0;
}

/* lmp_escape3.c ~74 */
INLINE void testbox_set_key_action_test_flag(u8 flag)
{
    key_action_test_flag = flag;
}

u8 LMP_CODE testbox_get_key_action_test_flag(void *priv)
{
    return key_action_test_flag;
}

void LMP_CODE reg_revic_buf_addr(void *addr)
{
    uboot_revic_handle = addr;
}

int LMP_CODE lmp_escape3_acl_u_s_send(struct lmp_connection *conn, u8 opcode, u8 *buf, u16 len)
{
    struct bredr_tx_bulk *tx = bredr_tx_bulk_alloc(len + 4);

    if (tx == NULL) {
        LOG_E("alloc escp3 acl err\n");
        return -12;
    }
    tx->offset = 0;
    tx->llid = 2;
    tx->len = len + 4;
    tx->data[0] = 0;
    tx->data[1] = opcode;
    tx->data[2] = len;          /* sic: data[3] is left as allocated */
    memcpy(&tx->data[4], buf, len);
    link_conn_tx_bulk_avaiable(conn->link, tx);
    return 0;
}

void LMP_CODE uboot_rx_handler(int handle, struct bredr_rx_bulk *rx)
{
    u8 f;

    if (!(config_update_mode & 1))
        return;
    rx->payload_head[0] = handle;
    switch (rx->llid) {
    case 1:
        f = 0x10;
        goto set;
    case 2:
        f = 0x20;
set:
        rx->payload_head[1] = f | handle >> 8;
        break;
    }
    rx->payload_head[2] = rx->len;
    rx->payload_head[3] = rx->len >> 8;
    if (uboot_revic_handle)
        uboot_revic_handle(&rx->payload_head[4], rx->len);
    bt_updata_set_flag(1);
}

void LMP_CODE bt_updata_tx_detch(void)
{
    if (!(config_update_mode & 1))
        return;
    if (update_conn == NULL) {
        LOG_E("err:bt_updata_tx_detch null \n");
        return;
    }
    lmp_tx_detch(update_conn, 0x13);
}

void LMP_CODE bt_update_link_close(void)
{
    if (!(config_update_mode & 1))
        return;
    if (update_conn == NULL) {
        LOG_E("err:bt_updata_tx_detch null \n");
        return;
    }
    link_conn_close(update_conn->link);
}

/* lmp_escape3.c ~176 */
INLINE void lmp_rx_jlbt_id(struct lmp_connection *conn, u8 *data)
{
    struct link_connection *link;

    update_conn = conn;
    if (lmp_get_conn_num() > 1) {
        puts("resopnse test not accepted");
        lmp_response(conn, "b05", 0xfc, 2, 126, 100, 9);
        return;
    }
    lmp_conn_for_address(NULL);
    lmp_response(conn, "b04", 0xfc, 1, 126, 100);
    link = conn->link;
    link->test_con_flag = 1;
    link_conn_set_max_slot(link, 5);
    testbox_set_key_action_test_flag(0);
    hci_send_event(0xfe, "11", data[0], 1);
}

/* lmp_escape3.c ~206 */
INLINE void lmp_rx_jl_chip_req(struct lmp_connection *conn)
{
    u8 flag = get_bt_osc_offset_flag() ? 0xff : 0;

    LOG_I("jl_chip_req:%x %x %s %x\n", flag, flag, test_name, get_jl_chip_id());
    lmp_response(conn, "1111C0101", 0xfc, 107, flag, flag, test_name, get_jl_chip_id());
}

/* lmp_escape3.c ~219 */
INLINE void lmp_rx_custom_test(struct lmp_connection *conn, u8 *data)
{
    if (data[0] == 0 && conn->cap_adjust) {
        conn->cap_adjust = 0;
        hci_send_event(0xfb, "2", clk_get_osc_cap());
    }
    if (data[0]) {
        fre_offset_trim_flag = 0;
    } else if (fre_offset_trim_flag) {
        LOG_I(">>> bt_osc_offset_save\n");
        bt_osc_offset_save();
        hci_send_event(0xf6, "2", bt_fre_offset_get());
    }
    LOG_I(">>>CUSTOM_TEST:%x\n", data[0]);
    lmp_response(conn, "b04", 0xfc, 1, 126, 3);
}

/* lmp_escape3.c ~241 */
INLINE void lmp_rx_test_osc_int(struct lmp_connection *conn, u8 *data)
{
    LOG_I("cap %x %x \n", data[0], data[1]);
    conn->cap_adjust = 1;
    lmp_response(conn, "b04", 0xfc, 1, 126, 120);
}

/* lmp_escape3.c ~251 */
INLINE void lmp_rx_fast_test_req(struct lmp_connection *conn, u8 *data)
{
    lmp_response(conn, "b04", 0xfc, 1, 126, 101);
    hci_send_event(0xfc, "11", data[0], 1);
}

__attribute__((weak)) u16 LMP_CODE bt_get_chip_key_crc(void)
{
    u16 id = get_chip_id();

    return CRC16(&id, 2);
}

__attribute__((weak)) u8 *LMP_CODE bt_get_flash_id(void)
{
    static u32 ex_info_flash_id LMP_BSS;

    norflash_ioctl(NULL, 100, (int)&ex_info_flash_id);
    LOG_I("ex_info_flash_id:%x\n", ex_info_flash_id);
    return (u8 *)&ex_info_flash_id;
}

__attribute__((weak)) u8 *LMP_CODE bt_get_sdk_ver_info(u8 *len)
{
    char *ver = sdk_version_info_get();

    if (len)
        *len = strlen(ver);
    LOG_I("sdk_ver:%s %x\n", ver, *len);       /* sic: dereferences len even if NULL */
    return (u8 *)ver;
}

void LMP_CODE bt_testbox_ex_info_get_handle_register(u8 info_type, void *handle)
{
    switch (info_type) {
    case 0:
        get_vbat_level_handle = handle;
        break;
    case 1:
        get_vbat_percent_handle = handle;
        break;
    case 2:
        sdfile_get_burn_code_handle = handle;
        break;
    case 32:
        get_flash_id_handle = handle;
        break;
    case 33:
        get_chip_key_crc_handle = handle;
        break;
    case 3:
        get_sdk_ver_info_handle = handle;
        break;
    default:
        LOG_E("info type:%x err\n", info_type);
        break;
    }
}

__attribute__((weak)) u16 LMP_CODE ex_info_total_len_get(u32 mask)
{
    u16 len = 0;
    int i;

    for (i = 0; i < 5; i++) {
        if ((1 << ex_info_type_match_len_tab[i][0]) & mask)
            len += ex_info_type_match_len_tab[i][1];
    }
    return len;
}

__attribute__((weak)) u16 LMP_CODE bt_testbox_ex_info_fill_buf(u32 mask, u8 *buf, u16 len)
{
    u8 *p = buf, *s;
    u8 vlen;

    if ((mask & 1) && get_vbat_level_handle && get_vbat_percent_handle) {
        p[0] = 4;
        p[1] = 0;
        *(unaligned_u16 *)(p + 2) = get_vbat_level_handle();
        p[4] = get_vbat_percent_handle();
        p += 5;
    }
    if ((mask & 2) && sdfile_get_burn_code_handle) {
        p[0] = 17;
        p[1] = 1;
        memcpy(p + 2, sdfile_get_burn_code_handle(NULL), 16);
        p += 18;
    }
    if ((mask & 4) && get_flash_id_handle) {
        p[0] = 5;
        p[1] = 2;
        *(unaligned_u32 *)(p + 2) = *(unaligned_u32 *)get_flash_id_handle();
        p += 6;
    }
    if ((mask & 8) && get_sdk_ver_info_handle) {
        s = get_sdk_ver_info_handle(&vlen);
        vlen = vlen < 32 ? vlen : 32;
        p[0] = vlen + 1;
        p[1] = 3;
        memcpy(p + 2, s, vlen);
        p += vlen + 2;
    }
    if ((mask & 16) && get_chip_key_crc_handle) {
        p[0] = 3;
        p[1] = 4;
        *(unaligned_u16 *)(p + 2) = get_chip_key_crc_handle();
        p += 4;
    }
    return p - buf;
}

/* lmp_escape3.c ~480: encrypt the report in 32-byte chunks. */
INLINE void lmp_ex_info_encrypt(u8 *buf, u16 len)
{
    u32 off = 0;
    u16 chunk;

    while (len) {
        chunk = len < 32 ? len : 32;
        doe(~(u16)(off >> 2), buf, chunk, 0);
        buf += chunk;
        off += chunk;
        len -= chunk;
    }
}

/* lmp_escape3.c ~497 */
INLINE void lmp_rx_ex_info_req(struct lmp_connection *conn, u8 *data)
{
    u32 mask = data[1] << 8 | data[0] | data[2] << 16 | data[3] << 24;
    u16 len = ex_info_total_len_get(mask), n;
    u8 *buf = malloc(len + 4);

    if (buf == NULL) {
        lmp_response(conn, "b04", 0xfc, 2, 126, 130);
        return;
    }
    *(u32 *)buf = mask;
    n = bt_testbox_ex_info_fill_buf(mask, buf + 4, len);
    lmp_ex_info_encrypt(buf, len + 4);
    lmp_escape3_acl_u_s_send(conn, 0x83, buf, n + 4);
    free(buf);
}

void LMP_CODE lmp_send_test_key(struct lmp_connection *conn, u8 key)
{
    test_info_upload_t info;

    if (conn == NULL) {
        puts("conn err\n");
        return;
    }
    printf("key:%x\n", key);
    if ((key & 0xfc) == 0xf0) {
        info = (test_info_upload_t){ 0, 2, 1 };
        info.data[0] = key + 16;
        lmp_request(conn, "11LB", 0xfc, 0x87, 4, &info);
        put_buf(&info, 4);
    } else if ((u8)(key + 12) < 5) {
        test_info_upload_t info2 = { 1, 2, 1 };

        info2.data[0] = key + 13;
        lmp_request(conn, "11LB", 0xfc, 0x87, 4, &info2);
    } else {
        lmp_request(conn, "111", 0xfc, 0x67, key);
    }
}

void LMP_CODE bt_updata_control(u8 btupdata_sw, u16 len, u8 seek_type, u32 seek_offsize)
{
    if (update_conn == NULL) {
        LOG_E("err:bt_updata_control_handle_null\n");
        return;
    }
    lmp_request(update_conn, "111214", 0xfc, 0x66, btupdata_sw, len, seek_type, seek_offsize);
}

/* lmp_escape3.c ~615 */
INLINE void lmp_rx_bt_updata_req(u8 *data)
{
    hci_send_event(0xfd, "11", data[0], 1);
    btctrler_hci_cmd_to_task(0x400009, 0);
}

void LMP_CODE lmp_update_init(void)
{
    if (!(config_update_mode & 1))
        return;
    if (update_conn == NULL) {
        LOG_E("lmp update init conn is null\n");
        return;
    }
    g_lmp_update_start_flag = 1;
    lmp_update_rx_handler_register(uboot_rx_handler);
}

void LMP_CODE lmp_update_exit(void)
{
    if (!(config_update_mode & 1))
        return;
    if (update_conn == NULL) {
        LOG_E("lmp update exit conn is null\n");
        return;
    }
    g_lmp_update_start_flag = 0;
    lmp_update_rx_handler_register(NULL);
}

void LMP_CODE lmp_escape3_fre_offset(struct lmp_connection *conn, u8 *data)
{
    s32 offset = data[2] << 16 | data[3] << 24 | data[1] << 8 | data[0];

    if (get_bt_osc_offset_flag()) {
        LOG_E(">>>>>>>>>>>>>bt_osc_offset_flag ok<<<<<\n");
        lmp_response(conn, "b05", 0xfc, 2, 126, 99, 43);
        return;
    }
    fre_offset_trim_flag = 1;
    bt_osc_offset_set(offset);
    link_conn_pll_comp_reset(conn->link);
    lmp_response(conn, "b04", 0xfc, 1, 126, 99);
}

void LMP_CODE lmp_escape3_get_remote_addr_res(struct lmp_connection *conn, u8 *data)
{
    lmp_response(conn, "11c06", 0xfc, 0x85, gbredr_local_dev.internal_addr_local);
}

void LMP_CODE lmp_escape3_get_remote_addr(struct lmp_connection *conn, u8 *addr)
{
    tws_api_set_sibling_addr(addr);
    memcpy(conn->core_s.remote_addr, addr, 6);
    user_send_cmd_prepare(1, 6, addr);
}

void LMP_CODE lmp_set_features_req_step(u8 *addr)
{
    struct lmp_connection *conn = lmp_conn_for_address(addr);

    if (config_asser) {
        if (!conn) {
            printf("file:%s, line:%d", SDK_DIR LMP_FILE, 686);
            puts("ASSERT-FAILD: conn != NULL lmp no conn");
            cpu_assert_debug();
        }
    } else if (!conn) {
        cpu_reset();
    }
    conn->core.step = LMP_TX_FEATURES_REQ;
    lmp_tx_features_req(conn);
    conn->core.step = LMP_WAIT_FEATURES_RES;
}

u8 LMP_CODE lmp_get_conn_step(u8 *addr)
{
    struct lmp_connection *conn;

    if (!CONFIG_BTCTLER_FAST_CONNECT_ENABLE)
        return 0;
    conn = lmp_conn_for_address(addr);
    if (conn == NULL)
        return 0;
    return conn->core.step == LMP_WAIT_GET_REMOTE_ADDR_RES;
}

void LMP_CODE lmp_send_user_info(struct lmp_connection *conn, u32 info)
{
    if (conn == NULL) {
        puts("conn err\n");
        return;
    }
    lmp_request(conn, "114", 0xfc, 0x88, info);
}

__attribute__((weak)) void LMP_CODE bt_get_uesr_info(u32 info)
{
}

/* lmp_escape3.c ~726 */
INLINE void lmp_rx_user_info(u8 *data)
{
    bt_get_uesr_info(data[2] << 16 | data[3] << 24 | data[1] << 8 | data[0]);
}

/* lmp_escape3.c ~734 */
INLINE void lmp_rx_e3_accepted(u8 op)
{
    if (op == 102 && (config_update_mode & 1))
        bt_updata_set_flag(1);
}

void LMP_CODE lmp_escape3_command_process(struct lmp_connection *conn, u8 opcode, u8 *opcode1, u8 *data)
{
    switch (opcode) {
    case 1:
        LOG_I("E3_LMP_ACCEPTED_EXT");
        *opcode1 = data[1];
        lmp_rx_e3_accepted(data[1]);
        break;
    case 2:
        LOG_E("E3_LMP_NOT_ACCEPTED_EXT");
        *opcode1 = data[1];
        break;
    case 100:
        LOG_I("LMP_JLBT_ID \n");
        lmp_rx_jlbt_id(conn, data);
        break;
    case 105:
        LOG_I("LMP_JL_CHIP_REQ_ID \n");
        lmp_rx_jl_chip_req(conn);
        break;
    case 3:
        LOG_I("LMP_CUSTOM_TEST \n");
        lmp_rx_custom_test(conn, data);
        break;
    case 120:
        LOG_I("LMP_TEST_OSC_INT \n");
        lmp_rx_test_osc_int(conn, data);
        break;
    case 101:
        LOG_I("LMP_FAST_TEST_REQ \n");
        lmp_rx_fast_test_req(conn, data);
        break;
    case 0x82:
        LOG_I("LMP_EX_INFO_REQ \n");
        lmp_rx_ex_info_req(conn, data);
        break;
    case 104:
        if (config_update_mode & 1) {
            LOG_I("LMP_BT_UPDATA_REQ \n");
            lmp_rx_bt_updata_req(data);
        }
        break;
    case 99:
        LOG_I("LMP_BT_FRE_OFFSET\n");
        lmp_escape3_fre_offset(conn, data);
        break;
    case 0x84:
        if (CONFIG_BTCTLER_FAST_CONNECT_ENABLE)
            lmp_escape3_get_remote_addr_res(conn, data);
        break;
    case 0x85:
        if (CONFIG_BTCTLER_FAST_CONNECT_ENABLE)
            lmp_escape3_get_remote_addr(conn, data);
        break;
    case 0x86:
        LOG_I("test cfg:%x %x %x\n", data[0], data[1], data[2]);
        lmp_response(conn, "b04", 0xfc, 1, 126, 134);
        if (data[0] == 0) {
            switch (data[1]) {
            case 1:
                LOG_I(">>>KEY DET INIT\n");
                testbox_set_key_action_test_flag(1);
                lmp_tx_sniff_req(conn);
                break;
            case 3:
                LOG_I(">>>KEY DET EXIT\n");
                testbox_set_key_action_test_flag(0);
                lmp_tx_unsniff_req(conn);
                break;
            }
        }
        if (data[0] == 1) {
            switch (data[1]) {
            case 1:
                LOG_I(">>>EAR DET INIT\n");
                lmp_tx_sniff_req(conn);
                break;
            case 3:
                LOG_I(">>EAR DET EXIT\n");
                lmp_tx_unsniff_req(conn);
                break;
            }
        }
        hci_send_event(0xf5, "1LB", 1, data[2] + 3, data);
        break;
    case 0x88:
        lmp_rx_user_info(data);
        break;
    default:
        lmp_response(conn, "b05", 0xfc, 2, 126, opcode, 26);
        LOG_E("unknow_escape3_opcode: %d", opcode);
        break;
    }
}

int LMP_CODE __bt_updata_save_connection_info(void)
{
    struct bd_bt_updata parm;
    struct lmp_connection *conn = update_conn;

    if (conn == NULL) {
        LOG_E("err:bt_updata_control_handle_NULL\n");
        return -1;
    }
    local_irq_disable();
    parm.bt_updata_config = 3;
    __bt_updata_save_link_info(conn->link, &parm);
    local_irq_enable();
    return 0;
}
