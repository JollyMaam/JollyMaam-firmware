/* Link/runtime smoke test; version queries do not open USB devices. */
#include <hidapi.h>
#include <libusb.h>
#include <zlib.h>
#include <stdio.h>

int main(void) {
  const struct libusb_version *usb = libusb_get_version();
  const struct hid_api_version *hid = hid_version();
  const char *z = zlibVersion();
  if (!usb || !hid || !z) return 1;
  printf("libusb %u.%u.%u, hidapi %d.%d.%d, zlib %s\n",
         usb->major, usb->minor, usb->micro,
         hid->major, hid->minor, hid->patch, z);
  return 0;
}
