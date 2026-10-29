/* BD19 H2 MUSB host transport recovered from h4_usb_interface.c.o. */
#include "../sdk.h"
#include "../glue.h"
#include <stdio.h>
#include <string.h>

extern const u8 log_tag_const_i_H4_USB, log_tag_const_e_H4_USB, log_tag_const_d_H4_USB;
extern void log_print(int, const char *, const char *, ...);
extern void printf_buf(u8 *, int);
#define INFO(...)  do { if (log_tag_const_i_H4_USB) log_print(2, 0, __VA_ARGS__); } while (0)
#define ERROR(...) do { if (log_tag_const_e_H4_USB) log_print(4, 0, __VA_ARGS__); } while (0)
#define DEBUG(...) do { if (log_tag_const_d_H4_USB) log_print(1, 0, __VA_ARGS__); } while (0)

#define USB_CODE __attribute__((section(".hci_interface_code")))
#define USB_REG  ((volatile u32 *)0x1e1800u)
#define USB_IO   ((volatile u32 *)0x1e5400u)

extern void request_irq(u8, u8, void (*)(void), u8);
extern void bit_clr_ie(u8);
extern void bit_set_ie(u8);
static void cpu_reset(void);
static void usb_init(const void *) USB_CODE;
static int usb_open(void) USB_CODE;
static int usb_close(void) USB_CODE;
static void usb_register_packet_handler(void (*)(int, const u8 *, int)) USB_CODE;
static int usb_can_send_packet_now(uint8_t) USB_CODE;
static int usb_send_packet(int, const u8 *, int) USB_CODE;
static void usb_dummy_handler(int, const u8 *, int) USB_CODE;
static void usbd_irq_handler(void) USB_CODE;
static void bluetooth_class_packet_handler(struct usb_endpoint *, struct usb_ctrlrequest *) USB_CODE;
static void vendor_class_packet_handler(struct usb_endpoint *, struct usb_ctrlrequest *) USB_CODE;
static void __hw_write_usb(u8, u8) USB_CODE;
static u8 __hw_read_usb(u8) USB_CODE;
static u16 __musb_read_reg_rxcsr(void) USB_CODE;
static u16 __musb_read_reg_txcsr(void) USB_CODE;
static void __musb_ep0_rx_clear(u16) USB_CODE;
static void __musb_ep_rx_clear(struct usb_endpoint *) USB_CODE;
static void __musb_ep_data_out_resume(struct usb_endpoint *) USB_CODE;
static void __musb_endpoint_init(u8, u8, u8, u16, u8) USB_CODE;
static void __musb_request_packet(struct usb_endpoint *, u8 *, u16) USB_CODE;
static void __musb_send_stall(struct usb_endpoint *) USB_CODE;
static void __musb_ep_data_in(struct usb_endpoint *) USB_CODE;
static void __musb_ep_data_out(struct usb_endpoint *) USB_CODE;
static void __musb_send_packet(struct usb_endpoint *, u8 *, u16) USB_CODE;
static struct usb_endpoint *usb_endpoint_by_number(u8, u8) USB_CODE;
static void usbd_send_packet(struct usb_endpoint *, u8 *, int) USB_CODE;
static void h4_usbd_thread_process(struct thread *) USB_CODE;

__attribute__((section(".hci_interface_const"), aligned(1)))
const u8 hci_encryption_change[3] = { 0x08, 0x04, 0x00 };
__attribute__((section(".hci_interface_const"), aligned(1)))
const u8 smp_encryption_information[9] = { 0x01, 0x20, 0x15, 0, 0x11, 0, 6, 0, 6 };
static const hci_transport_t hci_transport_usb __attribute__((section(".hci_interface_const"), aligned(4))) = {
    "H2_MUSB", usb_init, usb_open, usb_close, usb_register_packet_handler,
    usb_can_send_packet_now, usb_send_packet, 0, 0, 0
};
static const u8 iso_alt_setting[6] __attribute__((section(".hci_interface_const"), aligned(1))) = {0,9,17,25,33,49};
static struct usb_hdl hdl __attribute__((section(".hci_interface_bss"), aligned(4)));
static void (*packet_handler)(int, const u8 *, int) __attribute__((section(".hci_interface_data"), aligned(4))) = usb_dummy_handler;

static void cpu_reset(void) { p33_soft_reset(); }

const hci_transport_t *hci_transport_usb_instance(void) __attribute__((section(".hci_interface_code")));
const hci_transport_t *hci_transport_usb_instance(void) { return &hci_transport_usb; }

static void usb_init(const void *arg)
{
    (void)arg;
    INFO("[Info]: [H4_USB]ops -> init\r\n");
    memset(&hdl, 0, sizeof(hdl));
}

static int usb_open(void)
{
    thread_create(&hdl.thread, "h4_usb", h4_usbd_thread_process, &bt_task_thread);
    USB_REG[0] &= (u32)-16438;
    USB_REG[0] &= (u32)-2049;
    USB_REG[1] = 0;
    USB_REG[0] |= 1;
    USB_IO[0] |= 4096;
    USB_REG[0] |= 1;
    USB_IO[0] |= 8192;
    USB_REG[0] |= 2048;
    USB_IO[0] &= (u32)-4097;
    USB_REG[0] |= 60;
    USB_REG[7] = (u32)hdl.ep0_dma;
    USB_REG[9] = (u32)hdl.ep1_rx_dma;
    USB_REG[8] = (u32)hdl.ep1_tx_dma;
    USB_REG[11] = (u32)hdl.ep2_rx_dma;
    USB_REG[10] = (u32)hdl.ep2_tx_dma;
    USB_REG[12] = (u32)hdl.ep3_tx_dma;
    USB_IO[0] &= (u32)-65; USB_IO[0] &= (u32)-129;
    USB_IO[0] &= (u32)-17; USB_IO[0] &= (u32)-33;
    USB_IO[0] |= 16; USB_IO[0] |= 128; USB_IO[0] |= 256; USB_IO[0] |= 512;
    request_irq(9, 1, usbd_irq_handler, 0);
    return 0;
}

static int usb_close(void)
{
    USB_REG[0] &= (u32)-16438; USB_REG[0] &= (u32)-2049;
    USB_REG[1] = 0; USB_REG[0] |= 1; USB_IO[0] |= 4096;
    USB_IO[0] &= (u32)-65; USB_IO[0] &= (u32)-129;
    USB_IO[0] &= (u32)-17; USB_IO[0] &= (u32)-33;
    return 0;
}

static void usb_register_packet_handler(void (*handler)(int, const u8 *, int))
{
    packet_handler = handler;
    INFO("[Info]: [H4_USB]registering packet handler 0x%x\r\n", handler);
}
static int usb_can_send_packet_now(uint8_t type) { (void)type; return 0; }
static void usb_dummy_handler(int type, const u8 *data, int len) { (void)type; (void)data; (void)len; }

static void __hw_write_usb(u8 reg, u8 value)
{
    if (USB_REG[0] & 4) {
        local_irq_disable(); USB_REG[1] = ((u32)reg << 8) | value;
        __asm__ volatile("nop");
        while ((s16)USB_REG[1] >= 0) { }
        local_irq_enable();
    }
}
static u8 __hw_read_usb(u8 reg)
{
    u8 value = 0;
    if (USB_REG[0] & 4) {
        local_irq_disable(); USB_REG[1] = ((u32)(reg | 64) << 8);
        __asm__ volatile("nop");
        while ((s16)USB_REG[1] >= 0) { }
        value = (u8)USB_REG[1]; local_irq_enable();
    }
    return value;
}
static u16 __musb_read_reg_rxcsr(void) { u8 lo=__hw_read_usb(20), hi=__hw_read_usb(21); return ((u16)hi<<8)|lo; }
static u16 __musb_read_reg_txcsr(void) { u8 lo=__hw_read_usb(17), hi=__hw_read_usb(18); return ((u16)hi<<8)|lo; }
static void __musb_ep0_rx_clear(u16 size) { __hw_write_usb(14,0); __hw_write_usb(17,size ? 64 : 72); }
static void __musb_ep_rx_clear(struct usb_endpoint *ep) { __hw_write_usb(14,ep->number); (void)__musb_read_reg_rxcsr(); __hw_write_usb(20,16); }
static void __musb_ep_data_out_resume(struct usb_endpoint *ep)
{
    bit_clr_ie(9); __hw_write_usb(14,ep->number);
    if (!ep->number) __musb_ep0_rx_clear(ep->dma_size); else __musb_ep_rx_clear(ep);
    bit_set_ie(9);
}

static void __musb_endpoint_init(u8 index, u8 number, u8 direction, u16 maxpkt, u8 type)
{
    struct usb_endpoint *ep = &hdl.ep[index];
    u8 *dma_buf;
    ep->number = number; ep->maxpktsize = maxpkt; ep->transfer_type = type; ep->direction = direction;
    switch (number) {
    case 0: dma_buf = hdl.ep0_dma; break;
    case 1: dma_buf = (direction & 0x80) ? hdl.ep1_tx_dma : hdl.ep1_rx_dma; break;
    case 2: dma_buf = (direction & 0x80) ? hdl.ep2_tx_dma : hdl.ep2_rx_dma; break;
    case 3: dma_buf = (direction & 0x80) ? hdl.ep3_tx_dma : 0; break;
    default: dma_buf = 0; break;
    }
    if (!dma_buf) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/h4_usb_interface.c", 1306); printf("ASSERT-FAILD: ptr Invalid Endpoint DMA"); cpu_assert_debug(); }
        else cpu_reset();
    }
    ep->dma_buf = dma_buf;
}

static void __musb_request_packet(struct usb_endpoint *ep, u8 *dst, u16 size)
{
    if (!ep) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/h4_usb_interface.c", 889); printf("ASSERT-FAILD: ep Invalid endpoint"); cpu_assert_debug(); }
        else cpu_reset();
    }
    ep->dma_size = size;
    if (size) { ep->dst = dst; ep->state = ep->number ? 1 : 2; }
    if (!ep->number) __musb_ep0_rx_clear(size); else __musb_ep_rx_clear(ep);
}

static void __musb_send_stall(struct usb_endpoint *ep)
{
    u8 number = ep->number;
    if (!number) { __hw_write_usb(14,0); __hw_write_usb(17,__hw_read_usb(17)|96); }
    else {
        __hw_write_usb(14,number);
        if ((ep->direction & 0x80)) __hw_write_usb(17,(u8)__musb_read_reg_txcsr()|16);
        else __hw_write_usb(17,(u8)__musb_read_reg_rxcsr()|32);
    }
    INFO("[Info]: [H4_USB][Stall]\r\n");
}

static void __musb_ep_data_in(struct usb_endpoint *ep)
{
    u16 count;
    if (ep->state) {
        count = ep->dma_size > ep->maxpktsize ? ep->maxpktsize : ep->dma_size;
        memcpy(ep->dma_buf, ep->src, count);
        ep->dma_size -= count; ep->src += count;
        if (!ep->dma_size && count != ep->maxpktsize) ep->state = 0;
        else if (!ep->dma_size) DEBUG("[Debug]: [H4_USB]Not End\r\n");
        if (!ep->number) {
            __hw_write_usb(14,0); __hw_write_usb(17,__hw_read_usb(17)|64);
            USB_REG[2] = count;
            __hw_write_usb(17, ep->state ? 2 : 10);
        } else {
            bit_clr_ie(9); ((u32 *)(uintptr_t)(0x1e1808u + (u32)ep->number * 4))[0] = count;
            __hw_write_usb(14,ep->number); __hw_write_usb(17,1); bit_set_ie(9);
        }
    } else { ERROR("<Error>: [H4_USB]Werid case\r\n"); ep->state = 0; }
}

static void __musb_ep_data_out(struct usb_endpoint *ep)
{
    u16 count = __hw_read_usb(22);
    if (!ep->dma_size) ep->state = 0;
    else {
        memcpy(ep->dst, ep->dma_buf, count); ep->dma_size -= count; ep->dst += count;
        if (!ep->dma_size) {
            ep->state = 0;
            switch (ep->number) {
            case 0:
                if (hdl.bluetooth.cmd_pending.counter == 2) {
                    hdl.bluetooth.cmd_length = (u16)(ep->dst - hdl.bluetooth.cmd);
                    local_irq_disable(); __asm__ volatile("csync;"); hdl.bluetooth.cmd_pending.counter=1; local_irq_enable();
                    thread_resume(&hdl.thread); return;
                } break;
            case 1:
                hdl.bluetooth.acl_length = (u16)(ep->dst - hdl.bluetooth.acl);
                local_irq_disable(); __asm__ volatile("csync;"); hdl.bluetooth.acl_pending.counter=1; local_irq_enable();
                thread_resume(&hdl.thread); return;
            case 2: {
                u8 sco_length = hdl.bluetooth.sco_length = (u8)(ep->dst - hdl.bluetooth.sco);
                local_irq_disable(); __asm__ volatile("csync;"); hdl.bluetooth.sco_pending.counter=1; local_irq_enable();
                INFO("[Info]: [H4_USB][SCO >+] %d - %d\r\n", hdl.bluetooth.in_fifos_w_index, sco_length);
                u8 i=hdl.bluetooth.in_fifos_w_index;
                memcpy(&hdl.bluetooth.in_synchronous_fifos[i*147], hdl.bluetooth.sco, sco_length);
                hdl.bluetooth.in_fifos_w_index = hdl.bluetooth.in_fifos_w_index == 7 ? 0 : hdl.bluetooth.in_fifos_w_index+1;
                thread_resume(&hdl.thread); break;
            } default: break;
            }
        }
    }
    if (!ep->number) __musb_ep0_rx_clear(ep->dma_size); else __musb_ep_rx_clear(ep);
}

static void __musb_send_packet(struct usb_endpoint *ep, u8 *data, u16 size)
{
    if (!ep) {
        if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/h4_usb_interface.c", 809); printf("ASSERT-FAILD: ep Invalid endpoint"); cpu_assert_debug(); }
        else cpu_reset();
    }
    ep->dma_size=size;
    if (size) { ep->src=data; ep->state=1; }
    __musb_ep_data_in(ep);
}

static struct usb_endpoint *usb_endpoint_by_number(u8 number, u8 direction)
{
    for (u32 i=0; i<6; ++i)
        if (hdl.ep[i].number==number && hdl.ep[i].direction==direction) return &hdl.ep[i];
    return 0;
}

static void usbd_send_packet(struct usb_endpoint *ep, u8 *data, int size)
{
    __musb_send_packet(ep,data,(u16)size);
    for (;;) {
        u32 start=sys_timer_get_time();
        for (;;) {
            bit_clr_ie(9); __hw_write_usb(14,ep->number);
            u16 csr=__musb_read_reg_txcsr(); bit_set_ie(9);
            if (!(csr&1)) break;
            if (start+2000 < sys_timer_get_time()) {
                if (log_tag_const_e_H4_USB) {
                    u32 now=sys_timer_get_time();
                    log_print(4,0,"<Error>: [H4_USB]Send bulk Timeout / start_time_ms = %d / rtos_time_get = %d\r\n", start, now);
                }
                local_irq_disable(); __asm__ volatile("csync;"); hdl.state.counter=0; local_irq_enable();
                return;
            }
            __asm__ volatile("csync;");
            if (!hdl.state.counter) { ERROR("<Error>: [H4_USB]Send bulk USB Reset\r\n"); return; }
        }
        if (!ep->state) return;
        __musb_ep_data_in(ep);
    }
}

static void bluetooth_class_packet_handler(struct usb_endpoint *ep, struct usb_ctrlrequest *req)
{
    if (req->bRequest==0) {
        local_irq_disable(); __asm__ volatile("csync;"); hdl.bluetooth.cmd_pending.counter=2; local_irq_enable();
        __musb_request_packet(ep, hdl.bluetooth.cmd, *(u16 *)&req->wLength);
    }
}
static void vendor_class_packet_handler(struct usb_endpoint *ep, struct usb_ctrlrequest *req)
{ (void)req; __musb_send_stall(ep); }

static int usb_send_packet(int type, const u8 *data, int size)
{
    __asm__ volatile("csync;");
    if (!hdl.state.counter) { ERROR("<Error>: [H4_USB]USB idle\r\n"); return -1; }
    switch (type) {
    case 4:
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][EVT < ]\r\n"); printf_buf((u8 *)data,size); }
        usbd_send_packet(&hdl.ep[1], (u8 *)data, size); break;
    case 2:
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][ACL < ]\r\n"); printf_buf((u8 *)data,size); }
        usbd_send_packet(&hdl.ep[3], (u8 *)data, size); break;
    case 3: {
        u8 i=hdl.bluetooth.fifos_w_index;
        memcpy(&hdl.bluetooth.synchronous_fifos[i*147], data, size);
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][SCO <+] %d\r\n", i); i=hdl.bluetooth.fifos_w_index; }
        hdl.bluetooth.fifos_w_index=i==7 ? 0 : i+1; break;
    }
    default: if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/h4_usb_interface.c", 1605); printf("ASSERT-FAILD: 0 h4_usb_notify_host - unkown hci packet type"); cpu_assert_debug(); } else cpu_reset(); return -1;
    }
    return 0;
}

static void h4_usbd_thread_process(struct thread *thread)
{
    __asm__ volatile("csync;");
    if (hdl.bluetooth.cmd_pending.counter==1) {
        local_irq_disable(); __asm__ volatile("csync;"); hdl.bluetooth.cmd_pending.counter=0; local_irq_enable();
        u16 length=hdl.bluetooth.cmd_length;
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][CMD > ]\r\n"); printf_buf(hdl.bluetooth.cmd,length); }
        packet_handler(1,hdl.bluetooth.cmd,hdl.bluetooth.cmd_length); __musb_ep_data_out_resume(hdl.ep);
    }
    __asm__ volatile("csync;");
    if (hdl.bluetooth.acl_pending.counter) {
        local_irq_disable(); __asm__ volatile("csync;"); hdl.bluetooth.acl_pending.counter=0; local_irq_enable();
        u16 length=hdl.bluetooth.acl_length;
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][ACL > ]\r\n"); printf_buf(hdl.bluetooth.acl,length); }
        packet_handler(2,hdl.bluetooth.acl,hdl.bluetooth.acl_length); __musb_ep_data_out_resume(&hdl.ep[2]);
    }
    __asm__ volatile("csync;");
    if (hdl.bluetooth.sco_pending.counter) {
        u16 size=hdl.ep[4].frame_size*3;
        INFO("[Info]: [H4_USB][SCO >-] %d - %d\r\n", hdl.bluetooth.in_fifos_r_index, size);
        u8 i=hdl.bluetooth.in_fifos_r_index;
        hdl.bluetooth.in_fifos_r_index=i==7 ? 0 : i+1;
        packet_handler(3,&hdl.bluetooth.in_synchronous_fifos[i*147],size);
        if (hdl.bluetooth.in_fifos_r_index==hdl.bluetooth.in_fifos_w_index) {
            local_irq_disable(); __asm__ volatile("csync;"); hdl.bluetooth.sco_pending.counter=0; local_irq_enable();
        }
    }
    __asm__ volatile("csync;");
    u8 pending=hdl.bluetooth.cmd_pending.counter; __asm__ volatile("csync;"); __asm__ volatile("csync;");
    if (!((hdl.bluetooth.acl_pending.counter|pending|hdl.bluetooth.sco_pending.counter)&255)) thread_suspend(thread,0);
}

static void __attribute__((interrupt)) usbd_irq_handler(void)
{
    u8 intrusb=__hw_read_usb(11), intrusb_en=__hw_read_usb(6);
    if ((intrusb&4)&intrusb_en) {
        hdl.usbd_class_packet_handler=bluetooth_class_packet_handler;
        hdl.usbd_vendor_packet_handler=vendor_class_packet_handler; hdl.feature=0;
        __musb_endpoint_init(0,0,0,64,0); __musb_endpoint_init(1,3,(u8)-128,16,3);
        __musb_endpoint_init(2,1,0,64,2); __musb_endpoint_init(3,1,(u8)-128,64,2);
        __musb_endpoint_init(4,2,0,63,1); __musb_endpoint_init(5,2,(u8)-128,63,1);
        __hw_write_usb(1,(u8)-127); __hw_write_usb(11,5); __hw_write_usb(7,5); __hw_write_usb(8,0);
        __hw_write_usb(9,6); __hw_write_usb(10,0);
        __hw_write_usb(14,3); __hw_write_usb(17,72); __hw_write_usb(16,65);
        __hw_write_usb(14,1); __hw_write_usb(20,(u8)-112); __hw_write_usb(19,65);
        __hw_write_usb(14,1); __hw_write_usb(17,72); __hw_write_usb(16,65);
        __hw_write_usb(14,2); __hw_write_usb(20,(u8)-112); __hw_write_usb(21,64); __hw_write_usb(19,65);
        __hw_write_usb(14,2); __hw_write_usb(17,72); __hw_write_usb(18,64); __hw_write_usb(16,5);
        ERROR("<Error>: [H4_USB]Reset\r\n");
        local_irq_disable(); __asm__ volatile("csync;"); hdl.state.counter=0; local_irq_enable();
    }
    u8 intsrc=__hw_read_usb(7), inten=__hw_read_usb(2), status=inten&intsrc;
    if (status&1) {
        __hw_write_usb(14,0); u8 csr=__hw_read_usb(17);
        if (hdl.address) { __hw_write_usb(0,hdl.address); hdl.address=0; }
        if (csr&4) { __hw_write_usb(17,csr&~4); hdl.ep[0].state=0; ERROR("<Error>: [H4_USB]CSR0P_SentStall\r\n"); }
        if (csr&16) { __hw_write_usb(17,csr&127); hdl.ep[0].state=0; ERROR("<Error>: [H4_USB]CSR0P_SetupEnd\r\n"); }
        if (!hdl.ep[0].state && (csr&1)) {
            struct usb_ctrlrequest *req=(void *)hdl.ep[0].dma_buf;
            u8 recip=req->bRequestType&96;
            if (recip==32) { if (hdl.usbd_class_packet_handler) hdl.usbd_class_packet_handler(hdl.ep,req); }
            else if (recip==64) { if (hdl.usbd_vendor_packet_handler) hdl.usbd_vendor_packet_handler(hdl.ep,req); }
            else switch(req->bRequest) {
            case 0: { u16 status=0, idx=*(u16 *)&req->wIndex; u8 type=req->bRequestType&31;
                if (type==1) { __musb_send_packet(hdl.ep,(u8 *)&status,2); INFO("[Info]: [H4_USB]USB_RECIP_INTERFACE\r\n"); }
                else if (type==2) { struct usb_endpoint *e=usb_endpoint_by_number(idx&15,idx&128); status=e->state&1; __musb_send_packet(e,(u8 *)&status,2); INFO("[Info]: [H4_USB]USB_RECIP_ENDPOINT\r\n"); }
                else if (type==0) { status=hdl.feature&3; __musb_send_packet(hdl.ep,(u8 *)&status,2); INFO("[Info]: [H4_USB]USB_RECIP_DEVICE\r\n"); }
                else __musb_send_stall(hdl.ep);
                INFO("[Info]: [H4_USB]USB_REQ_GET_STATUS\r\n"); break; }
            case 1: {
                u8 recipient=req->bRequestType&31; u16 value=*(u16 *)&req->wValue;
                if (recipient==1) __musb_send_stall(hdl.ep);
                else if (recipient==2) {
                    if (value==0) { u16 idx=*(u16 *)&req->wIndex; struct usb_endpoint *e=usb_endpoint_by_number(idx&15,idx&128);
                        __hw_write_usb(14,e->number); __hw_write_usb(17,(e->direction & 0x80)?64:128);
                    } else __musb_send_stall(hdl.ep);
                } else if (recipient==0) { if(value==2) __musb_send_stall(hdl.ep); }
                else { __musb_send_stall(hdl.ep); break; }
                __musb_request_packet(hdl.ep,0,0); INFO("[Info]: [H4_USB]USB_REQ_CLEAR_FEATURE\r\n"); break;
            }
            case 3: INFO("[Info]: [H4_USB]USB_REQ_SET_FEATURE\r\n"); break;
            case 5: { u8 address=(u8)*(u16 *)&req->wValue; __musb_request_packet(hdl.ep,0,0); hdl.address=address; INFO("[Info]: [H4_USB]USB_REQ_SET_ADDRESS\r\n"); break; }
            case 6: { u16 len=*(u16 *)&req->wLength, val=*(u16 *)&req->wValue; const u8 *d; u16 n;
                u8 dtype=(u8)(val>>8);
                if (dtype==1) { d=device_descriptor; n=18; __musb_send_packet(hdl.ep,(u8 *)d,len<n?len:n); INFO("[Info]: [H4_USB]USB_DT_DEVICE\r\n"); }
                else if (dtype==2) { d=configuration_descriptor; n=177; __musb_send_packet(hdl.ep,(u8 *)d,len<n?len:n); INFO("[Info]: [H4_USB]USB_DT_CONFIG\r\n"); }
                else if (dtype==3) {
                    switch((u8)val) { case 0:d=string0_descriptor;n=*d;break; case 1:d=string1_descriptor;n=*d;break; case 2:d=string2_descriptor;n=*d;break; }
                    __musb_send_packet(hdl.ep,(u8 *)d,len<n?len:n);
                    INFO("[Info]: [H4_USB]USB_DT_STRING\r\n");
                } else { __musb_send_stall(hdl.ep); INFO("[Info]: [H4_USB]USB_REQ_GET_DESCRIPTOR - \r\n"); break; }
                INFO("[Info]: [H4_USB]USB_REQ_GET_DESCRIPTOR - \r\n"); break;
            }
            case 7: __musb_request_packet(hdl.ep,0,0); INFO("[Info]: [H4_USB]USB_REQ_SET_DESCRIPTOR\r\n"); break;
            case 8: hdl.ep[0].dma_size=0; __musb_ep_data_in(hdl.ep); INFO("[Info]: [H4_USB]USB_REQ_GET_CONFIGURATION\r\n"); break;
            case 9: __musb_request_packet(hdl.ep,0,0); INFO("[Info]: [H4_USB]USB_REQ_SET_CONFIGURATION\r\n"); break;
            case 10: hdl.ep[0].dma_size=0; __musb_ep_data_in(hdl.ep); INFO("[Info]: [H4_USB]USB_REQ_GET_INTERFACE\r\n"); break;
            case 11: { u16 idx=*(u16 *)&req->wIndex, alt=*(u16 *)&req->wValue; if(idx==1) {
                    __musb_request_packet(hdl.ep,0,0); u8 frame=iso_alt_setting[alt];
                    hdl.ep[4].frame_size=frame; hdl.ep[5].frame_size=frame;
                    u8 error_disabled=(log_tag_const_e_H4_USB==0);
                    if (!error_disabled) { log_print(4,0,"<Error>: [H4_USB]Interface 1 : Alt setting %d\r\n", frame); frame=hdl.ep[5].frame_size; }
                    hdl.iso_frame_size=2;
                    if(frame) {
                        if (!error_disabled) { log_print(4,0,"<Error>: [H4_USB]ISO start >>\r\n"); frame=hdl.ep[5].frame_size; }
                        hdl.bluetooth.in_fifos_r_index=0; hdl.bluetooth.in_fifos_w_index=0; hdl.bluetooth.fifos_w_index=0; hdl.bluetooth.fifos_r_index=0;
                        __musb_send_packet(&hdl.ep[5],hdl.bluetooth.synchronous_fifos,frame);
                    }
                } else __musb_send_stall(hdl.ep);
                INFO("[Info]: [H4_USB]USB_REQ_SET_INTERFACE\r\n"); break; }
            case 12: hdl.ep[0].dma_size=0; __musb_ep_data_in(hdl.ep); INFO("[Info]: [H4_USB]USB_REQ_SYNCH_FRAME\r\n"); break;
            default: __musb_send_stall(hdl.ep); break;
            }
        } else if (hdl.ep[0].state==1) {
            if (csr&2) ERROR("<Error>: [H4_USB]CSR0P_TxPktRdy\r\n"); else __musb_ep_data_in(hdl.ep);
        } else if (hdl.ep[0].state==2) {
            if (csr&1) __musb_ep_data_out(hdl.ep); else ERROR("<Error>: [H4_USB][DataOut]:CSR0P_RxPktRdy\r\n");
        }
        local_irq_disable(); __asm__ volatile("csync;"); hdl.state.counter=1; local_irq_enable();
    }
    if (status&4) {
        __hw_write_usb(14,hdl.ep[5].number);
        INFO("[Info]: [H4_USB][SCO <-] %d-%d\r\n", hdl.bluetooth.fifos_r_index, hdl.ep[5].frame_offset);
        u8 offset=hdl.ep[5].frame_offset;
        if (!offset) { u8 i=hdl.bluetooth.fifos_r_index; hdl.bluetooth.fifos_r_index=i==7?0:i+1; }
        u8 size=hdl.ep[5].frame_size; hdl.ep[5].frame_offset=offset==hdl.iso_frame_size?0:offset+1;
        __musb_send_packet(&hdl.ep[5],&hdl.bluetooth.synchronous_fifos[size*offset],size);
    }
    intsrc=__hw_read_usb(9); inten=__hw_read_usb(4); status=inten&intsrc;
    if (status&2) {
        __hw_write_usb(14,hdl.ep[2].number);
        if (!hdl.ep[2].dma_size) { u16 n=((struct hci_acl_data_packet *)hdl.ep[2].dma_buf)->total_len; hdl.ep[2].dma_size=n+4; if(n){hdl.ep[2].dst=hdl.bluetooth.acl;hdl.ep[2].state=1;} }
        __musb_ep_data_out(&hdl.ep[2]);
    }
    if (status&4) {
        __hw_write_usb(14,hdl.ep[4].number);
        if (!hdl.ep[4].dma_size) { u8 n=((struct hci_sco_data_packet *)hdl.ep[4].dma_buf)->total_len; hdl.ep[4].dma_size=n+3; if(n){hdl.ep[4].dst=hdl.bluetooth.sco;hdl.ep[4].state=1;} }
        __musb_ep_data_out(&hdl.ep[4]);
    }
}
