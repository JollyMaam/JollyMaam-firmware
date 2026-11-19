/* BD19 H2 MUSB host transport recovered from h4_usb_interface.c.o. */
#include "../sdk.h"
#include "../glue.h"
#include <stdio.h>
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_i_H4_USB, log_tag_const_e_H4_USB, log_tag_const_d_H4_USB;
extern void log_print(int, const char *, const char *, ...);
extern void printf_buf(u8 *, int);
#define INFO(...)  do { if (log_tag_const_i_H4_USB) log_print(2, 0, __VA_ARGS__); } while (0)
#define ERROR(...) do { if (log_tag_const_e_H4_USB) log_print(4, 0, __VA_ARGS__); } while (0)
#define DEBUG(...) do { if (log_tag_const_d_H4_USB) log_print(1, 0, __VA_ARGS__); } while (0)

typedef u16 usb_unaligned_u16 __attribute__((aligned(1), may_alias));
#define WIRE16(p) (*(const usb_unaligned_u16 *)(const void *)(p))
#define ATOMIC_READ(a) (*(volatile int *)&(a).counter)
#define USB_CODE __attribute__((section(".hci_interface_code")))
#ifdef UNITTEST
static volatile u32 usb_test_regs[32], usb_test_io[1];
extern u8 usb_test_read(u8);
extern void usb_test_write(u8, u8);
#define USB_REG usb_test_regs
#define USB_IO usb_test_io
#define USB_CSYNC() __asm__ volatile("")
#define USB_INTERRUPT
#else
#define USB_REG  ((volatile u32 *)0x1e1800u)
#define USB_IO   ((volatile u32 *)0x1e5400u)
#define USB_CSYNC() __asm__ volatile("csync;")
#define USB_INTERRUPT __attribute__((interrupt))
#endif

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
static void __musb_endpoint_init(u8, u8, u8, u16, u8) USB_CODE __attribute__((noinline));
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
#ifndef UNITTEST
_Static_assert(sizeof(struct usb_hdl)==8244, "BD19 USB handle");
#endif
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
    __builtin_memset(&hdl, 0, sizeof(hdl));
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
    USB_REG[7] = (u32)(uintptr_t)hdl.ep0_dma;
    USB_REG[9] = (u32)(uintptr_t)hdl.ep1_rx_dma;
    USB_REG[8] = (u32)(uintptr_t)hdl.ep1_tx_dma;
    USB_REG[11] = (u32)(uintptr_t)hdl.ep2_rx_dma;
    USB_REG[10] = (u32)(uintptr_t)hdl.ep2_tx_dma;
    USB_REG[12] = (u32)(uintptr_t)hdl.ep3_tx_dma;
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
#ifdef UNITTEST
    usb_test_write(reg, value);
#else
    if (USB_REG[0] & 4) {
        local_irq_disable(); USB_REG[1] = ((u32)reg << 8) | value;
        __asm__ volatile("nop");
        while ((s16)USB_REG[1] >= 0) { }
        local_irq_enable();
    }
#endif
}
static u8 __hw_read_usb(u8 reg)
{
#ifdef UNITTEST
    return usb_test_read(reg);
#else
    u8 value = 0;
    if (USB_REG[0] & 4) {
        local_irq_disable(); USB_REG[1] = ((u32)(reg | 64) << 8);
        __asm__ volatile("nop");
        while ((s16)USB_REG[1] >= 0) { }
        value = (u8)USB_REG[1]; local_irq_enable();
    }
    return value;
#endif
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
        __builtin_memcpy(ep->dma_buf, ep->src, count);
        ep->dma_size -= count; ep->src += count;
        if (!ep->dma_size && count != ep->maxpktsize) ep->state = 0;
        else if (!ep->dma_size) DEBUG("[Debug]: [H4_USB]Not End\r\n");
        if (!ep->number) {
            __hw_write_usb(14,0); __hw_write_usb(17,__hw_read_usb(17)|64);
            USB_REG[2] = count;
            __hw_write_usb(17, ep->state ? 2 : 10);
        } else {
            bit_clr_ie(9); ((u32 *)(void *)USB_REG)[2 + ep->number] = count;
            __hw_write_usb(14,ep->number); __hw_write_usb(17,1); bit_set_ie(9);
        }
    } else { ERROR("<Error>: [H4_USB]Werid case\r\n"); ep->state = 0; }
}

static void __musb_ep_data_out(struct usb_endpoint *ep)
{
    u16 count = __hw_read_usb(22);
    if (!ep->dma_size) ep->state = 0;
    else {
        __builtin_memcpy(ep->dst, ep->dma_buf, count); ep->dma_size -= count; ep->dst += count;
        if (!ep->dma_size) {
            ep->state = 0;
            switch (ep->number) {
            case 0:
                USB_CSYNC();
                if (ATOMIC_READ(hdl.bluetooth.cmd_pending) == 2) {
                    hdl.bluetooth.cmd_length = (u16)(ep->dst - hdl.bluetooth.cmd);
                    local_irq_disable(); USB_CSYNC(); hdl.bluetooth.cmd_pending.counter=1; local_irq_enable();
                    thread_resume(&hdl.thread); return;
                } break;
            case 1:
                hdl.bluetooth.acl_length = (u16)(ep->dst - hdl.bluetooth.acl);
                local_irq_disable(); USB_CSYNC(); hdl.bluetooth.acl_pending.counter=1; local_irq_enable();
                thread_resume(&hdl.thread); return;
            case 2: {
                hdl.bluetooth.sco_length = (u8)(ep->dst - hdl.bluetooth.sco);
                local_irq_disable(); USB_CSYNC(); hdl.bluetooth.sco_pending.counter=1; local_irq_enable();
                u8 sco_length=hdl.bluetooth.sco_length;
                INFO("[Info]: [H4_USB][SCO >+] %d - %d\r\n", hdl.bluetooth.in_fifos_w_index, sco_length);
                u8 i=hdl.bluetooth.in_fifos_w_index;
                __builtin_memcpy(&hdl.bluetooth.in_synchronous_fifos[i*147], hdl.bluetooth.sco, sco_length);
                hdl.bluetooth.in_fifos_w_index = i == 7 ? 0 : i+1;
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
                local_irq_disable(); USB_CSYNC(); hdl.state.counter=0; local_irq_enable();
                if (log_tag_const_e_H4_USB) {
                    u32 now=sys_timer_get_time();
                    log_print(4,0,"<Error>: [H4_USB]Send bulk Timeout / start_time_ms = %d / rtos_time_get = %d\r\n",start,now);
                }
                return;
            }
            USB_CSYNC();
            if (!ATOMIC_READ(hdl.state)) { ERROR("<Error>: [H4_USB]Send bulk USB Reset\r\n"); return; }
        }
        if (!ep->state) return;
        __musb_ep_data_in(ep);
    }
}

static void bluetooth_class_packet_handler(struct usb_endpoint *ep, struct usb_ctrlrequest *req)
{
    if (req->bRequest==0) {
        local_irq_disable(); USB_CSYNC(); hdl.bluetooth.cmd_pending.counter=2; local_irq_enable();
        __musb_request_packet(ep, hdl.bluetooth.cmd, WIRE16(&req->wLength));
    }
}
static void vendor_class_packet_handler(struct usb_endpoint *ep, struct usb_ctrlrequest *req)
{ (void)req; __musb_send_stall(ep); }

static int usb_send_packet(int type, const u8 *data, int size)
{
    USB_CSYNC();
    if (!ATOMIC_READ(hdl.state)) { ERROR("<Error>: [H4_USB]USB idle\r\n"); return -1; }
    switch (type) {
    case 4:
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][EVT < ]\r\n"); printf_buf((u8 *)data,size); }
        usbd_send_packet(&hdl.ep[1], (u8 *)data, size); break;
    case 2:
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][ACL < ]\r\n"); printf_buf((u8 *)data,size); }
        usbd_send_packet(&hdl.ep[3], (u8 *)data, size); break;
    case 3: {
        u8 i=hdl.bluetooth.fifos_w_index;
        __builtin_memcpy(&hdl.bluetooth.synchronous_fifos[i*147], data, size);
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][SCO <+] %d\r\n", i); i=hdl.bluetooth.fifos_w_index; }
        hdl.bluetooth.fifos_w_index=i==7 ? 0 : i+1; break;
    }
    default: if (config_asser) { printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/common/h4_usb_interface.c", 1605); printf("ASSERT-FAILD: 0 h4_usb_notify_host - unkown hci packet type"); cpu_assert_debug(); } else cpu_reset(); return -1;
    }
    return 0;
}

static void h4_usbd_thread_process(struct thread *thread)
{
    USB_CSYNC();
    if (ATOMIC_READ(hdl.bluetooth.cmd_pending)==1) {
        local_irq_disable(); USB_CSYNC(); hdl.bluetooth.cmd_pending.counter=0; local_irq_enable();
        u16 length=hdl.bluetooth.cmd_length;
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][CMD > ]\r\n"); printf_buf(hdl.bluetooth.cmd,length); }
        packet_handler(1,hdl.bluetooth.cmd,length); __musb_ep_data_out_resume(hdl.ep);
    }
    USB_CSYNC();
    if (ATOMIC_READ(hdl.bluetooth.acl_pending)) {
        local_irq_disable(); USB_CSYNC(); hdl.bluetooth.acl_pending.counter=0; local_irq_enable();
        u16 length=hdl.bluetooth.acl_length;
        if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB][ACL > ]\r\n"); printf_buf(hdl.bluetooth.acl,length); }
        packet_handler(2,hdl.bluetooth.acl,length); __musb_ep_data_out_resume(&hdl.ep[2]);
    }
    USB_CSYNC();
    if (ATOMIC_READ(hdl.bluetooth.sco_pending)) {
        u8 size=(u8)(hdl.ep[4].frame_size*3);
        INFO("[Info]: [H4_USB][SCO >-] %d - %d\r\n", hdl.bluetooth.in_fifos_r_index, size);
        u8 i=hdl.bluetooth.in_fifos_r_index;
        hdl.bluetooth.in_fifos_r_index=i==7 ? 0 : i+1;
        packet_handler(3,&hdl.bluetooth.in_synchronous_fifos[i*147],size);
        if (hdl.bluetooth.in_fifos_r_index==hdl.bluetooth.in_fifos_w_index) {
            local_irq_disable(); USB_CSYNC(); hdl.bluetooth.sco_pending.counter=0; local_irq_enable();
        }
    }
    USB_CSYNC();
    int pending=ATOMIC_READ(hdl.bluetooth.cmd_pending);
    USB_CSYNC(); pending |= ATOMIC_READ(hdl.bluetooth.acl_pending);
    USB_CSYNC(); pending |= ATOMIC_READ(hdl.bluetooth.sco_pending);
    if (!(pending&255)) thread_suspend(thread,0);
}

static void USB_INTERRUPT usbd_irq_handler(void)
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
        local_irq_disable(); USB_CSYNC(); hdl.state.counter=0; local_irq_enable();
    }
    u8 intsrc=__hw_read_usb(7), inten=__hw_read_usb(2), status=inten&intsrc;
    if (status&1) {
        __hw_write_usb(14,0); u8 csr=__hw_read_usb(17);
        if (hdl.address) { __hw_write_usb(0,hdl.address); hdl.address=0; }
        if (csr&4) { __hw_write_usb(17,csr&~4); hdl.ep[0].state=0; ERROR("<Error>: [H4_USB]CSR0P_SentStall\r\n"); }
        if (csr&16) { __hw_write_usb(17,csr&127); hdl.ep[0].state=0; ERROR("<Error>: [H4_USB]CSR0P_SetupEnd\r\n"); }
        u8 ep0_state = hdl.ep[0].state;
        if (ep0_state==0 && (csr&1)) {
            struct usb_ctrlrequest *req=(void *)hdl.ep[0].dma_buf;
            u8 recip=req->bRequestType&96;
            if (recip==32) { if (hdl.usbd_class_packet_handler) hdl.usbd_class_packet_handler(hdl.ep,req); }
            else if (recip==64) { if (hdl.usbd_vendor_packet_handler) hdl.usbd_vendor_packet_handler(hdl.ep,req); }
            else switch(req->bRequest) {
            case 0: { u16 status=0, idx=WIRE16(&req->wIndex); u8 type=req->bRequestType&31;
                if (type==1) { __musb_send_packet(hdl.ep,(u8 *)&status,2); if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB]USB_RECIP_INTERFACE\r\n"); log_print(2,0,"[Info]: [H4_USB]USB_REQ_GET_STATUS\r\n"); } break; }
                else if (type==2) { struct usb_endpoint *e=usb_endpoint_by_number(idx&15,idx&128); status=e->state&1; __musb_send_packet(e,(u8 *)&status,2); if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB]USB_RECIP_ENDPOINT\r\n"); log_print(2,0,"[Info]: [H4_USB]USB_REQ_GET_STATUS\r\n"); } break; }
                else if (type==0) { status=hdl.feature&3; __musb_send_packet(hdl.ep,(u8 *)&status,2); if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB]USB_RECIP_DEVICE\r\n"); log_print(2,0,"[Info]: [H4_USB]USB_REQ_GET_STATUS\r\n"); } break; }
                else __musb_send_stall(hdl.ep);
                INFO("[Info]: [H4_USB]USB_REQ_GET_STATUS\r\n"); break; }
            case 1: {
                u8 recipient=req->bRequestType&31; u16 value=WIRE16(&req->wValue);
                if (recipient==1) __musb_send_stall(hdl.ep);
                else if (recipient==2) {
                    if (value==0) { u16 idx=WIRE16(&req->wIndex); struct usb_endpoint *e=usb_endpoint_by_number(idx&15,idx&128);
                        __hw_write_usb(14,e->number); __hw_write_usb(17,(e->direction & 0x80)?64:128);
                    } else __musb_send_stall(hdl.ep);
                } else if (recipient==0) { if(value==2) __musb_send_stall(hdl.ep); }
                else { __musb_send_stall(hdl.ep); INFO("[Info]: [H4_USB]USB_REQ_CLEAR_FEATURE\r\n"); break; }
                __musb_request_packet(hdl.ep,0,0); INFO("[Info]: [H4_USB]USB_REQ_CLEAR_FEATURE\r\n"); break;
            }
            case 3: INFO("[Info]: [H4_USB]USB_REQ_SET_FEATURE\r\n"); break;
            case 5: { u8 address=(u8)WIRE16(&req->wValue); __musb_request_packet(hdl.ep,0,0); hdl.address=address; INFO("[Info]: [H4_USB]USB_REQ_SET_ADDRESS\r\n"); break; }
            case 6: { u16 len=WIRE16(&req->wLength), val=WIRE16(&req->wValue); const u8 *d; u16 n;
                u8 dtype=(u8)(val>>8);
                if (dtype==1) { d=device_descriptor; n=18; __musb_send_packet(hdl.ep,(u8 *)d,len<n?len:n); if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB]USB_DT_DEVICE\r\n"); log_print(2,0,"[Info]: [H4_USB]USB_REQ_GET_DESCRIPTOR - \r\n"); } break; }
                else if (dtype==2) { d=configuration_descriptor; n=177; __musb_send_packet(hdl.ep,(u8 *)d,len<n?len:n); if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB]USB_DT_CONFIG\r\n"); log_print(2,0,"[Info]: [H4_USB]USB_REQ_GET_DESCRIPTOR - \r\n"); } break; }
                else if (dtype==3) {
                    switch((u8)val) { case 0:d=string0_descriptor;n=*d;break; case 1:d=string1_descriptor;n=*d;break; case 2:d=string2_descriptor;n=*d;break; }
                    __musb_send_packet(hdl.ep,(u8 *)d,len<n?len:n);
                    if (log_tag_const_i_H4_USB) { log_print(2,0,"[Info]: [H4_USB]USB_DT_STRING\r\n"); log_print(2,0,"[Info]: [H4_USB]USB_REQ_GET_DESCRIPTOR - \r\n"); } break;
                } else { __musb_send_stall(hdl.ep); INFO("[Info]: [H4_USB]USB_REQ_GET_DESCRIPTOR - \r\n"); break; }
                INFO("[Info]: [H4_USB]USB_REQ_GET_DESCRIPTOR - \r\n"); break;
            }
            case 7: __musb_request_packet(hdl.ep,0,0); INFO("[Info]: [H4_USB]USB_REQ_SET_DESCRIPTOR\r\n"); break;
            case 8: hdl.ep[0].dma_size=0; __musb_ep_data_in(hdl.ep); INFO("[Info]: [H4_USB]USB_REQ_GET_CONFIGURATION\r\n"); break;
            case 9: __musb_request_packet(hdl.ep,0,0); INFO("[Info]: [H4_USB]USB_REQ_SET_CONFIGURATION\r\n"); break;
            case 10: hdl.ep[0].dma_size=0; __musb_ep_data_in(hdl.ep); INFO("[Info]: [H4_USB]USB_REQ_GET_INTERFACE\r\n"); break;
            case 11: { u16 idx=WIRE16(&req->wIndex); if(idx==1) {
                    __musb_request_packet(hdl.ep,0,0); u16 alt=WIRE16(&req->wValue); u8 frame=iso_alt_setting[alt];
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
        } else if (ep0_state==1) {
            if (csr&2) ERROR("<Error>: [H4_USB]CSR0P_TxPktRdy\r\n"); else __musb_ep_data_in(hdl.ep);
        } else if (ep0_state==2) {
            if (csr&1) __musb_ep_data_out(hdl.ep); else ERROR("<Error>: [H4_USB][DataOut]:CSR0P_RxPktRdy\r\n");
        }
        local_irq_disable(); USB_CSYNC(); hdl.state.counter=1; local_irq_enable();
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
        if (!hdl.ep[2].dma_size) { u16 n=WIRE16(hdl.ep[2].dma_buf + offsetof(struct hci_acl_data_packet, total_len)); hdl.ep[2].dma_size=n+4; if(n){hdl.ep[2].dst=hdl.bluetooth.acl;hdl.ep[2].state=1;} }
        __musb_ep_data_out(&hdl.ep[2]);
    }
    if (status&4) {
        __hw_write_usb(14,hdl.ep[4].number);
        if (!hdl.ep[4].dma_size) { u8 n=((struct hci_sco_data_packet *)hdl.ep[4].dma_buf)->total_len; hdl.ep[4].dma_size=n+3; if(n){hdl.ep[4].dst=hdl.bluetooth.sco;hdl.ep[4].state=1;} }
        __musb_ep_data_out(&hdl.ep[4]);
    }
}

#ifdef UNITTEST
#include "test.h"
int config_asser;
u8 log_tag_const_i_H4_USB, log_tag_const_e_H4_USB, log_tag_const_d_H4_USB;
struct thread_owner bt_task_thread;
const u8 device_descriptor[18]={18,1,2,3};
const u8 configuration_descriptor[177]={9,2,177};
const u8 string0_descriptor[4]={4,3,9,4};
const u8 string1_descriptor[4]={4,3,9,4};
const u8 string2_descriptor[24]={24,3,'J',0};
static u8 globals[16], indexed[4][32], selected;
static int writes, reads, irq_depth, max_irq_depth, resumed, suspended, created, irq_requests;
static int mask_off, mask_on, reset_count, assertion_count, busy_reads, timeout_mode, clock_calls;
static int delivered, types[8], lengths[8], callback_mutate;
static const u8 *delivered_data[8];
static u32 clock_time;
static void (*installed_irq)(void);
static u8 last_reg, last_value;
static int log_mode, log_calls, mutate_setup;
u8 usb_test_read(u8 r)
{
    ++reads;
    if(r==17 && selected && busy_reads) { --busy_reads; return 1; }
    if(r==17 && selected && timeout_mode) return 1;
    if(r==17 && selected) return indexed[selected][r]&~1;
    return r<16 ? globals[r] : indexed[selected][r];
}
void usb_test_write(u8 r,u8 v)
{
    ++writes; last_reg=r; last_value=v;
    if(r==14) { selected=v; CHECK(selected<4); }
    if(r<16) globals[r]=v; else indexed[selected][r]=v;
    if(mutate_setup && selected==0 && r==17 && v==72) ((struct usb_ctrlrequest *)(void *)hdl.ep0_dma)->wValue=5;
}
void local_irq_disable(void) { ++irq_depth; if(irq_depth>max_irq_depth) max_irq_depth=irq_depth; }
void local_irq_enable(void) { CHECK(irq_depth>0); --irq_depth; }
void bit_clr_ie(u8 n) { CHECK(n==9); ++mask_off; }
void bit_set_ie(u8 n) { CHECK(n==9); ++mask_on; }
void request_irq(u8 n,u8 priority,void (*fn)(void),u8 cpu)
{ CHECK(n==9 && priority==1 && !cpu); installed_irq=fn; ++irq_requests; }
int thread_create(struct thread *t,char *name,void (*fn)(struct thread *),struct thread_owner *owner)
{ CHECK(t==&hdl.thread && !strcmp(name,"h4_usb") && fn==h4_usbd_thread_process && owner==&bt_task_thread); ++created; return -1; }
void thread_suspend(struct thread *t,int timeout) { CHECK(t==&hdl.thread && !timeout); ++suspended; }
void thread_resume(struct thread *t) { CHECK(t==&hdl.thread); ++resumed; }
u32 sys_timer_get_time(void) { ++clock_calls; clock_time+=timeout_mode ? 2001 : 1; return clock_time; }
void log_print(int level,const char *tag,const char *fmt,...)
{
    (void)level; (void)tag; ++log_calls;
    if(log_mode==1) hdl.bluetooth.cmd_length=99;
    if(log_mode==2) CHECK(hdl.state.counter==0);
    if(log_mode==3) { log_tag_const_e_H4_USB=0; hdl.ep[5].frame_size=log_calls==1 ? 9 : 17; }
    if(log_mode==4) log_tag_const_i_H4_USB=0;
    if(log_mode==5) { hdl.bluetooth.in_fifos_w_index=7; hdl.bluetooth.sco_length=99; }
}
void printf_buf(u8 *p,int n) { (void)p; (void)n; }
void cpu_assert_debug(void) { ++assertion_count; }
void p33_soft_reset(void) { ++reset_count; }
static void capture(int type,const u8 *data,int n)
{
    CHECK(delivered<8); types[delivered]=type; delivered_data[delivered]=data; lengths[delivered++]=n;
    if(callback_mutate && type==3) hdl.bluetooth.in_fifos_w_index=hdl.bluetooth.in_fifos_r_index;
}
static void fixture(void)
{
    memset((void *)usb_test_regs,0,sizeof(usb_test_regs)); memset((void *)usb_test_io,0,sizeof(usb_test_io));
    memset(globals,0,sizeof(globals)); memset(indexed,0,sizeof(indexed)); selected=0;
    writes=reads=irq_depth=max_irq_depth=resumed=suspended=created=irq_requests=0;
    mask_off=mask_on=reset_count=assertion_count=busy_reads=timeout_mode=clock_calls=0;
    delivered=callback_mutate=log_mode=log_calls=mutate_setup=0; clock_time=0;
    config_asser=log_tag_const_i_H4_USB=log_tag_const_e_H4_USB=log_tag_const_d_H4_USB=0;
    usb_init(NULL); packet_handler=usb_dummy_handler;
    __musb_endpoint_init(0,0,0,64,0); __musb_endpoint_init(1,3,128,16,3);
    __musb_endpoint_init(2,1,0,64,2); __musb_endpoint_init(3,1,128,64,2);
    __musb_endpoint_init(4,2,0,63,1); __musb_endpoint_init(5,2,128,63,1);
}
static void setup(u8 type,u8 request,u16 value,u16 index,u16 length)
{
    struct usb_ctrlrequest *r=(void *)hdl.ep0_dma;
    r->bRequestType=type; r->bRequest=request; r->wValue=value; r->wIndex=index; r->wLength=length;
    globals[7]=globals[2]=1; globals[9]=globals[4]=0;
    indexed[0][17]=1; hdl.ep[0].state=0;
    usbd_irq_handler(); CHECK(hdl.state.counter==1 && !irq_depth);
}
int main(void)
{
    fixture(); const hci_transport_t *transport=hci_transport_usb_instance();
    CHECK(!strcmp(transport->name,"H2_MUSB") && transport->can_send_packet_now(4)==0);
    CHECK(transport->open()==0 && created==1 && irq_requests==1 && installed_irq==usbd_irq_handler);
    CHECK(usb_test_regs[7]==(u32)(uintptr_t)hdl.ep0_dma && usb_test_regs[9]==(u32)(uintptr_t)hdl.ep1_rx_dma && usb_test_regs[8]==(u32)(uintptr_t)hdl.ep1_tx_dma && usb_test_regs[11]==(u32)(uintptr_t)hdl.ep2_rx_dma && usb_test_regs[10]==(u32)(uintptr_t)hdl.ep2_tx_dma && usb_test_regs[12]==(u32)(uintptr_t)hdl.ep3_tx_dma);
    CHECK(transport->close()==0);
    u8 data[128]; for(int i=0;i<128;i++) data[i]=i;
    CHECK(transport->send_packet(4,data,4)==-1 && !reset_count);
    hdl.state.counter=1; indexed[3][17]=0;
    /* Short transfer completes in one packet, exact multiple needs final zero packet. */
    __musb_send_packet(&hdl.ep[1],data,15); CHECK(!hdl.ep[1].state && !hdl.ep[1].dma_size && usb_test_regs[5]==15);
    __musb_send_packet(&hdl.ep[1],data,16); CHECK(hdl.ep[1].state==1 && !hdl.ep[1].dma_size);
    __musb_ep_data_in(&hdl.ep[1]); CHECK(!hdl.ep[1].state && usb_test_regs[5]==0);
    /* Full blocking send must keep draining all chunks and reset its clock per chunk. */
    fixture(); hdl.state.counter=1;
    usbd_send_packet(&hdl.ep[1],data,40);
    CHECK(!hdl.ep[1].state && !hdl.ep[1].dma_size && hdl.ep[1].src==data+40 && clock_calls==3 && !memcmp(hdl.ep3_tx_dma,data+32,8));
    fixture(); hdl.state.counter=1; timeout_mode=1;
    usbd_send_packet(&hdl.ep[1],data,40); CHECK(!hdl.state.counter && hdl.ep[1].dma_size==24 && reset_count==0);
    /* Ring append has no fullness check and wraps at seven. */
    fixture(); hdl.state.counter=1; hdl.bluetooth.fifos_w_index=7;
    CHECK(usb_send_packet(3,data,6)==0 && !hdl.bluetooth.fifos_w_index && !memcmp(hdl.bluetooth.synchronous_fifos+7*147,data,6));
    CHECK(usb_send_packet(99,data,6)==-1 && reset_count==1);
    /* Command is deferred while pending==2; completion pauses EP0 until task delivers it. */
    fixture(); usb_register_packet_handler(capture); struct usb_ctrlrequest req={0}; req.wLength=4;
    bluetooth_class_packet_handler(&hdl.ep[0],&req); CHECK(hdl.bluetooth.cmd_pending.counter==2 && hdl.ep[0].state==2);
    h4_usbd_thread_process(&hdl.thread); CHECK(!delivered && suspended==0);
    memcpy(hdl.ep0_dma,data,4); selected=0; indexed[0][22]=4;
    __musb_ep_data_out(&hdl.ep[0]); CHECK(hdl.bluetooth.cmd_pending.counter==1 && hdl.bluetooth.cmd_length==4 && resumed==1);
    h4_usbd_thread_process(&hdl.thread); CHECK(delivered==1 && types[0]==1 && lengths[0]==4 && !memcmp(delivered_data[0],data,4) && hdl.bluetooth.cmd_pending.counter==0);
    /* ACL/SCO completion and read-index reload after callback. */
    fixture(); usb_register_packet_handler(capture); memcpy(hdl.ep1_rx_dma,data,8);
    __musb_request_packet(&hdl.ep[2],hdl.bluetooth.acl,8); selected=1; indexed[1][22]=8;
    __musb_ep_data_out(&hdl.ep[2]); CHECK(hdl.bluetooth.acl_pending.counter==1 && hdl.bluetooth.acl_length==8);
    h4_usbd_thread_process(&hdl.thread); CHECK(types[0]==2 && lengths[0]==8);
    __musb_request_packet(&hdl.ep[4],hdl.bluetooth.sco,6); memcpy(hdl.ep2_rx_dma,data,6); selected=2; indexed[2][22]=6;
    __musb_ep_data_out(&hdl.ep[4]); CHECK(hdl.bluetooth.sco_pending.counter==1 && hdl.bluetooth.in_fifos_w_index==1 && !memcmp(hdl.bluetooth.in_synchronous_fifos,data,6) && last_reg==20 && last_value==16);
    hdl.ep[4].frame_size=2; callback_mutate=1; h4_usbd_thread_process(&hdl.thread);
    CHECK(types[1]==3 && lengths[1]==6 && !hdl.bluetooth.sco_pending.counter);
    /* Reset endpoint map and standard/class setup dispatch. */
    fixture(); globals[11]=globals[6]=4; usbd_irq_handler();
    CHECK(hdl.usbd_class_packet_handler==bluetooth_class_packet_handler && hdl.usbd_vendor_packet_handler==vendor_class_packet_handler && !hdl.state.counter);
    CHECK(hdl.ep[1].number==3 && hdl.ep[1].direction==128 && hdl.ep[3].dma_buf==hdl.ep1_tx_dma && hdl.ep[5].dma_buf==hdl.ep2_tx_dma);
    globals[6]=globals[11]=0;
    setup(0x80,6,0x100,0,8); CHECK(usb_test_regs[2]==8 && !memcmp(hdl.ep0_dma,device_descriptor,8));
    setup(0,5,37,0,0); CHECK(hdl.address==37 && globals[0]==0);
    globals[7]=globals[2]=1; indexed[0][17]=0; usbd_irq_handler(); CHECK(!hdl.address && globals[0]==37);
    setup(0x20,0,0,0,3); CHECK(hdl.bluetooth.cmd_pending.counter==2 && hdl.ep[0].dma_size==3);
    setup(0x40,0,0,0,0); CHECK(indexed[0][17]&32);
    setup(1,11,2,1,0); CHECK(hdl.ep[4].frame_size==17 && hdl.ep[5].frame_size==17 && hdl.iso_frame_size==2);
    /* SDK TX-SCO callback advances ring index but addresses only the first slot. */
    globals[7]=globals[2]=4; indexed[0][17]=0; hdl.bluetooth.fifos_r_index=3; hdl.ep[5].frame_offset=1;
    memset(hdl.bluetooth.synchronous_fifos,0x11,147); memset(hdl.bluetooth.synchronous_fifos+3*147,0x33,147);
    usbd_irq_handler(); CHECK(hdl.ep2_tx_dma[0]==0x11 && hdl.ep[5].frame_offset==2 && hdl.bluetooth.fifos_r_index==3);
    CHECK(!irq_depth && mask_off==mask_on);
    /* Log callbacks can change mutable fields; preserve cached lengths and gates. */
    fixture(); usb_register_packet_handler(capture); log_tag_const_i_H4_USB=1; log_mode=1;
    hdl.bluetooth.cmd_pending.counter=1; hdl.bluetooth.cmd_length=4;
    h4_usbd_thread_process(&hdl.thread); CHECK(lengths[0]==4 && hdl.bluetooth.cmd_length==99);
    fixture(); log_tag_const_e_H4_USB=1; log_mode=2; timeout_mode=1; hdl.state.counter=1;
    usbd_send_packet(&hdl.ep[1],data,40); CHECK(log_calls==1 && clock_calls==3 && hdl.ep[1].dma_size==24);
    fixture(); log_tag_const_e_H4_USB=1; log_mode=3;
    setup(1,11,2,1,0); CHECK(log_calls==2 && hdl.ep[5].frame_size==17 && usb_test_regs[4]==17);
    fixture(); log_tag_const_i_H4_USB=1; log_mode=4;
    setup(0x80,6,0x100,0,8); CHECK(log_calls==2);
    fixture(); mutate_setup=1; setup(1,11,2,1,0); CHECK(hdl.ep[5].frame_size==49);
    fixture(); log_tag_const_i_H4_USB=1; log_mode=5;
    __musb_request_packet(&hdl.ep[4],hdl.bluetooth.sco,6); memcpy(hdl.ep2_rx_dma,data,6); selected=2; indexed[2][22]=6;
    __musb_ep_data_out(&hdl.ep[4]);
    CHECK(!hdl.bluetooth.in_fifos_w_index && !memcmp(hdl.bluetooth.in_synchronous_fifos+7*147,data,6));
    fixture(); usb_register_packet_handler(capture); hdl.ep[4].frame_size=100; hdl.bluetooth.sco_pending.counter=1;
    h4_usbd_thread_process(&hdl.thread); CHECK(delivered==1 && lengths[0]==44);
    fixture(); hdl.bluetooth.cmd_pending.counter=256; h4_usbd_thread_process(&hdl.thread); CHECK(suspended==1);
    return TEST_DONE("btctrler_bd19_h4_usb_interface");
}
#endif
