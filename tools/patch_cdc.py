#!/usr/bin/env python3
"""Apply the small CDC fixes required by the Cyclone 2 console.

The SDK treats SET_CONTROL_LINE_STATE as a one-way latch: once a host has
opened the tty it never clears DTR/RTS.  That makes later writes target an
unattended bulk endpoint after c2ctl exits.  Keep the SDK source pristine and
make the behavior correct in the private build copy.
"""

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise SystemExit(f"expected exactly one CDC patch site, found {text.count(old)}")
    return text.replace(old, new)


if len(sys.argv) != 3:
    raise SystemExit(f"usage: {sys.argv[0]} INPUT OUTPUT")

source = pathlib.Path(sys.argv[1]).read_text()

old_line_state = """            if (cdc_hdl) {
                /* if (ctrl_req->wValue & BIT(0)) { //DTR */
                cdc_hdl->bmTransceiver |= BIT(0);
                /* } else { */
                /* usb_slave->cdc->bmTransceiver &= ~BIT(0); */
                /* } */
                /* if (ctrl_req->wValue & BIT(1)) { //RTS */
                cdc_hdl->bmTransceiver |= BIT(1);
                /* } else { */
                /* usb_slave->cdc->bmTransceiver &= ~BIT(1); */
                /* } */
                cdc_hdl->bmTransceiver |= BIT(4);  //cfg done
            }
"""
new_line_state = """            if (cdc_hdl) {
                /* Mirror the host's DTR/RTS state instead of latching it. */
                cdc_hdl->bmTransceiver =
                    (cdc_hdl->bmTransceiver & ~(BIT(0) | BIT(1))) |
                    (ctrl_req->wValue & (BIT(0) | BIT(1))) | BIT(4);
            }
"""
source = replace_once(source, old_line_state, new_line_state)

source = replace_once(
    source,
    "            cdc_endpoint_init(usb_device, (ctrl_req->wIndex & USB_RECIP_MASK));\n",
    "",
)

old_wakeup = """void cdc_set_wakeup_handler(void (*handle)(struct usb_device_t *usb_device))
{
    if (cdc_hdl) {
        cdc_hdl->wakeup_handler = handle;
    }
}
"""
new_wakeup = old_wakeup + """
u8 cyclone2_cdc_terminal_open(void)
{
    const u8 required = BIT(0) | BIT(1) | BIT(4);
    return cdc_hdl && (cdc_hdl->bmTransceiver & required) == required;
}
"""
source = replace_once(source, old_wakeup, new_wakeup)

pathlib.Path(sys.argv[2]).write_text(source)
