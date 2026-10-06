#ifndef BTCTRLR_SDK_ABI_H
#define BTCTRLR_SDK_ABI_H

/* Retained implementation uses BTCTRLER_BIN_<name> with an assembler symbol
 * binding: no forwarding wrapper and no hidden runtime cost. Owned C entry
 * points keep normal names; BTCTRLER_ABI_* is generated typed layout glue.
 * Recovered BR23 controller ABI. Connection storage belongs to the SDK and
 * pointers are used only during a controller-thread call. Layout access is
 * generated from checked LLVM types, never duplicated as a C struct here. */
struct lmp_connection;

/* Retained btctrler.a implementation. */
struct lmp_connection *BTCTRLER_BIN_lmp_conn_for_handle(int handle) __asm__("lmp_conn_for_handle");
void BTCTRLER_BIN_route_selected(struct lmp_connection *, int *message);
void BTCTRLER_BIN_lmp_hci_sniff_mode(struct lmp_connection *, unsigned short max,
                       unsigned short min, unsigned short attempt, unsigned short timeout) __asm__("lmp_hci_sniff_mode");
void BTCTRLER_BIN_lmp_hci_exit_sniff_mode(struct lmp_connection *) __asm__("lmp_hci_exit_sniff_mode");
void BTCTRLER_BIN_lmp_hci_role_discovery(unsigned short handle) __asm__("lmp_hci_role_discovery");
void BTCTRLER_BIN_lmp_hci_switch_role_command(unsigned char *address, unsigned char role) __asm__("lmp_hci_switch_role_command");
void BTCTRLER_BIN_lmp_hci_read_link_policy_settings(unsigned short handle) __asm__("lmp_hci_read_link_policy_settings");
void BTCTRLER_BIN_lmp_hci_write_link_policy_settings(unsigned short handle, unsigned short policy) __asm__("lmp_hci_write_link_policy_settings");
int BTCTRLER_BIN_hci_send_event(int event, const char *format, ...) __asm__("hci_send_event");
int BTCTRLER_BIN_hci_send_event_cmd_complete(int opcode, const char *format, ...) __asm__("hci_send_event_cmd_complete");
int BTCTRLER_BIN_lmp_response(struct lmp_connection *, const char *format, ...) __asm__("lmp_response");
int BTCTRLER_BIN_lmp_request(struct lmp_connection *, const char *format, ...) __asm__("lmp_request");

/* Retained Classic command helpers. Inquiry/eSCO feature flags are owned
 * by platform_jieli_sdk.c; the helpers below are binary implementation. */
struct link_inquiry_result;
void BTCTRLER_BIN_link_page_try_start_enable(unsigned char type, unsigned char *address,
    unsigned char repetition, int timeout) __asm__("link_page_try_start_enable");
int BTCTRLER_BIN_link_inquiry_enable(unsigned char *lap, int duration_ms,
    unsigned char max_responses, int (*result)(struct link_inquiry_result *)) __asm__("link_inquiry_enable");
void BTCTRLER_BIN_link_inquiry_disable(void) __asm__("link_inquiry_disable");
void BTCTRLER_BIN_link_page_disable(int reason) __asm__("link_page_disable");
void BTCTRLER_BIN_lmp_hci_cmd_handler_to_conn(unsigned char *parameters, int *message) __asm__("lmp_hci_cmd_handler_to_conn");
void BTCTRLER_BIN_lmp_hci_read_remote_supported_features(unsigned short handle) __asm__("lmp_hci_read_remote_supported_features");
void BTCTRLER_BIN_lmp_hci_read_remote_extended_features(unsigned short handle) __asm__("lmp_hci_read_remote_extended_features");
void BTCTRLER_BIN_lmp_hci_remote_name_request(unsigned char *address, unsigned char repetition,
    unsigned short clock_offset) __asm__("lmp_hci_remote_name_request");
void BTCTRLER_BIN_lmp_hci_read_clock_offset(unsigned short handle) __asm__("lmp_hci_read_clock_offset");
void BTCTRLER_BIN_lmp_hci_setup_sync_connection(unsigned short handle,
    unsigned int transmit_bw, unsigned int receive_bw, unsigned short max_latency,
    unsigned short voice, unsigned char retransmission, unsigned short packet_type) __asm__("lmp_hci_setup_sync_connection");

/* Retained command-group handlers and error completion fragment. */
int BTCTRLER_BIN_hci_link_control_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_link_control_cmd_handler");
int BTCTRLER_BIN_hci_link_policy_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_link_policy_cmd_handler");
int BTCTRLER_BIN_hci_lmp_informational_parameters_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_lmp_informational_parameters_cmd_handler");
int BTCTRLER_BIN_hci_status_parameters_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_status_parameters_cmd_handler");
int BTCTRLER_BIN_hci_lmp_status_parameters_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_lmp_status_parameters_cmd_handler");
int BTCTRLER_BIN_hci_testing_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_testing_cmd_handler");
int BTCTRLER_BIN_hci_lmp_testing_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_lmp_testing_cmd_handler");
int BTCTRLER_BIN_hci_le_controller_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_le_controller_cmd_handler");
int BTCTRLER_BIN_hci_vendor_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_vendor_cmd_handler");
int BTCTRLER_BIN_hci_lmp_vendor_cmd_handler(unsigned short opcode, unsigned char *parameters, int length) __asm__("hci_lmp_vendor_cmd_handler");
int BTCTRLER_BIN_ogf_vendor_le_controller(unsigned short opcode, unsigned char *parameters, int length) __asm__("ogf_vendor_le_controller");
void BTCTRLER_BIN_finish_command(int opcode, int result, _Bool bad_group, unsigned char *packet, int size);

void BTCTRLER_BIN_lmp_reset(void) __asm__("lmp_reset");
void BTCTRLER_BIN_ll_reset(void) __asm__("ll_reset");
void BTCTRLER_BIN_le_dut_hci_reset_handle(void) __asm__("le_dut_hci_reset_handle");

/* Typed SDK storage access, generated from LLVM layout. */
struct lmp_connection *BTCTRLER_ABI_acl_slot(void);
unsigned short BTCTRLER_ABI_connection_handle(struct lmp_connection *);
int BTCTRLER_ABI_connection_has_link(struct lmp_connection *);
unsigned char *BTCTRLER_ABI_connection_address(struct lmp_connection *);
unsigned char BTCTRLER_ABI_default_policy_get(void);
void BTCTRLER_ABI_default_policy_set(unsigned char policy);
unsigned char BTCTRLER_ABI_connection_role(struct lmp_connection *);
unsigned int BTCTRLER_ABI_connection_state(struct lmp_connection *);
void BTCTRLER_ABI_connection_state_set(struct lmp_connection *, unsigned int state);
unsigned char BTCTRLER_ABI_connection_step(struct lmp_connection *);
unsigned short BTCTRLER_ABI_supervision_ms(void);

unsigned char *BTCTRLER_ABI_event_mask(void);
unsigned char *BTCTRLER_ABI_local_commands(void);
unsigned char *BTCTRLER_ABI_classic_address(void);
unsigned char *BTCTRLER_ABI_local_features(void);
unsigned char *BTCTRLER_ABI_local_extended_features(void);
unsigned char *BTCTRLER_ABI_le_address(void);
unsigned char BTCTRLER_ABI_hci_version(void);
unsigned short BTCTRLER_ABI_hci_revision(void);
unsigned char BTCTRLER_ABI_lmp_version(void);
unsigned short BTCTRLER_ABI_manufacturer(void);
unsigned short BTCTRLER_ABI_lmp_subversion(void);

void BTCTRLER_BIN_lmp_scan_enable(unsigned char enable) __asm__("lmp_scan_enable");
void BTCTRLER_BIN_lmp_hci_write_local_name(unsigned char *name) __asm__("lmp_hci_write_local_name");
int BTCTRLER_BIN_lmp_hci_refresh_encryption_key(unsigned short handle) __asm__("lmp_hci_refresh_encryption_key");
unsigned char *BTCTRLER_ABI_classic_flags(void);
unsigned char *BTCTRLER_ABI_classic_name(void);
unsigned short *BTCTRLER_ABI_classic_page_timeout(void);
unsigned short *BTCTRLER_ABI_classic_inquiry_interval(void);
unsigned short *BTCTRLER_ABI_classic_inquiry_window(void);
unsigned short *BTCTRLER_ABI_classic_page_interval(void);
unsigned short *BTCTRLER_ABI_classic_page_window(void);
unsigned short *BTCTRLER_ABI_classic_accept_timeout(void);
unsigned short *BTCTRLER_ABI_classic_voice(void);
unsigned int *BTCTRLER_ABI_classic_class(void);
unsigned char *BTCTRLER_ABI_classic_iac(void);
unsigned char *BTCTRLER_ABI_classic_num_iac(void);
unsigned char *BTCTRLER_ABI_classic_iac_count(void);
unsigned char *BTCTRLER_ABI_classic_eir_length(void);
unsigned char *BTCTRLER_ABI_classic_eir_packet_type(void);
unsigned char **BTCTRLER_ABI_classic_eir_buffer(void);
void BTCTRLER_ABI_host_buffer_sizes_set(unsigned short acl, unsigned short sco, unsigned short acl_count, unsigned short sco_count);

/* Owned C entry points and trace callback. */
int hci_lmp_controller_baseband_cmd_handler(unsigned short opcode, unsigned char *parameters, int length);
int hci_informational_parameters_cmd_handler(unsigned short opcode, unsigned char *parameters, int length);
int hci_controller_baseband_cmd_handler(unsigned short opcode, unsigned char *parameters, int length);
void hci_command_process(unsigned char *packet, int size);
int hci_lmp_link_control_cmd_handler(unsigned short opcode, unsigned char *parameters, int length);
int hci_lmp_link_policy_cmd_handler(unsigned short opcode, unsigned char *parameters, int length);
void lmp_hci_cmd_addr_to_conn(unsigned char *parameters, int *message);
struct lmp_connection *lmp_conn_for_address(const unsigned char *address);
void lmp_vendor_qos_interval_config(unsigned short slots);
void lmp_tx_qos_req(struct lmp_connection *);
void lmp_event_esco_link_setup(struct lmp_connection *);
void cyclone2_bringup_record(unsigned int code);

/* SDK CPU runtime, outside btctrler.a. */
void local_irq_disable(void);
void local_irq_enable(void);

#endif
