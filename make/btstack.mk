# Upstream BTstack, unmodified, built from source.  Some symbols are renamed
# at build time to avoid clashing with the Jieli SDK's controller.

BTSTACK := /shared/github.com-bluekitchen-btstack
BTSTACK_USED :=
BTSTACK_USED += src/btstack_hid_parser.c
BTSTACK_USED += src/btstack_linked_list.c
BTSTACK_USED += src/btstack_memory.c
BTSTACK_USED += src/btstack_memory_pool.c
BTSTACK_USED += src/btstack_run_loop.c
BTSTACK_USED += src/btstack_run_loop_base.c
BTSTACK_USED += src/btstack_crypto.c
BTSTACK_USED += src/btstack_tlv.c
BTSTACK_USED += src/btstack_util.c
BTSTACK_USED += src/hci.c
BTSTACK_USED += src/hci_cmd.c
BTSTACK_USED += src/hci_event.c
BTSTACK_USED += src/hci_dump.c
BTSTACK_USED += src/l2cap.c
BTSTACK_USED += src/l2cap_signaling.c
BTSTACK_USED += src/ad_parser.c
BTSTACK_USED += src/classic/device_id_server.c
BTSTACK_USED += src/classic/hid_device.c
BTSTACK_USED += src/classic/sdp_server.c
BTSTACK_USED += src/classic/sdp_util.c
BTSTACK_USED += src/ble/att_db.c
BTSTACK_USED += src/ble/att_dispatch.c
BTSTACK_USED += src/ble/att_server.c
BTSTACK_USED += src/ble/le_device_db_memory.c
BTSTACK_USED += src/ble/sm.c

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
