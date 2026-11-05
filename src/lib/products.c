#include "jollymaam.h"
#include <string.h>

/* Every product: our firmware's image PID, its build directory and what the
 * vendor loader writes from it, the chip, the 8BitDo loader's model, flash
 * the vendor flash keeps (the Nova pad's stock data), and how to enter the
 * vendor loader by hand.  docs/<product>/ for where these come from. */
static const struct jm_product products[] = {
  {"GS_C2_ADC_DEVICE", "gamesir-cyclone-2-pad", "Cyclone 2", JM_CHIP_BR23, 0, "firmware.img", 1, 0, 0,
   "stock firmware: Xbox 360 mode (green), or hold Home+Start while plugging in USB"},
  {"GS_C2_Dongle", "gamesir-cyclone-2-dongle", "Cyclone 2 Dongle", JM_CHIP_BR23, 0, "firmware.img", 1, 0, 0,
   "hold the button while plugging in USB"},
  {"AC695X_TRANS", "gamesir-nova-2-lite-pad", "Nova 2 Lite", JM_CHIP_BR23, 0, "firmware.img", 1, 0x64000, 0x1c000,
   "stock firmware: Xbox 360 mode (green)"},
  {"AC632N_TRANS", "gamesir-nova-2-lite-dongle", "Nova 2 Lite Dongle", JM_CHIP_BD19, 0, "firmware.img", 1, 0, 0,
   "hold the pairing button while connecting USB"},
  {"U2C_WIRELESS", "8bitdo-ultimate2c-wireless-pad", "Ultimate 2C Wireless", JM_CHIP_TLSR8208, 0x301b,
   "firmware.img", 1, 0, 0, "USB unplugged, hold L1+R1, tap Home, plug in USB"},
  {"U2C_RECEIVER", NULL, "Ultimate 2C Receiver", JM_CHIP_TLSR8208, 0x301c, NULL, 0, 0, 0, NULL},
  {"U2C_BLUETOOTH", "8bitdo-ultimate2c-bluetooth-pad", "Ultimate 2C Bluetooth", JM_CHIP_YC3121, 0x301a,
   "firmware.img", 1, 0, 0, "USB unplugged, hold L1+R1, tap Home, plug in USB"},
};

static const struct jm_stock stock[] = {
  /* docs/gamesir-cyclone2/stock-pad-dongle.md (3.52) */
  {"GS_C2_ADC_DEVICE", "Xbox 360 (green)", 0x3537, 0x1053, 0x0352, NULL, NULL, 0xfff0, 64,
   {0x0f, 0x17, 0x55, 0x88, 0x00}, 0},
  {"GS_C2_ADC_DEVICE", "Switch (red)", 0x057e, 0x2009, 0x0352, NULL, "Gamepad", 0, 0, {0}, 0},
  {"GS_C2_ADC_DEVICE", "DS4 (blue)", 0x054c, 0x09cc, 0x0352,
   "Guangzhou Chicken Run Network Technology Co., Ltd.", NULL, 0, 0, {0}, 0},
  /* docs/gamesir-nova-2-lite/ */
  {"AC695X_TRANS", "Xbox 360 (green)", 0x3537, 0x100f, 0x0100, NULL, NULL, 0xff7a, 65,
   {0x00, 0xf2, 0x00, 0x04, 0xf6}, 0},
  {"AC695X_TRANS", "Switch (red)", 0x057e, 0x2009, 0x0200, "Zikway", "Pro Controller", 0, 0, {0}, 0},
  {"AC695X_TRANS", "DS4 (blue)", 0x054c, 0x09cc, 0x0100, "Sony Computer Entertainment",
   "Wireless Controller", 0, 0, {0}, 0},
  {"AC632N_TRANS", "", 0x3537, 0x2106, 0x0100, "Zikway", "HID zk", 0, 0, {0}, 0},
  /* docs/8bitdo-ultimate2c-wireless/: stock's vendor interface takes the
   * loader's commands on output report 0x81; 0x51 restarts it. */
  {NULL, "", 0x2dc8, 0x310a, 0, NULL, NULL, 1, 64, {0x81, 0x05, 0x00, 0x51, 0x00}, 1},
  {NULL, "", 0x2dc8, 0x301b, 0, NULL, NULL, 1, 64, {0x81, 0x05, 0x00, 0x51, 0x00}, 1},
  {NULL, "", 0x2dc8, 0x301c, 0, NULL, NULL, 1, 64, {0x81, 0x05, 0x00, 0x51, 0x00}, 1},
  {NULL, "", 0x2dc8, 0x301d, 0, NULL, NULL, 1, 64, {0x81, 0x05, 0x00, 0x51, 0x00}, 1},
};

const struct jm_stock *jm_stock_at(size_t index) {
  return index < sizeof stock / sizeof stock[0] ? &stock[index] : NULL;
}

const struct jm_product *jm_product(const char *pid) {
  for (size_t i = 0; pid && i < sizeof products / sizeof products[0]; i++)
    if (!strcmp(products[i].pid, pid))
      return &products[i];
  return NULL;
}

const struct jm_product *jm_product_at(size_t index) {
  return index < sizeof products / sizeof products[0] ? &products[index] : NULL;
}

const char *jm_chip_name(enum jm_chip chip) {
  static const char *const names[] = {"BR23", "BD19", "TLSR8208", "YC3121"};
  return (unsigned)chip < 4 ? names[chip] : "?";
}
