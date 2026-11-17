/* Owned lmp_inquiry_page.c: starting page and the scans.  Each waits until
 * no detach is in progress, retrying every 50 ms. */
#include <string.h>
#include "btctrler/glue.h"

_Static_assert(!BTCFG_TWS, "TWS scans are not implemented here");

static void link_inquiry_scan_try_timeout_enable(void *p)
{
    (void)p;
    if (check_lmp_detch_over()) {
        inquiry_scan_parm.sys_time_id = 0;
        link_inquiry_scan_enable(inquiry_scan_parm.clk_base);
    } else {
        inquiry_scan_parm.sys_time_id =
            sys_timeout_add(&inquiry_scan_parm, link_inquiry_scan_try_timeout_enable, 50);
    }
}

static void link_inquiry_scan_try_start_enable(void)
{
    if (inquiry_scan_parm.sys_time_id) sys_timeout_del((u16)inquiry_scan_parm.sys_time_id);
    inquiry_scan_parm.clk_base = 0;
    inquiry_scan_parm.sys_time_id =
        sys_timeout_add(&inquiry_scan_parm, link_inquiry_scan_try_timeout_enable, 10);
}

void link_inquiry_scan_try_start_disable(void)
{
    if (inquiry_scan_parm.sys_time_id) {
        sys_timeout_del((u16)inquiry_scan_parm.sys_time_id);
        inquiry_scan_parm.sys_time_id = 0;
    }
    link_inquiry_scan_disable();
}

static void link_page_scan_try_timeout_enable(void *p)
{
    (void)p;
    if (check_lmp_detch_over()) {
        page_scan_parm.sys_time_id = 0;
        link_page_scan_enable(page_scan_parm.clk_base);
    } else {
        page_scan_parm.sys_time_id =
            sys_timeout_add(&page_scan_parm, link_page_scan_try_timeout_enable, 50);
    }
}

static void link_page_scan_try_start_enable(void)
{
    if (page_scan_parm.sys_time_id) sys_timeout_del((u16)page_scan_parm.sys_time_id);
    page_scan_parm.clk_base = 0;
    page_scan_parm.sys_time_id = sys_timeout_add(&page_scan_parm, link_page_scan_try_timeout_enable, 10);
}

void link_page_scan_try_start_disable(void)
{
    if (page_scan_parm.sys_time_id) {
        sys_timeout_del((u16)page_scan_parm.sys_time_id);
        page_scan_parm.sys_time_id = 0;
    }
    link_page_scan_disable();
}

/* HCI Write Scan Enable: bit 0 inquiry scan, bit 1 page scan. */
void lmp_scan_enable(u8 enable)
{
    hci_param.scan_enable = enable & 7;
    if (!(enable & 1)) link_inquiry_scan_try_start_disable();
    if (!(enable & 2)) link_page_scan_try_start_disable();
    if (enable & 1) link_inquiry_scan_try_start_enable();
    if (enable & 2) link_page_scan_try_start_enable();
}

/* Page (or remote name request page) finished: err 0 connected, 4 page
 * timeout, -22 cancelled.  Called by the link layer too. */
void page_completed(u8 *addr, int err)
{
    static const u8 no_name[248];
    if (page_parm.remote_name_req) {
        if (err == 4 && BTCFG_NAME_REQ) {
            link_page_disable(0);
            page_parm.remote_name_req = 0;
            /* Remote Name Request Complete, page timeout.  Fix: the SDK sent
             * 248 bytes read from the 6-byte address as the name. */
            hci_send_event(0x07, "1AC248", 4, addr, no_name);
        }
    } else if (err) {
        if (err == 4) link_page_disable(0);
        hci_send_event(0x03, "1HA11", err, 0, addr, 1, 0);    /* Connection Complete, failed */
    }
    lmp_scan_enable(hci_param.scan_enable);
}

static void link_page_try_timeout_enable(void *p)
{
    (void)p;
    if (check_lmp_detch_over()) {
        page_parm.sys_time_id = 0;
        link_page_enable(page_parm.remote_addr, page_parm.remote_sr, page_parm.clk_offset, page_completed);
    } else {
        page_parm.sys_time_id = sys_timeout_add(page_parm.remote_addr, link_page_try_timeout_enable, 50);
    }
}

void link_page_try_start_enable(u8 remote_name_req, u8 *remote_addr, u8 remote_sr, int clk_offset)
{
    if (page_parm.sys_time_id) link_page_try_start_disable(-14);
    local_irq_disable();
    memcpy(page_parm.remote_addr, remote_addr, 6);
    page_parm.remote_name_req = remote_name_req;
    page_parm.remote_sr = remote_sr;
    page_parm.clk_offset = clk_offset;
    local_irq_enable();
    page_parm.sys_time_id = sys_timeout_add(page_parm.remote_addr, link_page_try_timeout_enable, 20);
}

void link_page_try_start_disable(int err)
{
    if (page_parm.sys_time_id) {
        sys_timer_del((u16)page_parm.sys_time_id);    /* sic: the SDK deletes this timeout as a timer */
        page_parm.sys_time_id = 0;
        page_completed(page_parm.remote_addr, -22);
    }
    page_parm.remote_name_req = 0;
    link_page_disable(err);
}
