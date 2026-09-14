#ifndef APP_MAIN_H
#define APP_MAIN_H
typedef struct _APP_VAR {
    s8 music_volume;
    s8 call_volume;
    s8 wtone_volume;
    u8 opid_play_vol_sync; /* unused? */
    u8 aec_dac_gain;
    u8 aec_mic_gain;
    u8 rf_power; // unused?
    u8 goto_poweroff_flag; // unused?
    u8 goto_poweroff_cnt; // unused?
    u8 play_poweron_tone; // unused?
    u8 remote_dev_company; // unused?
    u8 siri_stu; // unused?
    int auto_stop_page_scan_timer; // unused?
    volatile int auto_shut_down_timer; // unused?
    volatile int wait_exit_timer; // unused?
    u16 auto_off_time; // unused?
    u16 warning_tone_v; // unused?
    u16 poweroff_tone_v; // unused?
    u32 start_time; // unused?
    s8  usb_mic_gain; // unused?
} APP_VAR;

extern APP_VAR app_var;
#endif
