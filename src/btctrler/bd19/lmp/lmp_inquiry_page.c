/* Classic/lmp_inquiry_page.c (included by lmp.c): scan enables deferred
 * while a link detaches, and paging for HCI create connection / remote
 * name requests. */
#undef LMP_FILE
#define LMP_FILE "lmp_inquiry_page.c"

static void LMP_CODE link_inquiry_scan_try_timeout_enable(void *priv)
{
    if (check_lmp_detch_over()) {
        inquiry_scan_parm.sys_time_id = 0;
        link_inquiry_scan_enable(inquiry_scan_parm.clk_base);
    } else {
        inquiry_scan_parm.sys_time_id = sys_timeout_add(&inquiry_scan_parm, link_inquiry_scan_try_timeout_enable, 50);
    }
}

INLINE void link_inquiry_scan_try_start_enable(int clk_base)
{
    if (inquiry_scan_parm.sys_time_id)
        sys_timeout_del(inquiry_scan_parm.sys_time_id);
    inquiry_scan_parm.clk_base = clk_base;
    inquiry_scan_parm.sys_time_id = sys_timeout_add(&inquiry_scan_parm, link_inquiry_scan_try_timeout_enable, 10);
}

void LMP_CODE link_inquiry_scan_try_start_disable(void)
{
    if (inquiry_scan_parm.sys_time_id) {
        sys_timeout_del(inquiry_scan_parm.sys_time_id);
        inquiry_scan_parm.sys_time_id = 0;
    }
    link_inquiry_scan_disable();
}

INLINE int __link_page_scan_enable(int clk_base)
{
    return link_page_scan_enable(clk_base);
}

static void LMP_CODE link_page_scan_try_timeout_enable(void *priv)
{
    if (check_lmp_detch_over()) {
        page_scan_parm.sys_time_id = 0;
        __link_page_scan_enable(page_scan_parm.clk_base);
    } else {
        page_scan_parm.sys_time_id = sys_timeout_add(&page_scan_parm, link_page_scan_try_timeout_enable, 50);
    }
}

INLINE void link_page_scan_try_start_enable(int clk_base)
{
    if (page_scan_parm.sys_time_id)
        sys_timeout_del(page_scan_parm.sys_time_id);
    page_scan_parm.clk_base = clk_base;
    page_scan_parm.sys_time_id = sys_timeout_add(&page_scan_parm, link_page_scan_try_timeout_enable, 10);
}

void LMP_CODE link_page_scan_try_start_disable(void)
{
    if (page_scan_parm.sys_time_id) {
        sys_timeout_del(page_scan_parm.sys_time_id);
        page_scan_parm.sys_time_id = 0;
    }
    link_page_scan_disable();
}

static void LMP_CODE page_completed(u8 *addr, int err)
{
    if (page_parm.remote_name_req) {
        if (err == 4 && CONFIG_LMP_NAME_REQ_ENABLE) {
            link_page_disable(0);
            page_parm.remote_name_req = 0;
            if (config_btctler_hci_standard)
                hci_send_event(7, "1AC248", 4, addr, addr);
            else
                hci_send_event(7, "1A", 4, addr);
        }
    } else {
        switch (err) {
        case 0:
            break;
        case 4:
            link_page_disable(0);
            /* fall through */
        default:
            hci_send_event(3, "1HA11", err, 0, addr, 1, 0);
            break;
        }
    }
    if (config_btctler_hci_standard)
        lmp_scan_enable(hci_param.scan_enable);
}

static void LMP_CODE link_page_try_timeout_enable(void *priv)
{
    if (check_lmp_detch_over()) {
        page_parm.sys_time_id = 0;
        link_page_enable(page_parm.remote_addr, page_parm.remote_sr, page_parm.clk_offset, page_completed);
    } else {
        page_parm.sys_time_id = sys_timeout_add(page_parm.remote_addr, link_page_try_timeout_enable, 50);
    }
}

void LMP_CODE link_page_try_start_disable(int err)
{
    if (page_parm.sys_time_id) {
        sys_timer_del(page_parm.sys_time_id);       /* sic: added with sys_timeout_add */
        page_parm.sys_time_id = 0;
        page_completed(page_parm.remote_addr, -22);
    }
    page_parm.remote_name_req = 0;
    link_page_disable(err);
}

u8 LMP_CODE get_page_remote_name(void)
{
    return page_parm.remote_name_req;
}

void LMP_CODE link_page_try_start_enable(u8 name_req, u8 *addr, u8 sr, int clk_offset)
{
    if (page_parm.sys_time_id)
        link_page_try_start_disable(-14);
    local_irq_disable();
    memcpy(page_parm.remote_addr, addr, 6);
    page_parm.remote_name_req = name_req;
    page_parm.remote_sr = sr;
    page_parm.clk_offset = clk_offset;
    local_irq_enable();
    page_parm.sys_time_id = sys_timeout_add(page_parm.remote_addr, link_page_try_timeout_enable, 20);
}

void LMP_CODE lmp_scan_enable(u8 enable)
{
    hci_param.scan_enable = enable;
    if (!(enable & 1)) {
        if (CONFIG_BTCTLER_TWS_ENABLE)
            link_inquiry_scan_disable();
        else
            link_inquiry_scan_try_start_disable();
    }
    if (!(enable & 2)) {
        if (CONFIG_BTCTLER_TWS_ENABLE)
            link_page_scan_disable();
        else
            link_page_scan_try_start_disable();
    }
    if (enable & 1) {
        if (CONFIG_BTCTLER_TWS_ENABLE)
            link_inquiry_scan_enable(0);
        else
            link_inquiry_scan_try_start_enable(0);
    }
    if (enable & 2) {
        if (CONFIG_BTCTLER_TWS_ENABLE)
            __link_page_scan_enable(0);
        else
            link_page_scan_try_start_enable(0);
    }
}
