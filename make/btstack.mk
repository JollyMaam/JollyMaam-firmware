# Upstream BTstack, unmodified, built from source.  Some symbols are renamed
# at build time to avoid clashing with the Jieli SDK's controller.

BTSTACK := /shared/github.com-bluekitchen-btstack
BTSTACK_USED := \
	src/btstack_hid_parser.c src/btstack_linked_list.c src/btstack_memory.c \
	src/btstack_memory_pool.c src/btstack_run_loop.c src/btstack_run_loop_base.c \
	src/btstack_crypto.c src/btstack_tlv.c src/btstack_util.c src/hci.c src/hci_cmd.c \
	src/hci_event.c src/hci_dump.c src/l2cap.c src/l2cap_signaling.c src/ad_parser.c \
	src/classic/device_id_server.c src/classic/hid_device.c src/classic/sdp_server.c \
	src/classic/sdp_util.c src/ble/att_db.c src/ble/att_dispatch.c src/ble/att_server.c \
	src/ble/le_device_db_memory.c src/ble/sm.c
BTSTACK_RENAMES := -Dhci_send_cmd=btstack_hci_send_cmd \
	-Dhci_event_disconnection_complete=btstack_hci_event_disconnection_complete \
	-Dbig_endian_read_32=btstack_big_endian_read_32 \
	-Dbig_endian_store_32=btstack_big_endian_store_32 \
	-Dnibble_for_char=btstack_nibble_for_char
BTSTACK_INCLUDES := -I$(BTSTACK)/src -I$(BTSTACK)/src/classic
# Our files that call BTstack: built with its renames.
BTSTACK_USERS := src/bt/btstack/bt.c src/dongle/dongle_bt.c src/platform/jieli/btstack_hci_jieli.c
