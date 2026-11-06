# Upstream BTstack, unmodified, built from source.  Some symbols are renamed
# at build time to avoid clashing with the Jieli SDK's controller.

BTSTACK := external/bluekitchen-btstack
# BTstack's sources, as units: an image adds the ones it uses.
# Every host: HCI, L2CAP, the run loop and utilities.
BTSTACK_CORE_SOURCES :=
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/btstack_linked_list.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/btstack_memory.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/btstack_memory_pool.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/btstack_run_loop.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/btstack_run_loop_base.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/btstack_tlv.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/btstack_util.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/hci.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/hci_cmd.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/hci_event.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/hci_dump.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/l2cap.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/l2cap_signaling.c
BTSTACK_CORE_SOURCES += $(BTSTACK)/src/ad_parser.c
# LE: the ATT server.
BTSTACK_LE_SOURCES :=
BTSTACK_LE_SOURCES += $(BTSTACK)/src/ble/att_db.c
BTSTACK_LE_SOURCES += $(BTSTACK)/src/ble/att_dispatch.c
BTSTACK_LE_SOURCES += $(BTSTACK)/src/ble/att_server.c
BTSTACK_LE_SOURCES += $(BTSTACK)/src/ble/le_device_db_memory.c
# LE: the security manager.
BTSTACK_SM_SOURCES :=
BTSTACK_SM_SOURCES += $(BTSTACK)/src/ble/sm.c
BTSTACK_SM_SOURCES += $(BTSTACK)/src/btstack_crypto.c
# Classic: HID device, SDP, Device ID.
BTSTACK_CLASSIC_SOURCES :=
BTSTACK_CLASSIC_SOURCES += $(BTSTACK)/src/btstack_hid_parser.c
BTSTACK_CLASSIC_SOURCES += $(BTSTACK)/src/classic/device_id_server.c
BTSTACK_CLASSIC_SOURCES += $(BTSTACK)/src/classic/hid_device.c
BTSTACK_CLASSIC_SOURCES += $(BTSTACK)/src/classic/sdp_server.c
BTSTACK_CLASSIC_SOURCES += $(BTSTACK)/src/classic/sdp_util.c
# Classic and LE with pairing (the pads and dongles).
BTSTACK_SOURCES := $(BTSTACK_CORE_SOURCES) $(BTSTACK_LE_SOURCES) $(BTSTACK_SM_SOURCES) $(BTSTACK_CLASSIC_SOURCES)

BTSTACK_RENAMES :=
BTSTACK_RENAMES += -Dhci_send_cmd=btstack_hci_send_cmd
BTSTACK_RENAMES += -Dhci_event_disconnection_complete=btstack_hci_event_disconnection_complete
BTSTACK_RENAMES += -Dbig_endian_read_32=btstack_big_endian_read_32
BTSTACK_RENAMES += -Dbig_endian_store_32=btstack_big_endian_store_32
BTSTACK_RENAMES += -Dnibble_for_char=btstack_nibble_for_char

BTSTACK_INCLUDES :=
BTSTACK_INCLUDES += -I$(BTSTACK)/src
BTSTACK_INCLUDES += -I$(BTSTACK)/src/classic
# Our files that call BTstack, built with its renames; a product adds its own.
BTSTACK_USERS := src/platform/jieli/btstack_hci_jieli.c
