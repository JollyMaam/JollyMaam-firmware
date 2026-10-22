"""The known-device table: stock pads by USB identity, in each mode."""
import unittest

import padknown as known

KEYBOARD = bytes.fromhex("05010906a1018503050719e029e715002501750195088102c0")
CYCLONE_GREEN = KEYBOARD + bytes.fromhex("06f0ff0940a10185100600ff")


def hid(vid, product, bcd, descriptor=b"", manufacturer="", name="", location="5-2.3", path="h"):
    return {"path": path, "vid": vid, "product": product, "bcd": bcd, "manufacturer": manufacturer,
            "product_name": name, "location": location, "descriptor": descriptor}


class StockTests(unittest.TestCase):
    def test_usage_page_found_after_other_collections(self):
        self.assertTrue(known.has_usage_page(CYCLONE_GREEN, 0xfff0))
        self.assertFalse(known.has_usage_page(KEYBOARD, 0xfff0))
        self.assertFalse(known.has_usage_page(bytes([0xfe, 2, 0, 0x06, 0xf0]) , 0xfff0))

    def test_each_mode_of_each_pad(self):
        cases = [
            (hid(0x3537, 0x1053, 0x0352, CYCLONE_GREEN), "GS_C2_ADC_DEVICE", "Xbox 360 (green)", True),
            (hid(0x057e, 0x2009, 0x0352, name="Gamepad"), "GS_C2_ADC_DEVICE", "Switch (red)", False),
            (hid(0x054c, 0x09cc, 0x0352, manufacturer="Guangzhou Chicken Run Network Technology Co., Ltd.",
                 name="Wireless Controller"), "GS_C2_ADC_DEVICE", "DS4 (blue)", False),
            (hid(0x3537, 0x100f, 0x0100, bytes.fromhex("067aff0901a101")), "AC695X_TRANS", "Xbox 360 (green)", True),
            (hid(0x057e, 0x2009, 0x0200, manufacturer="Zikway", name="Pro Controller"),
             "AC695X_TRANS", "Switch (red)", False),
            (hid(0x054c, 0x09cc, 0x0100, manufacturer="Sony Computer Entertainment",
                 name="Wireless Controller"), "AC695X_TRANS", "DS4 (blue)", False),
        ]
        for device, pid, mode, command in cases:
            entry = known.match_stock(device)
            self.assertEqual((entry.pid, entry.mode, bool(entry.usage_page)), (pid, mode, command))

    def test_lookalikes_are_not_stock(self):
        self.assertIsNone(known.match_stock(hid(0x3537, 0x1053, 0x0352, KEYBOARD)))      # its keyboard
        self.assertIsNone(known.match_stock(hid(0x3537, 0x1053, 0x0353, CYCLONE_GREEN)))  # another version
        self.assertIsNone(known.match_stock(hid(0x057e, 0x2009, 0x0200, manufacturer="Nintendo Co., Ltd.",
                                                name="Pro Controller")))
        self.assertIsNone(known.match_stock(hid(0x054c, 0x09cc, 0x0100, manufacturer="Sony Interactive Entertainment",
                                                name="Wireless Controller")))

    def test_windows_collection_matches_by_its_usage_page_and_length(self):
        device = hid(0x3537, 0x100f, 0x0100)
        del device["descriptor"]
        self.assertIsNone(known.match_stock(dict(device, usage_page=0x0001, out_len=2)))
        self.assertEqual(known.match_stock(dict(device, usage_page=0xff7a, out_len=65)).pid, "AC695X_TRANS")

    def test_one_device_per_usb_device_with_the_command_interface(self):
        found = known.stock_devices([hid(0x3537, 0x100f, 0x0100, bytes.fromhex("05010906"), location="5-2.1", path="kbd"),
                                     hid(0x3537, 0x100f, 0x0100, bytes.fromhex("067aff09"), location="5-2.1", path="vendor"),
                                     hid(0x3537, 0x2106, 0x0100, manufacturer="Zikway", name="HID zk",
                                         location="5-2.4", path="dongle")])
        self.assertEqual(sorted(h["path"] for h, e in found), ["dongle", "vendor"])
        dongle = next(e for h, e in found if h["path"] == "dongle")
        self.assertEqual((dongle.pid, known.stock_name(dongle)), ("AC632N_TRANS", "GameSir Nova 2 Lite dongle"))

    def test_every_stock_device_has_a_product_and_a_way_into_its_loader(self):
        for entry in known.STOCK:
            self.assertIn(entry.pid, known.PRODUCTS)
            self.assertTrue(known.command_mode(entry.pid) or known.PRODUCTS[entry.pid].by_hand)
            if entry.usage_page:
                self.assertTrue(1 < len(entry.report) <= entry.length <= 65)


if __name__ == "__main__":
    unittest.main()
