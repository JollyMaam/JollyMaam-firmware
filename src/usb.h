#ifndef CYCLONE2_USB_CONTROL_H
#define CYCLONE2_USB_CONTROL_H

void cyclone2_usb_control_initialize(void);
void cyclone2_usb_control_poll(void);
void cyclone2_usb_debug(const char *format, ...);

#endif
