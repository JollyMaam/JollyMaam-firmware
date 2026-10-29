/*
 * BD19 tws_esco.c.o occurrence 1 recovery checkpoint.
 *
 * The small state helpers below are transcribed from the pinned BD19 IR.
 * The remaining member bodies are intentionally not represented yet; this
 * file must not be admitted to bd19.json until the full symbol boundary and
 * the hardware/timer paths have been recovered.
 */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS_ESCO;
void log_print(int level, const char *tag, const char *format, ...);

#define TWS_ESCO_CODE __attribute__((section(".classic_tws_code.esco")))

/* BD19 IR lines 161–187. Keep the separate 1/2 request cases and state
 * guard: this is not equivalent to treating request as a boolean. */
void tws_esco_tx_requset(struct bredr_esco_link *esco, u8 request,
                         u8 acl_resume_t) TWS_ESCO_CODE;
void tws_esco_tx_requset(struct bredr_esco_link *esco, u8 request,
                         u8 acl_resume_t)
{
    if (!esco)
        return;
    if (request == 1) {
        if (esco->esco_role_switch_state != 2) {
            esco->esco_acl_resume_t = acl_resume_t;
            esco->esco_role_switch_state = 2;
        }
    } else if (request == 2) {
        if (esco->esco_role_switch_state != 4) {
            esco->esco_acl_resume_t = acl_resume_t;
            esco->esco_role_switch_state = 4;
        }
    }
}

/* BD19 IR lines 191–212. The strict >6 test and byte subtraction are SDK
 * behavior, including wrap for a zero request value in the accepted path. */
void tws_esco_rx_requset(struct bredr_esco_link *esco, u8 request,
                         u8 acl_resume_t) TWS_ESCO_CODE;
void tws_esco_rx_requset(struct bredr_esco_link *esco, u8 request,
                         u8 acl_resume_t)
{
    if (!esco)
        return;
    if (request == 1) {
        if (acl_resume_t > 6 && esco->esco_role_switch_state != 3) {
            esco->esco_role_switch_state = 3;
            esco->esco_acl_resume_t = (u8)(acl_resume_t - 1);
            if (log_tag_const_i_TWS_ESCO)
                log_print(2, NULL,
                          "[Info]: [TWS_ESCO]slave_role_switch_resume_acl\n\r\n");
        }
    } else if (request == 2 && esco->esco_role_switch_state != 5) {
        esco->esco_role_switch_state = 5;
        esco->esco_acl_resume_t = (u8)(acl_resume_t - 1);
        if (log_tag_const_i_TWS_ESCO)
            log_print(2, NULL,
                      "[Info]: [TWS_ESCO]tws_esco_resume_acl\n\r\n");
    }
}

/* Weak SDK default, BD19 IR line 216. */
u8 get_esco_packet_dump(void) __attribute__((weak, section(".classic_tws_code.esco")));
u8 get_esco_packet_dump(void)
{
    return 120;
}

/* The remaining 21 public and six private definitions are pending. */
