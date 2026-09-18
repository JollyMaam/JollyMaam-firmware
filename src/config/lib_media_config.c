/*********************************************************************************************
    *   Filename        : lib_driver_config.c

    *   TRANSLATED: Description: code and RAM optimization settings.

    *   Author          : Bingquan

    *   Email           : caibingquan@zh-jieli.com

    *   Last modifiled  : 2019-03-18 14:58

    *   Copyright:(c)JIELI  2011-2019  @ , All Rights Reserved.
*********************************************************************************************/
#include "app_config.h"
#include "system/includes.h"
#include "media/includes.h"

const u8 audio_decoder_occupy_trace_enable = 0;
const u8 audio_decoder_occupy_trace_dump = 0;

const int config_audio_eq_en = 0
                               | 0; //end

const float config_audio_eq_fade_step = 0.1f;// TRANSLATED: Bass/treble gain adjustment step during music playback.
const int AUDIO_EQ_CLEAR_MEM_BY_MUTE_TIME_MS = 0;// TRANSLATED: Continuous silence duration before clearing EQ memory (example: 300 ms).
const int AUDIO_EQ_CLEAR_MEM_BY_MUTE_LIMIT = 0; // TRANSLATED: Silence detection threshold.

// TRANSLATED: WTS sample-rate support; enable either group or both.
const  int  silk_fsN_enable = 1;   // TRANSLATED: Support 8-12 kHz sample rates.
const  int  silk_fsW_enable = 1;  // TRANSLATED: Support 16-24 kHz sample rates.

const int config_audio_drc_en = 0;

const int LPC_JUST_FADE = 0; // TRANSLATED: Music PLC: 0=packet concealment (3660 bytes RAM, 1268 bytes code); 1=fade only (688 bytes RAM, 500 bytes code).
const char config_audio_mixer_ch_highlight_enable = 0; // TRANSLATED: Enable mixer channel highlighting.

#ifdef SBC_CUSTOM_DECODER_BUF_SIZE
const short config_sbc_decoder_buf_size = 512;
#endif

const int config_audio_dac_mix_enable = 0;

// TRANSLATED: Enable the mixer; disabling bypasses mixing and most mixer features.
const int config_mixer_en = 1;
// TRANSLATED: Enable mixer resampling.
const int config_mixer_src_en = 1;

// TRANSLATED: Allow simultaneous decoder resources; when disabled, configured overlap becomes preemption.
const int config_audio_dec_wait_protect_en = 1;

// TRANSLATED: Enable audio-stream branches.
const int config_audio_stream_frame_copy_en = 1;

// TRANSLATED: Let the decoder app control the mixer; otherwise the upper layer must set the stream output node.
const int audio_dec_app_mix_en = 1;

// TRANSLATED: Audio-stream branch circular-buffer size.
const int config_audio_stream_frame_copy_cbuf_min = 128;
const int config_audio_stream_frame_copy_cbuf_max = 1024;

#if RECORDER_MIX_EN
// TRANSLATED: Timeout while waiting for other decoders to finish becoming inactive.
const int config_audio_dec_unactive_to = 10;
// TRANSLATED: Enable audio-stream ioctrl.
const int config_audio_stream_frame_ioctrl_en = 1;
#else
// TRANSLATED: Timeout while waiting for other decoders to finish becoming inactive.
const int config_audio_dec_unactive_to = 0;
// TRANSLATED: Enable audio-stream ioctrl.
const int config_audio_stream_frame_ioctrl_en = 0;
#endif

// TRANSLATED: Enable TWS synchronization in the decoder app.
const int audio_dec_app_sync_en = 0;

// TRANSLATED: WMA TWS decoder control.
const int WMA_TWSDEC_EN = 0;

#if TCFG_DEC2TWS_ENABLE || SOUNDCARD_ENABLE

// TRANSLATED: Samples per decode: 1 means 32 sample pairs; n means n*32 pairs.
// TRANSLATED: Values above 1 require malloc, e.g. config_mp3_dec_use_malloc=1.
const int MP3_OUTPUT_LEN = 4;
const int WMA_OUTPUT_LEN = 4;

#else

// TRANSLATED: Samples per decode: 1 means 32 sample pairs; n means n*32 pairs.
// TRANSLATED: Values above 1 require malloc, e.g. config_mp3_dec_use_malloc=1.
const int MP3_OUTPUT_LEN = 1;
const int WMA_OUTPUT_LEN = 1;

#endif

// TRANSLATED: Maximum WAV bitrate, in kbps.
const int WAV_MAX_BITRATEV = (48 * 2 * 24);

// TRANSLATED: Samples per decode; recommended 32..900, e.g. 128 means 128 sample pairs.
// TRANSLATED: Above 128, use malloc, e.g. config_wav_dec_use_malloc=1.
const int  WAV_DECODER_PCM_POINTS = 128;

// TRANSLATED: Above 128 output samples without malloc, enlarge the corresponding buffer.
// TRANSLATED: Check the decoder's logged memory requirement; each additional output sample usually needs four bytes.
int wav_mem_ext[(1336  + 3) / 4] SEC(.wav_mem); // TRANSLATED: Enlarge this array for output sizes above 128.

const int OPUS_SRINDEX = 0; // TRANSLATED: Opus frame size: 0=40 bytes, 1=80 bytes, 2=160 bytes.

const  int  WTGV2_STACK2BUF = 0;  // TRANSLATED: 1 enlarges the decoder buffer by 760 bytes and reduces stack use.

// TRANSLATED: Output from the mixer in a separate task.
const int config_mixer_task = 0;

// TRANSLATED: Automatically choose output channels for TWS decoding.
// TRANSLATED: Mono: AUDIO_CH_L/AUDIO_CH_R; stereo: AUDIO_CH_DUAL_L/AUDIO_CH_DUAL_R.
// TRANSLATED: When disabled, use output_ch_num and output_ch_type/ch_type.
const int audio_tws_auto_channel = 1;

// TRANSLATED: Use a separate decoder output task.
const int config_audio_dec_out_task_en = 0;

const char config_audio_mini_enable = 0;

/* TRANSLATED: Capacitorless MIC configuration. */
const u8 const_mic_capless_en = 0;

#if AUDIO_EQUALLOUDNESS_CONFIG
const int const_equall_loundness_en = 1;
#else
const int const_equall_loundness_en = 0;
#endif

#if AUDIO_VBASS_CONFIG
const int const_vbass_en = 1;
#else
const int const_vbass_en = 0;
#endif

#if AUDIO_SURROUND_CONFIG
const int const_surround_en = 1;
#else
const int const_surround_en = 0;
#endif

const int const_sel_adpcm_type = 1;// TRANSLATED: 1=imaen_adpcm, 0=msen_adpcm.

const int MIDI_TONE_MODE = 1;

const int config_mp3_dec_use_malloc     = 1;
const int config_mp3pick_dec_use_malloc = 1;
const int config_wma_dec_use_malloc     = 1;
const int config_wmapick_dec_use_malloc = 1;
const int config_m4a_dec_use_malloc     = 1;
const int config_m4apick_dec_use_malloc = 1;
const int config_wav_dec_use_malloc     = 1;
const int config_alac_dec_use_malloc    = 1;
const int config_dts_dec_use_malloc     = 1;
const int config_amr_dec_use_malloc     = 1;
const int config_flac_dec_use_malloc    = 1;
const int config_ape_dec_use_malloc     = 1;
const int config_aac_dec_use_malloc     = 1;
const int config_aptx_dec_use_malloc    = 1;
const int config_midi_dec_use_malloc    = 1;
const int config_lc3_dec_use_malloc     = 1;
const int config_speex_dec_use_malloc   = 1;
const int config_opus_dec_use_malloc   = 1;

const int vc_pitchshift_fastmode_flag        = 1;
const  int  vc_pitchshift_downmode_flag = 0;  // TRANSLATED: Enable downsampling for voice changing.
const int howling_pitchshift_fastmode_flag   = 1;
const  int RS_FAST_MODE_QUALITY = 2;	// TRANSLATED: Software resampler filter order: 2..8, with 8 selecting order 16. The SDK notes a correlation with processing speed.
const int config_howling_enable_pemafrow_mode = 0;
const int config_howling_enable_trap_mode     = 0;// TRANSLATED: Enable notch-filter howling suppression.
const int config_howling_enable_pitchps_mode  = 1; // TRANSLATED: Enable frequency-shifting howling suppression.
const  int DOWN_S_FLAG 				= 0; // TRANSLATED: Enable downsampling for reverb.
const  int  ECHO_INT_VAL_OUT = 0;       //  TRANSLATED: 1 makes echo output int samples and requires a downstream DRC limiter; not implemented.

#if (AUDIO_OUTPUT_WAY == AUDIO_OUTPUT_WAY_DONGLE)
const int config_mp3_enc_use_layer_3	= 1;
#else
const int config_mp3_enc_use_layer_3	= 0;
#endif

#define FAST_FREQ_restrict				0x01 // TRANSLATED: Skip frequencies above 16 kHz; generally beyond hearing but detectable by instruments.
#define FAST_FILTER_restrict			0x02 // TRANSLATED: Shorten the filter; increases subband sidelobes and reduces edge steepness.
#define FAST_CHANNEL_restrict			0x04 // TRANSLATED: Mix left/right before decoding; sacrifices spatial separation, especially noticeable on headphones.

const int config_mp3_dec_speed_mode 	= 0; // TRANSLATED: FAST_FREQ_restrict | FAST_FILTER_restrict | FAST_CHANNEL_restrict: all three give the fastest decoding.

const int config_mp3_start_silence_drop =  0;// TRANSLATED: Discard initial silence in MP3 decoding.

// TRANSLATED: Return an end message when fast-forward/rewind reaches the file end.
const int config_decoder_ff_fr_end_return_event_end = 0;

// TRANSLATED: Decoder task test.
const int audio_decoder_test_en = 0;
// TRANSLATED: Implement the following interface when audio_decoder_test_en is enabled.
// TRANSLATED: Example interface implementation.
#include "audio/demo/audio_decoder_test.c"

// TRANSLATED: Encoder task test.
const int audio_encoder_test_en = 0;
// TRANSLATED: Implement the following interface when audio_encoder_test_en is enabled.
// TRANSLATED: Example interface implementation.
#include "audio/demo/audio_encoder_test.c"
// TRANSLATED: Use assembly optimization for digital volume; set to 0 on unsupported chips.
const int const_config_digvol_use_round = 1;

/**
 * @brief Log (Verbose/Info/Debug/Warn/Error)
 */
/*-----------------------------------------------------------*/
const char log_tag_const_v_EQ_CFG AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(FALSE);
const char log_tag_const_i_EQ_CFG AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(FALSE);
const char log_tag_const_d_EQ_CFG AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(FALSE);
const char log_tag_const_w_EQ_CFG AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_EQ_CFG AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_EQ_CFG_TOOL AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(FALSE);
const char log_tag_const_i_EQ_CFG_TOOL AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(FALSE);
const char log_tag_const_d_EQ_CFG_TOOL AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(FALSE);
const char log_tag_const_w_EQ_CFG_TOOL AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_EQ_CFG_TOOL AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_EQ_APPLY AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_EQ_APPLY AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_EQ_APPLY AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_w_EQ_APPLY AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_EQ_APPLY AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_w_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_APP_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_APP_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_APP_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_w_APP_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_APP_DRC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_EQ AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_EQ AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_EQ AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_w_EQ AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_EQ AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_VBASS AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_VBASS AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_VBASS AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_w_VBASS AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_VBASS AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_AUD_ADC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_AUD_ADC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_AUD_ADC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_w_AUD_ADC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_e_AUD_ADC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);

const char log_tag_const_v_AUD_DAC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_AUD_DAC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_AUD_DAC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_w_AUD_DAC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_e_AUD_DAC AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);

const char log_tag_const_v_AUD_AUX AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_AUD_AUX AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_AUD_AUX AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_w_AUD_AUX AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_e_AUD_AUX AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);

const char log_tag_const_v_MIXER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_c_MIXER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_MIXER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_MIXER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_MIXER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_AUDIO_STREAM AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_c_AUDIO_STREAM AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_AUDIO_STREAM AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_AUDIO_STREAM AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_AUDIO_STREAM AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_AUDIO_DECODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_c_AUDIO_DECODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_AUDIO_DECODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_AUDIO_DECODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_AUDIO_DECODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_AUDIO_ENCODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_c_AUDIO_ENCODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_AUDIO_ENCODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_d_AUDIO_ENCODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_AUDIO_ENCODER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);

const char log_tag_const_v_AEC_USER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_c_AEC_USER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(0);
const char log_tag_const_i_AEC_USER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(1);
const char log_tag_const_d_AEC_USER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
const char log_tag_const_e_AEC_USER AT(.LOG_TAG_CONST) = CONFIG_DEBUG_LIB(TRUE);
