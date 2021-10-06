/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/debugfs.h>
#include <linux/wait.h>
#include <linux/bitops.h>
#include <linux/mfd/wcd9xxx/core.h>
#include <linux/mfd/wcd9xxx/wcd9xxx_registers.h>
#include <linux/mfd/wcd9xxx/wcd9320_registers.h>
#include <linux/mfd/wcd9xxx/pdata.h>
#include <linux/mxaudio.h>
#include <linux/regulator/consumer.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/pm_qos.h>
#include <linux/pm.h>
#include <mach/cpuidle.h>
#include <linux/wakelock.h>
#include <linux/switch.h>
#include <linux/sec_jack.h>
#include "wcd9320.h"
#include "wcd9xxx-resmgr.h"
#include "wcd9xxx-common.h"

#if defined(CONFIG_SND_SOC_ES705)
#include "audience/es705-export.h"
#elif defined(CONFIG_SND_SOC_ES325)
#include "es325-export.h"
#endif

#if defined(CONFIG_SND_SOC_ES705)

#define CONFIG_SND_SOC_ESXXX
#define REMOTE_ROUTE_ENABLE_CB  es705_remote_route_enable
#define SLIM_GET_CHANNEL_MAP_CB es705_slim_get_channel_map
#define SLIM_SET_CHANNEL_MAP_CB es705_slim_set_channel_map
#define SLIM_HW_PARAMS_CB es705_slim_hw_params
#define REMOTE_CFG_SLIM_RX_CB es705_remote_cfg_slim_rx
#define REMOTE_CLOSE_SLIM_RX_CB	es705_remote_close_slim_rx
#define REMOTE_CFG_SLIM_TX_CB es705_remote_cfg_slim_tx
#define REMOTE_CLOSE_SLIM_TX_CB	es705_remote_close_slim_tx
#define REMOTE_ADD_CODEC_CONTROLS_CB es705_remote_add_codec_controls
#endif

#define TAIKO_MAD_SLIMBUS_TX_PORT 12
#define TAIKO_MAD_AUDIO_FIRMWARE_PATH "wcd9320/wcd9320_mad_audio.bin"
#define TAIKO_VALIDATE_RX_SBPORT_RANGE(port) ((port >= 16) && (port <= 22))
#define TAIKO_CONVERT_RX_SBPORT_ID(port) (port - 16) /* RX1 port ID = 0 */

#define TAIKO_HPH_PA_SETTLE_COMP_ON 5000
#define TAIKO_HPH_PA_SETTLE_COMP_OFF 13000

#define DAPM_MICBIAS2_EXTERNAL_STANDALONE "MIC BIAS2 External Standalone"
#define DAPM_MICBIAS3_EXTERNAL_STANDALONE "MIC BIAS3 External Standalone"

/* RX_HPH_CNP_WG_TIME increases by 0.24ms */
#define TAIKO_WG_TIME_FACTOR_US	240
#define ZDET_RAMP_WAIT_US 18000

static struct wcd9xxx *sound_control_codec_ptr;
static struct snd_soc_codec *direct_codec;
static atomic_t kp_taiko_priv;
struct wake_lock hph_playback_wake_lock;
struct wake_lock spk_playback_wake_lock;
#ifdef CONFIG_SAMSUNG_JACK_SYSFS
extern int secjack_state;
#endif
static struct afe_param_slimbus_slave_port_cfg taiko_slimbus_slave_port_cfg = {
	.minor_version = 1,
	.slimbus_dev_id = AFE_SLIMBUS_DEVICE_1,
	.slave_dev_pgd_la = 0,
	.slave_dev_intfdev_la = 0,
	.bit_width = 16,
	.data_format = 0,
	.num_channels = 1
};

static struct afe_param_cdc_reg_cfg audio_reg_cfg[] = {
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_MAD_MAIN_CTL_1),
		HW_MAD_AUDIO_ENABLE, 0x1, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_MAD_AUDIO_CTL_3),
		HW_MAD_AUDIO_SLEEP_TIME, 0xF, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_MAD_AUDIO_CTL_4),
		HW_MAD_TX_AUDIO_SWITCH_OFF, 0x1, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_INTR_DESTN3),
		MAD_AUDIO_INT_DEST_SELECT_REG, 0x1, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_INTR_MASK3),
		MAD_AUDIO_INT_MASK_REG, 0x1, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_INTR_STATUS3),
		MAD_AUDIO_INT_STATUS_REG, 0x1, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_INTR_CLEAR3),
		MAD_AUDIO_INT_CLEAR_REG, 0x1, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_SB_PGD_PORT_TX_BASE),
		SB_PGD_PORT_TX_WATERMARK_N, 0x1E, 8, 0x1
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_SB_PGD_PORT_TX_BASE),
		SB_PGD_PORT_TX_ENABLE_N, 0x1, 8, 0x1
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_SB_PGD_PORT_RX_BASE),
		SB_PGD_PORT_RX_WATERMARK_N, 0x1E, 8, 0x1
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_SB_PGD_PORT_RX_BASE),
		SB_PGD_PORT_RX_ENABLE_N, 0x1, 8, 0x1
	},
	{	1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_ANC1_IIR_B1_CTL),
		AANC_FF_GAIN_ADAPTIVE, 0x4, 8, 0
	},
	{	1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_ANC1_IIR_B1_CTL),
		AANC_FFGAIN_ADAPTIVE_EN, 0x8, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_ANC1_GAIN_CTL),
		AANC_GAIN_CONTROL, 0xFF, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_INTR_DESTN3),
		MAD_CLIP_INT_DEST_SELECT_REG, 0x8, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_INTR_MASK3),
		MAD_CLIP_INT_MASK_REG, 0x8, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_INTR_STATUS3),
		MAD_CLIP_INT_STATUS_REG, 0x8, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_INTR_CLEAR3),
		MAD_CLIP_INT_CLEAR_REG, 0x8, 8, 0
	},
};

static struct afe_param_cdc_reg_cfg clip_reg_cfg[] = {
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_SPKR_CLIPDET_B1_CTL),
		SPKR_CLIP_PIPE_BANK_SEL, 0x3, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_SPKR_CLIPDET_VAL0),
		SPKR_CLIPDET_VAL0, 0xff, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_SPKR_CLIPDET_VAL1),
		SPKR_CLIPDET_VAL1, 0xff, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_SPKR_CLIPDET_VAL2),
		SPKR_CLIPDET_VAL2, 0xff, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_SPKR_CLIPDET_VAL3),
		SPKR_CLIPDET_VAL3, 0xff, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_SPKR_CLIPDET_VAL4),
		SPKR_CLIPDET_VAL4, 0xff, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_SPKR_CLIPDET_VAL5),
		SPKR_CLIPDET_VAL5, 0xff, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_SPKR_CLIPDET_VAL6),
		SPKR_CLIPDET_VAL6, 0xff, 8, 0
	},
	{
		1,
		(TAIKO_REGISTER_START_OFFSET + TAIKO_A_CDC_SPKR_CLIPDET_VAL7),
		SPKR_CLIPDET_VAL7, 0xff, 8, 0
	},
};

static struct afe_param_cdc_reg_cfg_data taiko_audio_reg_cfg = {
	.num_registers = ARRAY_SIZE(audio_reg_cfg),
	.reg_data = audio_reg_cfg,
};

static struct afe_param_cdc_reg_cfg_data taiko_clip_reg_cfg = {
	.num_registers = ARRAY_SIZE(clip_reg_cfg),
	.reg_data = clip_reg_cfg,
};

static struct afe_param_id_cdc_aanc_version taiko_cdc_aanc_version = {
	.cdc_aanc_minor_version = AFE_API_VERSION_CDC_AANC_VERSION,
	.aanc_hw_version        = AANC_HW_BLOCK_VERSION_2,
};

static struct afe_param_id_clip_bank_sel clip_bank_sel = {
	.minor_version = AFE_API_VERSION_CLIP_BANK_SEL_CFG,
	.num_banks = AFE_CLIP_MAX_BANKS,
	.bank_map = {0, 1, 2, 3},
};

#define WCD9320_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |\
			SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000 |\
			SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)

#define NUM_DECIMATORS 10
#define NUM_INTERPOLATORS 7
#define BITS_PER_REG 8
#define TAIKO_TX_PORT_NUMBER	16
#define TAIKO_RX_PORT_START_NUMBER	16

#define TAIKO_I2S_MASTER_MODE_MASK 0x08

#define TAIKO_SLIM_CLOSE_TIMEOUT 1000
#define TAIKO_SLIM_IRQ_OVERFLOW (1 << 0)
#define TAIKO_SLIM_IRQ_UNDERFLOW (1 << 1)
#define TAIKO_SLIM_IRQ_PORT_CLOSED (1 << 2)
#define TAIKO_MCLK_CLK_12P288MHZ 12288000
#define TAIKO_MCLK_CLK_9P6MHZ 9600000

#define TAIKO_FORMATS_S16_S24_LE (SNDRV_PCM_FMTBIT_S16_LE | \
			SNDRV_PCM_FORMAT_S24_LE)

#define TAIKO_FORMATS (SNDRV_PCM_FMTBIT_S16_LE)

#define TAIKO_SLIM_PGD_PORT_INT_TX_EN0 (TAIKO_SLIM_PGD_PORT_INT_EN0 + 2)

enum {
	AIF1_PB = 0,
	AIF1_CAP = 1,
	AIF2_PB = 2,
	AIF2_CAP = 3,
	AIF3_PB = 4,
	AIF3_CAP = 5,
	AIF4_VIFEED = 6,
	AIF4_MAD_TX = 7,
	NUM_CODEC_DAIS = 8,
};

enum {
	RX_MIX1_INP_SEL_ZERO = 0,
	RX_MIX1_INP_SEL_SRC1 = 1,
	RX_MIX1_INP_SEL_SRC2 = 2,
	RX_MIX1_INP_SEL_IIR1 = 3,
	RX_MIX1_INP_SEL_IIR2 = 4,
	RX_MIX1_INP_SEL_RX1 = 5,
	RX_MIX1_INP_SEL_RX2 = 6,
	RX_MIX1_INP_SEL_RX3 = 7,
	RX_MIX1_INP_SEL_RX4 = 8,
	RX_MIX1_INP_SEL_RX5 = 9,
	RX_MIX1_INP_SEL_RX6 = 10,
	RX_MIX1_INP_SEL_RX7 = 11,
	RX_MIX1_INP_SEL_AUXRX = 12,
};

#define TAIKO_COMP_DIGITAL_GAIN_OFFSET 3

static const DECLARE_TLV_DB_SCALE(digital_gain, 0, 1, 0);
static const DECLARE_TLV_DB_SCALE(line_gain, 0, 7, 1);
static const DECLARE_TLV_DB_SCALE(analog_gain, 0, 25, 1);
static struct snd_soc_dai_driver taiko_dai[];

/* Codec supports 2 IIR filters */
enum {
	IIR1 = 0,
	IIR2 = 1,
	IIR_MAX = 2,
};
/* Codec supports 5 bands */
enum {
	BAND1 = 0,
	BAND2 = 1,
	BAND3 = 2,
	BAND4 = 3,
	BAND5 = 4,
	BAND_MAX = 5,
};

enum {
	COMPANDER_0 = 0,
	COMPANDER_1 = 1,
	COMPANDER_2 = 2,
	COMPANDER_MAX = 3,
};

enum {
	COMPANDER_FS_8KHZ = 0,
	COMPANDER_FS_16KHZ = 1,
	COMPANDER_FS_32KHZ = 2,
	COMPANDER_FS_48KHZ = 3,
	COMPANDER_FS_96KHZ = 4,
	COMPANDER_FS_192KHZ = 5,
	COMPANDER_FS_MAX = 6,
};

struct comp_sample_dependent_params {
	u32 peak_det_timeout;
	u32 rms_meter_div_fact;
	u32 rms_meter_resamp_fact;
};

struct taiko_priv {
	struct snd_soc_codec *codec;
	u32 adc_count;
	u32 rx_bias_count;
	s32 dmic_1_2_clk_cnt;
	s32 dmic_3_4_clk_cnt;
	s32 dmic_5_6_clk_cnt;
	s32 ldo_h_users;
	s32 micb_2_users;

	u32 anc_slot;
	int anc_func;

	/*track taiko interface type*/
	u8 intf_type;

	/* num of slim ports required */
	struct wcd9xxx_codec_dai_data  dai[NUM_CODEC_DAIS];

	/*compander*/
	int comp_enabled[COMPANDER_MAX];
	u32 comp_fs[COMPANDER_MAX];

	/* Maintain the status of AUX PGA */
	int aux_pga_cnt;
	u8 aux_l_gain;
	u8 aux_r_gain;

	bool spkr_pa_widget_on;
	struct regulator *spkdrv_reg;

	bool mbhc_started;

	struct afe_param_cdc_slimbus_slave_cfg slimbus_slave_cfg;

	/* resmgr module */
	struct wcd9xxx_resmgr resmgr;
	/* mbhc module */
	struct wcd9xxx_mbhc mbhc;

	/* class h specific data */
	struct wcd9xxx_clsh_cdc_data clsh_d;

	int (*machine_codec_event_cb)(struct snd_soc_codec *codec,
			enum wcd9xxx_codec_event);

	/*
	 * list used to save/restore registers at start and
	 * end of impedance measurement
	 */
	struct list_head reg_save_restore;
	struct pm_qos_request pm_qos_req;
};

struct hpf_work {
	struct taiko_priv *taiko;
	u32 decimator;
	u8 tx_hpf_cut_off_freq;
	struct delayed_work dwork;
};

static struct hpf_work tx_hpf_work[NUM_DECIMATORS];

static const struct wcd9xxx_ch taiko_rx_chs[TAIKO_RX_MAX] = {
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER, 0),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 1, 1),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 2, 2),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 3, 3),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 4, 4),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 5, 5),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 6, 6),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 7, 7),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 8, 8),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 9, 9),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 10, 10),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 11, 11),
	WCD9XXX_CH(TAIKO_RX_PORT_START_NUMBER + 12, 12),
};

static const struct wcd9xxx_ch taiko_tx_chs[TAIKO_TX_MAX] = {
	WCD9XXX_CH(0, 0),
	WCD9XXX_CH(1, 1),
	WCD9XXX_CH(2, 2),
	WCD9XXX_CH(3, 3),
	WCD9XXX_CH(4, 4),
	WCD9XXX_CH(5, 5),
	WCD9XXX_CH(6, 6),
	WCD9XXX_CH(7, 7),
	WCD9XXX_CH(8, 8),
	WCD9XXX_CH(9, 9),
	WCD9XXX_CH(10, 10),
	WCD9XXX_CH(11, 11),
	WCD9XXX_CH(12, 12),
	WCD9XXX_CH(13, 13),
	WCD9XXX_CH(14, 14),
	WCD9XXX_CH(15, 15),
};

static const u32 vport_check_table[NUM_CODEC_DAIS] = {
	0,					/* AIF1_PB */
	(1 << AIF2_CAP) | (1 << AIF3_CAP),	/* AIF1_CAP */
	0,					/* AIF2_PB */
	(1 << AIF1_CAP) | (1 << AIF3_CAP),	/* AIF2_CAP */
	0,					/* AIF2_PB */
	(1 << AIF1_CAP) | (1 << AIF2_CAP),	/* AIF2_CAP */
};

static const u32 vport_i2s_check_table[NUM_CODEC_DAIS] = {
	0,	/* AIF1_PB */
	0,	/* AIF1_CAP */
	0,	/* AIF2_PB */
	0,	/* AIF2_CAP */
};

/* Compander 0's clock source is on interpolator 7 */
static const u32 comp_shift[] = {
	4,
	0,
	2,
};

static const int comp_rx_path[] = {
	COMPANDER_1,
	COMPANDER_1,
	COMPANDER_2,
	COMPANDER_2,
	COMPANDER_2,
	COMPANDER_2,
	COMPANDER_0,
	COMPANDER_MAX,
};

static const struct comp_sample_dependent_params comp_samp_params[] = {
	{
		/* 8 Khz */
		.peak_det_timeout = 0x06,
		.rms_meter_div_fact = 0x09,
		.rms_meter_resamp_fact = 0x06,
	},
	{
		/* 16 Khz */
		.peak_det_timeout = 0x07,
		.rms_meter_div_fact = 0x0A,
		.rms_meter_resamp_fact = 0x0C,
	},
	{
		/* 32 Khz */
		.peak_det_timeout = 0x08,
		.rms_meter_div_fact = 0x0B,
		.rms_meter_resamp_fact = 0x1E,
	},
	{
		/* 48 Khz */
		.peak_det_timeout = 0x09,
		.rms_meter_div_fact = 0x0B,
		.rms_meter_resamp_fact = 0x28,
	},
	{
		/* 96 Khz */
		.peak_det_timeout = 0x0A,
		.rms_meter_div_fact = 0x0C,
		.rms_meter_resamp_fact = 0x50,
	},
	{
		/* 192 Khz */
		.peak_det_timeout = 0x0B,
		.rms_meter_div_fact = 0xC,
		.rms_meter_resamp_fact = 0xA0,
	},
};

static unsigned short rx_digital_gain_reg[] = {
	TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL,
	TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL,
	TAIKO_A_CDC_RX3_VOL_CTL_B2_CTL,
	TAIKO_A_CDC_RX4_VOL_CTL_B2_CTL,
	TAIKO_A_CDC_RX5_VOL_CTL_B2_CTL,
	TAIKO_A_CDC_RX6_VOL_CTL_B2_CTL,
	TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL,
};


static unsigned short tx_digital_gain_reg[] = {
	TAIKO_A_CDC_TX1_VOL_CTL_GAIN,
	TAIKO_A_CDC_TX2_VOL_CTL_GAIN,
	TAIKO_A_CDC_TX3_VOL_CTL_GAIN,
	TAIKO_A_CDC_TX4_VOL_CTL_GAIN,
	TAIKO_A_CDC_TX5_VOL_CTL_GAIN,
	TAIKO_A_CDC_TX6_VOL_CTL_GAIN,
	TAIKO_A_CDC_TX7_VOL_CTL_GAIN,
	TAIKO_A_CDC_TX8_VOL_CTL_GAIN,
	TAIKO_A_CDC_TX9_VOL_CTL_GAIN,
	TAIKO_A_CDC_TX10_VOL_CTL_GAIN,
};

/* MX Audio */

#define MX_OUTPUT_MUTE 172
/* Shared values for core resource locking */
u8 hphl_cached_gain;
u8 hphr_cached_gain;
u8 speaker_cached_gain;
/* Mic gain
 * TAIKO_A_CDC_TX3_VOL_CTL_GAIN
*/

#if 0
u8 iir1_cached_gain;
u8 iir2_cached_gain;
unsigned int mx_hw_eq = HWEQ_OFF;
#endif
#ifdef CONFIG_RAMP_VOLUME
unsigned int ramp_volume;
#endif
static unsigned int headphone_mute;
static unsigned int speaker_mute;
#if 0
static u8 iir1_enabled[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir1_band_1[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir1_band_2[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir1_band_3[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir1_band_4[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir1_band_5[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir2_enabled[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir2_band_1[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir2_band_2[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir2_band_3[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir2_band_4[BAND_MAX] = { 0, 0, 0, 0, 0 };
static u8 iir2_band_5[BAND_MAX] = { 0, 0, 0, 0, 0 };
#endif
static u8 hphl_hpf_cutoff = 0;
static u8 hphr_hpf_cutoff = 0;
static u8 speaker_hpf_cutoff = 0;
static u8 hphl_hpf_bypass = 0;
static u8 hphr_hpf_bypass = 0;
static u8 speaker_hpf_bypass = 0;

#define HPH_RX_GAIN_MAX 20
#define HPH_PA_SHIFT 0
#define HPH_PA_GAIN_MASK GENMASK(4, 0)

static unsigned int hph_pa_enabled = 0;
static u8 hphl_pa_cached_gain = 20;
static u8 hphr_pa_cached_gain = 20;
static unsigned int hph_poweramp_mask = 31; /* (1 << fls(max)) - 1 */
static unsigned int uhqa_mode = 0;
static unsigned int high_perf_mode = 0;
static bool class_ab_left_active = false;
static bool class_ab_right_active = false;
static bool hpwidget_left = false;
static bool hpwidget_right = false;
static bool spkwidget = false;
static unsigned int compander_gain_lock = 0;
static unsigned int compander_gain_boost = 0;
static u32 sc_peak_det_timeout = 0x09;
static u32 sc_rms_meter_div_fact = 0x0B;
static u32 sc_rms_meter_resamp_fact = 0x28;
#ifdef CONFIG_WCD_BIAS_ACCESS
static u8 hph_pa_bias = 0x7A;
static u8 cnp_bias = 0x8A;
#endif
static unsigned int headphone_hdc = 0;
static unsigned int speaker_hdc = 0;

/*
#define TAIKO_A_RX_HPH_BIAS_CNP (0x1A8)
#define TAIKO_A_RX_HPH_BIAS_CNP__POR (0x8A)
*/

unsigned int anc_delay = 1;
static unsigned int hph_autochopper = 0;
static unsigned int chopper_bypass = 0;
#ifdef CONFIG_WAVEGEN_OVERRIDE
static unsigned int wavegen_override = 0;
#endif
/*RMS (Root Mean Squared) Power Detector*/
static unsigned int interpolator_boost = 0;
static bool interpolator_enabled;

#define PA_STAT_ON 8
#define PA_STAT_OFF 4

static int regread(unsigned short reg)
{
    return wcd9xxx_reg_read(&sound_control_codec_ptr->core_res, reg);
}

static void regwrite(unsigned short reg, u8 value)
{
    if (regread(reg) != value)
        wcd9xxx_reg_write(&sound_control_codec_ptr->core_res, reg, value);
}

static void mx_update_bits(unsigned short reg,
				u8 mask, u8 value)
{
    u8 old, new;
	old = regread(reg);
	new = (old & ~mask) | (value & mask);
	if (old != new)
		regwrite(reg, new);
}

static void mx_update_bits_locked(unsigned short reg,
				u8 mask, u8 value)
{
    mutex_lock(&direct_codec->mutex);
	mx_update_bits(reg, mask, value);
	mutex_unlock(&direct_codec->mutex);
}

static void hph_wake_lock(void)
{
    if (wake_lock_active(&hph_playback_wake_lock))
        return;

    wake_lock(&hph_playback_wake_lock);
}

static void hph_wake_unlock(void)
{
    if (!wake_lock_active(&hph_playback_wake_lock))
        return;

    wake_unlock(&hph_playback_wake_lock);
}

static void spk_wake_lock(void)
{
    if (wake_lock_active(&spk_playback_wake_lock))
        return;

    wake_lock(&spk_playback_wake_lock);
}

static void spk_wake_unlock(void)
{
    if (!wake_lock_active(&spk_playback_wake_lock))
        return;

    wake_unlock(&spk_playback_wake_lock);
}
#ifdef CONFIG_SAMSUNG_JACK_SYSFS
static bool sec_jacked(void)
{
    if (secjack_state)
        return true;

    return false;
}
#endif
static bool hpwidget(void)
{
    if ((regread(TAIKO_A_RX_HPH_L_STATUS) == PA_STAT_ON &&
         regread(TAIKO_A_RX_HPH_R_STATUS) == PA_STAT_ON) ||
         (hpwidget_left && hpwidget_right)) {
        hph_wake_lock();
        return true;
    }

    hph_wake_unlock();
    return false;
}

#if 0
static bool poweramp_active(void)
{
	return (hph_pa_enabled && hpwidget());
}
#endif
static bool hpwidget_any(void)
{
    return (regread(TAIKO_A_RX_HPH_L_STATUS) == PA_STAT_ON ||
            regread(TAIKO_A_RX_HPH_R_STATUS) == PA_STAT_ON ||
			hpwidget_left || hpwidget_right);
}

static bool spkwidget_active(void)
{
    if (spkwidget) {
        spk_wake_lock();
        return true;
    }

    spk_wake_unlock();
    return false;
}

static void set_high_perf_mode(bool enable)
{
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(direct_codec);
}

static void update_headphone_gain(void)
{
	if (headphone_mute) {
		regwrite(TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL, MX_OUTPUT_MUTE);
		regwrite(TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL, MX_OUTPUT_MUTE);
		return;
	}
	if (!hpwidget())
		return;

	regwrite(TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL, hphl_cached_gain);
	regwrite(TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL, hphr_cached_gain);
}
/*
	SOC_SINGLE_S8_TLV("IIR1 INP1 Volume", TAIKO_A_CDC_IIR1_GAIN_B1_CTL, -84,
		40, digital_gain),
	SOC_SINGLE_S8_TLV("IIR2 INP1 Volume", TAIKO_A_CDC_IIR2_GAIN_B1_CTL, -84,
		40, digital_gain),
*/
#if 0
static void update_iir_gain(void)
{
	if (!hpwidget())
		return;

	regwrite(TAIKO_A_CDC_IIR1_GAIN_B1_CTL, iir1_cached_gain);
	regwrite(TAIKO_A_CDC_IIR2_GAIN_B1_CTL, iir2_cached_gain);
}

static void update_crossfeed_gain(void)
{
	if (!hpwidget())
		return;
	regwrite(TAIKO_A_CDC_RX4_VOL_CTL_B2_CTL, crossleft_cached_gain);
	regwrite(TAIKO_A_CDC_RX3_VOL_CTL_B2_CTL, crossright_cached_gain);
}
#endif

static int read_hph_poweramp_gain(unsigned short reg)
{
	int rawval, outval;

	if (reg == WCD9XXX_A_RX_HPH_L_GAIN) {
		rawval = (regread(WCD9XXX_A_RX_HPH_L_GAIN) >> HPH_PA_SHIFT) & hph_poweramp_mask;
		outval = HPH_RX_GAIN_MAX - rawval;
        if (!hpwidget() || !outval)
            return hphl_pa_cached_gain;
		return outval;
	} else if (reg == WCD9XXX_A_RX_HPH_R_GAIN) {
		rawval = (regread(WCD9XXX_A_RX_HPH_R_GAIN) >> HPH_PA_SHIFT) & hph_poweramp_mask;
		outval = HPH_RX_GAIN_MAX - rawval;
        if (!hpwidget() || !outval)
            return hphr_pa_cached_gain;
		return outval;
	}
	return -EINVAL;
}

static void write_hph_poweramp_gain(unsigned short reg)
{
	unsigned int val, val_mask;
	unsigned int local_cached_gain;

	if (reg != WCD9XXX_A_RX_HPH_L_GAIN &&
		reg != WCD9XXX_A_RX_HPH_R_GAIN)
		return;

	if (!hpwidget())
    	local_cached_gain = 0;
	else if (reg == WCD9XXX_A_RX_HPH_L_GAIN)
		local_cached_gain = hphl_pa_cached_gain;
	else if (reg == WCD9XXX_A_RX_HPH_R_GAIN)
			local_cached_gain = hphr_pa_cached_gain;

	if (hph_pa_enabled)
        mx_update_bits(reg, 32, 32);
    else
        mx_update_bits(reg, 32, 0);

	val = local_cached_gain & hph_poweramp_mask;
	val = HPH_RX_GAIN_MAX - val;
	val_mask = hph_poweramp_mask << HPH_PA_SHIFT;
	val = val << HPH_PA_SHIFT;

	/*snd_soc_update_bits_locked(codec, reg, val_mask, val);*/
    mx_update_bits_locked(reg, val_mask, val);
}

static void write_hph_poweramp_regs(void)
{
	write_hph_poweramp_gain(WCD9XXX_A_RX_HPH_L_GAIN);
	write_hph_poweramp_gain(WCD9XXX_A_RX_HPH_R_GAIN);
}

static void update_speaker_gain(void)
{
	if (speaker_mute) {
		regwrite(TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL, MX_OUTPUT_MUTE);
		return;
	}

	if (!spkwidget_active())
		return;

	regwrite(TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL, speaker_cached_gain);
}

#ifdef CONFIG_WAVEGEN_OVERRIDE
static void update_wavegen(void)
{
        if (wavegen_override) {
    		/* Wavegen to 20 msec */
    		regwrite(TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDB);
    		regwrite(TAIKO_A_RX_HPH_CNP_WG_TIME, 0x58);
    		regwrite(TAIKO_A_RX_HPH_BIAS_WG_OCP, 0x1A);
        } else {
    		/* Wavegen to 5 msec */
    		regwrite(TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDA);
    		regwrite(TAIKO_A_RX_HPH_CNP_WG_TIME, 0x15);
    		regwrite(TAIKO_A_RX_HPH_BIAS_WG_OCP, 0x2A);
        }
}
#endif

/*
	__________________________Bypass Switch_______________________

	SOC_SINGLE(xname, reg, shift, max, invert)
	SOC_SINGLE("RX1 HPF Switch", TAIKO_A_CDC_RX1_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX2 HPF Switch", TAIKO_A_CDC_RX2_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX7 HPF Switch", TAIKO_A_CDC_RX7_B5_CTL, 2, 1, 0),

	#define SOC_SINGLE(xname, reg, shift, max, invert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw,\
	.put = snd_soc_put_volsw, \
	.private_value =  SOC_SINGLE_VALUE(reg, shift, max, invert) }

	static u8 hphl_hpf_bypass;
	static u8 hphr_hpf_bypass;
	static u8 speaker_hpf_bypass;
	______________________________________________________________
*/
static int read_hpf_bypass(unsigned short reg)
{
	unsigned int shift = 2;
	unsigned int mask = (1 << fls(1)) - 1;

	if (reg == TAIKO_A_CDC_RX1_B5_CTL ||
		reg == TAIKO_A_CDC_RX2_B5_CTL ||
		reg == TAIKO_A_CDC_RX7_B5_CTL)
		return (regread(reg) >> shift) & mask;

	return -EINVAL;
}

static void write_hpf_bypass(unsigned short reg)
{
	unsigned int shift = 2;
	unsigned int mask = (1 << fls(1)) - 1;
	unsigned int val, val_mask, old, new;
	unsigned short input_value;

	switch (reg) {
		case TAIKO_A_CDC_RX1_B5_CTL:
			input_value = hphl_hpf_bypass;
			break;
		case TAIKO_A_CDC_RX2_B5_CTL:
			input_value = hphr_hpf_bypass;
			break;
		case TAIKO_A_CDC_RX7_B5_CTL:
			input_value = speaker_hpf_bypass;
			break;
		default:
			return;
	}
	val = (input_value & mask);
	val_mask = mask << shift;
	val = val << shift;
	/*snd_soc_update_bits_locked(codec, reg, val_mask, val);*/
    mx_update_bits_locked(reg, val_mask, val);
}

/*
	__________________________HPF Cutoff__________________________
	Cutoff Registers - values are 0, 1, 2 corresponding with
		"MIN_3DB_4Hz", "MIN_3DB_75Hz", "MIN_3DB_150Hz"

	#define SOC_ENUM_SINGLE(xreg, xshift, xmax, xtexts) \
	static const struct soc_enum cf_rxmix1_enum =
		SOC_ENUM_SINGLE(TAIKO_A_CDC_RX1_B4_CTL, 0, 3, cf_text);

	static const struct soc_enum cf_rxmix2_enum =
		SOC_ENUM_SINGLE(TAIKO_A_CDC_RX2_B4_CTL, 0, 3, cf_text);

	static const struct soc_enum cf_rxmix7_enum =
		SOC_ENUM_SINGLE(TAIKO_A_CDC_RX7_B4_CTL, 0, 3, cf_text);

	.get = snd_soc_get_enum_double, .put = snd_soc_put_enum_double, \
	______________________________________________________________
*/

static void write_hpf_cutoff(unsigned short reg)
{
	unsigned int val, mask, bitmask, tempold, old, new;

	for (bitmask = 1; bitmask < 3; bitmask <<= 1)
		;
	switch (reg) {
		case TAIKO_A_CDC_RX1_B4_CTL:
			val = hphl_hpf_cutoff << 0;
			break;
		case TAIKO_A_CDC_RX2_B4_CTL:
			val = hphr_hpf_cutoff << 0;
			break;
		case TAIKO_A_CDC_RX7_B4_CTL:
			val = speaker_hpf_cutoff << 0;
			break;
		default:
			return;
	}

	mask = (bitmask - 1) << 0;

	/*snd_soc_update_bits_locked(codec, e->reg, mask, val);*/
    mx_update_bits_locked(reg, mask, val);
}

static int read_hpf_cutoff(unsigned short reg)
{
	unsigned int val, bitmask, local_reg_val;

	for (bitmask = 1; bitmask < 3; bitmask <<= 1)
		;
	val = regread(reg);
	local_reg_val = (val >> 0) & (bitmask - 1);

	switch (reg) {
		case TAIKO_A_CDC_RX1_B4_CTL:
			if (local_reg_val != hphl_hpf_cutoff) {
                local_reg_val = hphl_hpf_cutoff;
                write_hpf_cutoff(TAIKO_A_CDC_RX1_B4_CTL);
            }
   			break;
		case TAIKO_A_CDC_RX2_B4_CTL:
			if (local_reg_val != hphr_hpf_cutoff) {
                local_reg_val = hphr_hpf_cutoff;
                write_hpf_cutoff(TAIKO_A_CDC_RX2_B4_CTL);
            }
			break;
		case TAIKO_A_CDC_RX7_B4_CTL:
			if (local_reg_val != speaker_hpf_cutoff) {
                local_reg_val = speaker_hpf_cutoff;
                write_hpf_cutoff(TAIKO_A_CDC_RX7_B4_CTL);
            }
			break;
		default:
			return -EINVAL;
	}

	return local_reg_val;
}

/*
___________________________________________________________________________
IIR 1 Input Path:TAIKO_A_CDC_CONN_EQ1_B1_CTL
___________________________________________________________________________

static const struct soc_enum iir1_inp1_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ1_B1_CTL, 0, 18, iir_inp1_text);
___________________________________________________________________________
IIR 2 Input Path:TAIKO_A_CDC_CONN_EQ2_B1_CTL
___________________________________________________________________________
static const struct soc_enum iir2_inp1_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ2_B1_CTL, 0, 18, iir_inp1_text);
___________________________________________________________________________
SHARED:
___________________________________________________________________________
static const char * const iir_inp1_text[] = {
	"ZERO", "DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
	"DEC9", "DEC10", "RX1", "RX2", "RX3", "RX4", "RX5", "RX6", "RX7"
};
*/

/* mxaudio hweq 
static void setup_iir_path(void)
{
}
*/
static void write_autochopper(unsigned int enable)
{
    if (enable) {
        mx_update_bits_locked(TAIKO_A_RX_HPH_AUTO_CHOP, 5, 5);
    } else {
        mx_update_bits_locked(TAIKO_A_RX_HPH_AUTO_CHOP, 5, 0);

    }
}

/*
TAIKO_A_RX_HPH_AUTO_CHOP 0x1A4
TAIKO_A_RX_HPH_CHOP_CTL 0x1A5
*/

static void write_chopper(void)
{
    if (hpwidget_any()) {
        if (uhqa_mode && !chopper_bypass) {
            mx_update_bits(TAIKO_A_RX_HPH_CHOP_CTL, 0x80, 0x80);
            mx_update_bits(TAIKO_A_RX_HPH_CHOP_CTL, 0x20, 0x00);
        } else if (chopper_bypass) {
            mx_update_bits(TAIKO_A_RX_HPH_CHOP_CTL, 0x20, 0x20);
            mx_update_bits(TAIKO_A_RX_HPH_CHOP_CTL, 0x80, 0x00);
        } else {
            mx_update_bits(TAIKO_A_RX_HPH_CHOP_CTL, 0x80, 0x80);
            mx_update_bits(TAIKO_A_RX_HPH_CHOP_CTL, 0x20, 0x20);
        }
    } else {
        mx_update_bits(TAIKO_A_RX_HPH_CHOP_CTL, 0x20, 0x20);
        mx_update_bits(TAIKO_A_RX_HPH_CHOP_CTL, 0x80, 0x00);
    }
}

#ifdef CONFIG_WCD_BIAS_ACCESS
static void update_bias(void)
{
    if (!hpwidget_any()) {
       	mx_update_bits(TAIKO_A_RX_HPH_BIAS_PA, 0xff, hph_pa_bias);
        mx_update_bits(TAIKO_A_RX_HPH_BIAS_CNP, 0xff, cnp_bias);
    }
}
#else
static void update_bias(void) {}
#endif

static inline void update_interpolator(void)
{
    regwrite(TAIKO_A_CDC_COMP1_B3_CTL, 1);
    mx_update_bits(TAIKO_A_CDC_COMP1_B2_CTL, 240, 80);
    mx_update_bits(TAIKO_A_CDC_COMP1_B2_CTL, 15, 5);
    usleep_range(3000, 3100);

    if (interpolator_enabled) {
        if (interpolator_boost) {
            regwrite(TAIKO_A_CDC_COMP1_B3_CTL, sc_rms_meter_resamp_fact);
            mx_update_bits(TAIKO_A_CDC_COMP1_B2_CTL,
            	    240, (sc_rms_meter_div_fact << 4));
            mx_update_bits(TAIKO_A_CDC_COMP1_B2_CTL,
            		15, sc_peak_det_timeout);
        } else {
            regwrite(TAIKO_A_CDC_COMP1_B3_CTL, 0x28);
            mx_update_bits(TAIKO_A_CDC_COMP1_B2_CTL,
                    240, 176);
            mx_update_bits(TAIKO_A_CDC_COMP1_B2_CTL,
                    15, 9);
        }
    }
}

#define HDCLEFT 0
#define HDCRIGHT 1
static void write_hdc_left(bool enable)
{
    unsigned int engage;
    engage = !!enable;

	if (engage && headphone_hdc && hpwidget_left) {
		mx_update_bits(TAIKO_A_CDC_RX1_B3_CTL, 0xBC, 0x94);
		mx_update_bits(TAIKO_A_CDC_RX1_B4_CTL, 0x30, 0x10);
    } else {
		mx_update_bits(TAIKO_A_CDC_RX1_B3_CTL, 0xBC, 0x00);
		mx_update_bits(TAIKO_A_CDC_RX1_B4_CTL, 0x30, 0x00);
    }
}

static void write_hdc_right(bool enable)
{
    unsigned int engage;
    engage = !!enable;

	if (engage && headphone_hdc && hpwidget_right) {
		mx_update_bits(TAIKO_A_CDC_RX2_B3_CTL, 0xBC, 0x94);
		mx_update_bits(TAIKO_A_CDC_RX2_B4_CTL, 0x30, 0x10);
    } else {
		mx_update_bits(TAIKO_A_CDC_RX2_B3_CTL, 0xBC, 0x00);
		mx_update_bits(TAIKO_A_CDC_RX2_B4_CTL, 0x30, 0x00);
    }
}

static void write_hdc_dual(bool enable)
{
    unsigned int engage;
    engage = !!enable;

    write_hdc_left(engage);
    write_hdc_right(engage);
}

static void write_speaker_hdc(bool enable)
{
    unsigned int engage;
    engage = !!enable;

	if (engage && speaker_hdc && spkwidget_active()) {
		mx_update_bits(TAIKO_A_CDC_RX7_B3_CTL, 0xBC, 0x94);
		mx_update_bits(TAIKO_A_CDC_RX7_B4_CTL, 0x30, 0x10);
    } else {
		mx_update_bits(TAIKO_A_CDC_RX7_B3_CTL, 0xBC, 0x00);
		mx_update_bits(TAIKO_A_CDC_RX7_B4_CTL, 0x30, 0x00);
    }
}

/*
enum {
	IIR1 = 0,
	IIR2 = 1,
	IIR_MAX = 2,
};

enum {
	BAND1 = 0,
	BAND2 = 1,
	BAND3 = 2,
	BAND4 = 3,
	BAND5 = 4,
	BAND_MAX = 5,
};
 */
#if 0
static int read_iir_enable(unsigned short reg, unsigned int iir_slot, unsigned int band_slot)
{
	return ((regread((TAIKO_A_CDC_IIR1_CTL + 16 * iir_slot)) &
		(1 << band_slot)) != 0);
}

static void write_iir_enable(unsigned short reg, unsigned int iir_slot, unsigned int band_slot,
			int value)
{
	if (value < 0)
		value = 0;
	if (value > 1)
		value = 1;
	/* Mask first 5 bits, 6-8 are reserved */
	mx_update_bits((TAIKO_A_CDC_IIR1_CTL + 16 * iir_slot),
		(1 << band_slot), (value << band_slot));
}
#endif
static void update_control_regs(void)
{
	write_hpf_cutoff(TAIKO_A_CDC_RX1_B4_CTL);
	write_hpf_cutoff(TAIKO_A_CDC_RX2_B4_CTL);
	write_hpf_cutoff(TAIKO_A_CDC_RX7_B4_CTL);
	write_hpf_bypass(TAIKO_A_CDC_RX1_B5_CTL);
	write_hpf_bypass(TAIKO_A_CDC_RX2_B5_CTL);
	write_hpf_bypass(TAIKO_A_CDC_RX7_B5_CTL);
	write_chopper();
    write_autochopper(hph_autochopper);
}

static int spkr_drv_wrnd = 1;

static struct kernel_param_ops spkr_drv_wrnd_param_ops;
module_param_cb(spkr_drv_wrnd, &spkr_drv_wrnd_param_ops, &spkr_drv_wrnd, 0644);

static int spkr_drv_wrnd_param_set(const char *val,
				   const struct kernel_param *kp)
{
	struct snd_soc_codec *codec;
	int ret, old;
	struct taiko_priv *priv;

	priv = (struct taiko_priv *)atomic_read(&kp_taiko_priv);
	if (!priv) {
		pr_debug("%s: codec isn't yet registered\n", __func__);
		return 0;
	}

	codec = priv->codec;
    if (!codec)
        return 0;
	mutex_lock(&codec->mutex);
	old = spkr_drv_wrnd;
	ret = param_set_int(val, kp);
	if (ret) {
		mutex_unlock(&codec->mutex);
		return ret;
	}

	if ((old == -1 || old == 0) && spkr_drv_wrnd == 1) {
		WCD9XXX_BG_CLK_LOCK(&priv->resmgr);
		wcd9xxx_resmgr_get_bandgap(&priv->resmgr,
					   WCD9XXX_BANDGAP_AUDIO_MODE);
		WCD9XXX_BG_CLK_UNLOCK(&priv->resmgr);
		snd_soc_update_bits(codec, TAIKO_A_SPKR_DRV_EN, 0x80, 0x80);
	} else if (old == 1 && spkr_drv_wrnd == 0) {
		WCD9XXX_BG_CLK_LOCK(&priv->resmgr);
		wcd9xxx_resmgr_put_bandgap(&priv->resmgr,
					   WCD9XXX_BANDGAP_AUDIO_MODE);
		WCD9XXX_BG_CLK_UNLOCK(&priv->resmgr);
		if (!priv->spkr_pa_widget_on)
			snd_soc_update_bits(codec, TAIKO_A_SPKR_DRV_EN, 0x80,
					    0x00);
	}
	mutex_unlock(&codec->mutex);

	return 0;
}

static struct kernel_param_ops spkr_drv_wrnd_param_ops = {
	.set = spkr_drv_wrnd_param_set,
	.get = param_get_int,
};

static int taiko_get_anc_slot(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	ucontrol->value.integer.value[0] = taiko->anc_slot;
	return 0;
}

static int taiko_put_anc_slot(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	taiko->anc_slot = ucontrol->value.integer.value[0];
	return 0;
}

static int taiko_get_anc_func(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = taiko->anc_func;
	return 0;
}

static int taiko_put_anc_func(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	struct snd_soc_dapm_context *dapm = &codec->dapm;

	taiko->anc_func = ucontrol->value.integer.value[0];
	dev_dbg(codec->dev, "%s: anc_func %x", __func__, taiko->anc_func);

	if (taiko->anc_func) {
        mutex_lock(&dapm->codec->mutex);
		snd_soc_dapm_enable_pin(dapm, "ANC HPHR");
		snd_soc_dapm_enable_pin(dapm, "ANC HPHL");
		snd_soc_dapm_enable_pin(dapm, "ANC HEADPHONE");
		snd_soc_dapm_enable_pin(dapm, "ANC EAR PA");
		snd_soc_dapm_enable_pin(dapm, "ANC EAR");
		snd_soc_dapm_disable_pin(dapm, "HPHR");
		snd_soc_dapm_disable_pin(dapm, "HPHL");
		snd_soc_dapm_disable_pin(dapm, "HEADPHONE");
		snd_soc_dapm_disable_pin(dapm, "EAR PA");
		snd_soc_dapm_disable_pin(dapm, "EAR");
        mutex_unlock(&dapm->codec->mutex);
	} else {
        mutex_lock(&dapm->codec->mutex);
		snd_soc_dapm_disable_pin(dapm, "ANC HPHR");
		snd_soc_dapm_disable_pin(dapm, "ANC HPHL");
		snd_soc_dapm_disable_pin(dapm, "ANC HEADPHONE");
		snd_soc_dapm_disable_pin(dapm, "ANC EAR PA");
		snd_soc_dapm_disable_pin(dapm, "ANC EAR");
		snd_soc_dapm_enable_pin(dapm, "HPHR");
		snd_soc_dapm_enable_pin(dapm, "HPHL");
		snd_soc_dapm_enable_pin(dapm, "HEADPHONE");
		snd_soc_dapm_enable_pin(dapm, "EAR PA");
		snd_soc_dapm_enable_pin(dapm, "EAR");
        mutex_unlock(&dapm->codec->mutex);
	}

	snd_soc_dapm_sync(dapm);
	return 0;
}

static int taiko_get_iir_enable_audio_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	int iir_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;
	int band_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->shift;

	ucontrol->value.integer.value[0] =
		(snd_soc_read(codec, (TAIKO_A_CDC_IIR1_CTL + 16 * iir_idx)) &
		(1 << band_idx)) != 0;

	pr_debug("%s: IIR #%d band #%d enable %d\n", __func__,
		iir_idx, band_idx,
		(uint32_t)ucontrol->value.integer.value[0]);
	return 0;
}

static int taiko_put_iir_enable_audio_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	int iir_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;
	int band_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->shift;
	int value = ucontrol->value.integer.value[0];

	/* Mask first 5 bits, 6-8 are reserved */
	snd_soc_update_bits(codec, (TAIKO_A_CDC_IIR1_CTL + 16 * iir_idx),
		(1 << band_idx), (value << band_idx));

	pr_debug("%s: IIR #%d band #%d enable %d\n", __func__,
		iir_idx, band_idx,
		((snd_soc_read(codec, (TAIKO_A_CDC_IIR1_CTL + 16 * iir_idx)) &
		(1 << band_idx)) != 0));
	return 0;
}

static uint32_t get_iir_band_coeff(struct snd_soc_codec *codec,
				int iir_idx, int band_idx,
				int coeff_idx)
{
	uint32_t value = 0;

	/* Address does not automatically update if reading */
	snd_soc_write(codec,
		(TAIKO_A_CDC_IIR1_COEF_B1_CTL + 16 * iir_idx),
		((band_idx * BAND_MAX + coeff_idx)
		* sizeof(uint32_t)) & 0x7F);

	value |= snd_soc_read(codec,
		(TAIKO_A_CDC_IIR1_COEF_B2_CTL + 16 * iir_idx));

	snd_soc_write(codec,
		(TAIKO_A_CDC_IIR1_COEF_B1_CTL + 16 * iir_idx),
		((band_idx * BAND_MAX + coeff_idx)
		* sizeof(uint32_t) + 1) & 0x7F);

	value |= (snd_soc_read(codec,
		(TAIKO_A_CDC_IIR1_COEF_B2_CTL + 16 * iir_idx)) << 8);

	snd_soc_write(codec,
		(TAIKO_A_CDC_IIR1_COEF_B1_CTL + 16 * iir_idx),
		((band_idx * BAND_MAX + coeff_idx)
		* sizeof(uint32_t) + 2) & 0x7F);

	value |= (snd_soc_read(codec,
		(TAIKO_A_CDC_IIR1_COEF_B2_CTL + 16 * iir_idx)) << 16);

	snd_soc_write(codec,
		(TAIKO_A_CDC_IIR1_COEF_B1_CTL + 16 * iir_idx),
		((band_idx * BAND_MAX + coeff_idx)
		* sizeof(uint32_t) + 3) & 0x7F);

	/* Mask bits top 2 bits since they are reserved */
	value |= ((snd_soc_read(codec,
		(TAIKO_A_CDC_IIR1_COEF_B2_CTL + 16 * iir_idx)) & 0x3F) << 24);

	return value;
}

static int taiko_get_iir_band_audio_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	int iir_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;
	int band_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->shift;

	ucontrol->value.integer.value[0] =
		get_iir_band_coeff(codec, iir_idx, band_idx, 0);
	ucontrol->value.integer.value[1] =
		get_iir_band_coeff(codec, iir_idx, band_idx, 1);
	ucontrol->value.integer.value[2] =
		get_iir_band_coeff(codec, iir_idx, band_idx, 2);
	ucontrol->value.integer.value[3] =
		get_iir_band_coeff(codec, iir_idx, band_idx, 3);
	ucontrol->value.integer.value[4] =
		get_iir_band_coeff(codec, iir_idx, band_idx, 4);

	return 0;
}

static void set_iir_band_coeff(struct snd_soc_codec *codec,
				int iir_idx, int band_idx,
				uint32_t value)
{
	snd_soc_write(codec,
		(TAIKO_A_CDC_IIR1_COEF_B2_CTL + 16 * iir_idx),
		(value & 0xFF));

	snd_soc_write(codec,
		(TAIKO_A_CDC_IIR1_COEF_B2_CTL + 16 * iir_idx),
		(value >> 8) & 0xFF);

	snd_soc_write(codec,
		(TAIKO_A_CDC_IIR1_COEF_B2_CTL + 16 * iir_idx),
		(value >> 16) & 0xFF);

	/* Mask top 2 bits, 7-8 are reserved */
	snd_soc_write(codec,
		(TAIKO_A_CDC_IIR1_COEF_B2_CTL + 16 * iir_idx),
		(value >> 24) & 0x3F);
}

static int taiko_put_iir_band_audio_mixer(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	int iir_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->reg;
	int band_idx = ((struct soc_multi_mixer_control *)
					kcontrol->private_value)->shift;

	/* Mask top bit it is reserved */
	/* Updates addr automatically for each B2 write */
	snd_soc_write(codec,
		(TAIKO_A_CDC_IIR1_COEF_B1_CTL + 16 * iir_idx),
		(band_idx * BAND_MAX * sizeof(uint32_t)) & 0x7F);

	set_iir_band_coeff(codec, iir_idx, band_idx,
				ucontrol->value.integer.value[0]);
	set_iir_band_coeff(codec, iir_idx, band_idx,
				ucontrol->value.integer.value[1]);
	set_iir_band_coeff(codec, iir_idx, band_idx,
				ucontrol->value.integer.value[2]);
	set_iir_band_coeff(codec, iir_idx, band_idx,
				ucontrol->value.integer.value[3]);
	set_iir_band_coeff(codec, iir_idx, band_idx,
				ucontrol->value.integer.value[4]);

	return 0;
}

static int taiko_get_compander(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{

	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	int comp = ((struct soc_multi_mixer_control *)
		    kcontrol->private_value)->shift;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	if (hph_pa_enabled && comp == COMPANDER_1)
		ucontrol->value.integer.value[0] = 0;
	else
		ucontrol->value.integer.value[0] = taiko->comp_enabled[comp];
	return 0;
}

static int taiko_set_compander(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	int comp = ((struct soc_multi_mixer_control *)
		    kcontrol->private_value)->shift;
	int value = ucontrol->value.integer.value[0];

	if (hph_pa_enabled && comp == COMPANDER_1) {
		/* Wavegen to 20 msec */
		taiko->comp_enabled[comp] = 0;
#ifdef CONFIG_WAVEGEN_OVERRIDE
        if (wavegen_override) {
    		/* Wavegen to 20 msec */
    		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDB);
    		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_TIME, 0x58);
    		snd_soc_write(codec, TAIKO_A_RX_HPH_BIAS_WG_OCP, 0x1A);
        } else {
    		/* Wavegen to 5 msec */
    		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDA);
    		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_TIME, 0x15);
    		snd_soc_write(codec, TAIKO_A_RX_HPH_BIAS_WG_OCP, 0x2A);
        }
#else
   		/* Wavegen to 5 msec */
   		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDA);
   		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_TIME, 0x15);
   		snd_soc_write(codec, TAIKO_A_RX_HPH_BIAS_WG_OCP, 0x2A);
#endif
        write_chopper();
		snd_soc_write(codec, TAIKO_A_NCP_DTEST, 0x10);
		write_hph_poweramp_regs();

		return 0;
	} else
		taiko->comp_enabled[comp] = value;

	if (comp == COMPANDER_1 &&
			taiko->comp_enabled[comp] == 1) {
#ifdef CONFIG_WAVEGEN_OVERRIDE
        if (wavegen_override) {
    		/* Wavegen to 20 msec */
    		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDB);
    		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_TIME, 0x58);
    		snd_soc_write(codec, TAIKO_A_RX_HPH_BIAS_WG_OCP, 0x1A);
        } else {
    		/* Wavegen to 5 msec */
    		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDA);
    		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_TIME, 0x15);
    		snd_soc_write(codec, TAIKO_A_RX_HPH_BIAS_WG_OCP, 0x2A);
        }
#else
   		/* Wavegen to 5 msec */
   		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDA);
   		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_TIME, 0x15);
   		snd_soc_write(codec, TAIKO_A_RX_HPH_BIAS_WG_OCP, 0x2A);
#endif
        write_chopper();
		snd_soc_write(codec, TAIKO_A_NCP_DTEST, 0x20);

		pr_debug("%s: Enabled Chopper and set wavegen to 5 msec\n",
				__func__);

        write_hph_poweramp_regs();
	} else if (comp == COMPANDER_1 &&
			taiko->comp_enabled[comp] == 0) {
		/* Wavegen to 20 msec */
		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDB);
		snd_soc_write(codec, TAIKO_A_RX_HPH_CNP_WG_TIME, 0x58);
		snd_soc_write(codec, TAIKO_A_RX_HPH_BIAS_WG_OCP, 0x1A);

        write_chopper();
		snd_soc_write(codec, TAIKO_A_NCP_DTEST, 0x10);
		pr_debug("%s: Disabled Chopper and set wavegen to 20 msec\n",
				__func__);
		write_hph_poweramp_regs();
	}
	return 0;
}

static int taiko_config_gain_compander(struct snd_soc_codec *codec,
				       int comp, bool enable)
{
	int ret = 0;

	switch (comp) {
	case COMPANDER_0:
		snd_soc_update_bits(codec, TAIKO_A_SPKR_DRV_GAIN,
				    1 << 2, !enable << 2);
		break;
	case COMPANDER_1:
		snd_soc_update_bits(codec, TAIKO_A_RX_HPH_L_GAIN,
				    1 << 5, !enable << 5);
		snd_soc_update_bits(codec, TAIKO_A_RX_HPH_R_GAIN,
				    1 << 5, !enable << 5);
		break;
	case COMPANDER_2:
		snd_soc_update_bits(codec, TAIKO_A_RX_LINE_1_GAIN,
				    1 << 5, !enable << 5);
		snd_soc_update_bits(codec, TAIKO_A_RX_LINE_3_GAIN,
				    1 << 5, !enable << 5);
		snd_soc_update_bits(codec, TAIKO_A_RX_LINE_2_GAIN,
				    1 << 5, !enable << 5);
		snd_soc_update_bits(codec, TAIKO_A_RX_LINE_4_GAIN,
				    1 << 5, !enable << 5);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void taiko_discharge_comp(struct snd_soc_codec *codec, int comp)
{
	/* Level meter DIV Factor to 5*/
	snd_soc_update_bits(codec, TAIKO_A_CDC_COMP0_B2_CTL + (comp * 8), 0xF0,
			    0x05 << 4);
	/* Peak Detection Timeout to 5*/
	snd_soc_update_bits(codec, TAIKO_A_CDC_COMP0_B2_CTL + (comp * 8), 0x0F,
			    0x05);
	/* RMS meter Sampling to 0x01 */
	snd_soc_write(codec, TAIKO_A_CDC_COMP0_B3_CTL + (comp * 8), 0x01);
}

static enum wcd9xxx_buck_volt taiko_codec_get_buck_mv(
	struct snd_soc_codec *codec)
{
	int buck_volt = WCD9XXX_CDC_BUCK_UNSUPPORTED;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	struct wcd9xxx_pdata *pdata = taiko->resmgr.pdata;
	int i;

	for (i = 0; i < ARRAY_SIZE(pdata->regulator); i++) {
		if (!strncmp(pdata->regulator[i].name,
					 WCD9XXX_SUPPLY_BUCK_NAME,
					 sizeof(WCD9XXX_SUPPLY_BUCK_NAME))) {
			if ((pdata->regulator[i].min_uV ==
					WCD9XXX_CDC_BUCK_MV_1P8) ||
				(pdata->regulator[i].min_uV ==
					WCD9XXX_CDC_BUCK_MV_2P15))
				buck_volt = pdata->regulator[i].min_uV;
			break;
		}
	}
    pr_info("%s: Buck mV: %d\n", __func__, buck_volt);
	return buck_volt;
}

static int taiko_config_compander(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	int mask, enable_mask;
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	const int comp = w->shift;
	const u32 rate = taiko->comp_fs[comp];
	const struct comp_sample_dependent_params *comp_params =
	    &comp_samp_params[rate];
	enum wcd9xxx_buck_volt buck_mv;

	/* Compander 0 has single channel */
	mask = (comp == COMPANDER_0 ? 0x01 : 0x03);
	enable_mask = (comp == COMPANDER_0 ? 0x02 : 0x03);
	buck_mv = taiko_codec_get_buck_mv(codec);

	pr_debug("%s: %s event %d compander %d, enabled? %d", __func__,
		 w->name, event, comp, taiko->comp_enabled[comp]);
	if (comp == COMPANDER_1 && hph_pa_enabled) {
        interpolator_enabled = false;
        update_interpolator();
    	/* Disable compander */
		snd_soc_update_bits(codec,
				    TAIKO_A_CDC_COMP0_B1_CTL + (comp * 8),
				    enable_mask, 0x00);

		/* Toggle compander reset bits */
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp],
				    mask << comp_shift[comp]);
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp], 0);

		/* Turn off the clock for compander in pair */
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RX_B2_CTL,
				    mask << comp_shift[comp], 0);

		/* Set gain source to register */
		taiko_config_gain_compander(codec, comp, false);
        if (event == SND_SOC_DAPM_PRE_PMU || event == SND_SOC_DAPM_PRE_PMD)
            write_hph_poweramp_regs();

        return 0;
	}
	
	if (!taiko->comp_enabled[comp])
		return 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* Set compander Sample rate */
		snd_soc_update_bits(codec,
				    TAIKO_A_CDC_COMP0_FS_CFG + (comp * 8),
				    0x07, rate);
		/* Set the static gain offset */
        if (comp != COMPANDER_1){
			snd_soc_update_bits(codec,
					TAIKO_A_CDC_COMP0_B4_CTL + (comp * 8),
					0x80, 0x00);
		} else {
			if (compander_gain_lock) {
				if (compander_gain_boost)
					snd_soc_update_bits(codec,
							TAIKO_A_CDC_COMP0_B4_CTL + (comp * 8),
							0x80, 0x00);
				else
					snd_soc_update_bits(codec,
							TAIKO_A_CDC_COMP0_B4_CTL + (comp * 8),
							0x80, 0x80);
			} else if (buck_mv == WCD9XXX_CDC_BUCK_MV_1P8) {
				snd_soc_update_bits(codec,
						TAIKO_A_CDC_COMP0_B4_CTL + (comp * 8),
						0x80, 0x80);
			} else {
				snd_soc_update_bits(codec,
						TAIKO_A_CDC_COMP0_B4_CTL + (comp * 8),
						0x80, 0x00);
			}
		}
		/* Enable RX interpolation path compander clocks */
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RX_B2_CTL,
				    mask << comp_shift[comp],
				    mask << comp_shift[comp]);
		/* Toggle compander reset bits */
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp],
				    mask << comp_shift[comp]);
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp], 0);

		/* Set gain source to compander */
		taiko_config_gain_compander(codec, comp, true);

		/* Compander enable */
		snd_soc_update_bits(codec, TAIKO_A_CDC_COMP0_B1_CTL +
				    (comp * 8), enable_mask, enable_mask);

		/* Set sample rate dependent paramater */
/*		.peak_det_timeout = 0x0B,
		.rms_meter_div_fact = 0xC,
		.rms_meter_resamp_fact = 0x50,
		u32 sc_peak_det_timeout = 0xB;
		u32 sc_rms_meter_div_fact = 0xD;
		u32 sc_rms_meter_resamp_fact = 0xA0;
*/
        if (comp == COMPANDER_1) {
            interpolator_enabled = true;
            if (interpolator_boost) {
                update_interpolator();
            } else {
        		taiko_discharge_comp(codec, comp);
                /* Worst case timeout for compander CnP sleep timeout */
            	usleep_range(3000, 3100);
    			snd_soc_write(codec, TAIKO_A_CDC_COMP0_B3_CTL + (comp * 8),
                              comp_params->rms_meter_resamp_fact);
    			snd_soc_update_bits(codec,
                                    TAIKO_A_CDC_COMP0_B2_CTL + (comp * 8),
                                    0xF0, (comp_params->rms_meter_div_fact << 4));
    			snd_soc_update_bits(codec,
                                    TAIKO_A_CDC_COMP0_B2_CTL + (comp * 8),
                                    0x0F, comp_params->peak_det_timeout);
            }
		} else {
    		taiko_discharge_comp(codec, comp);
            /* Worst case timeout for compander CnP sleep timeout */
        	usleep_range(3000, 3100);
			snd_soc_write(codec, TAIKO_A_CDC_COMP0_B3_CTL + (comp * 8),
                          comp_params->rms_meter_resamp_fact);
			snd_soc_update_bits(codec,
                                TAIKO_A_CDC_COMP0_B2_CTL + (comp * 8),
                                0xF0, (comp_params->rms_meter_div_fact << 4));
			snd_soc_update_bits(codec,
                                TAIKO_A_CDC_COMP0_B2_CTL + (comp * 8),
                                0x0F, comp_params->peak_det_timeout);
		}
		break;
	case SND_SOC_DAPM_PRE_PMD:
        if (comp == COMPANDER_1) {
            interpolator_enabled = false;
            if (interpolator_boost) {
                update_interpolator();
            } else {
        		taiko_discharge_comp(codec, comp);
                /* Worst case timeout for compander CnP sleep timeout */
            	usleep_range(3000, 3100);
            }
        } else {
            taiko_discharge_comp(codec, comp);
            /* Worst case timeout for compander CnP sleep timeout */
            usleep_range(3000, 3100);
        }
            
		/* Disable compander */
		snd_soc_update_bits(codec,
				    TAIKO_A_CDC_COMP0_B1_CTL + (comp * 8),
				    enable_mask, 0x00);

		/* Toggle compander reset bits */
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp],
				    mask << comp_shift[comp]);
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp], 0);

		/* Turn off the clock for compander in pair */
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RX_B2_CTL,
				    mask << comp_shift[comp], 0);

		/* Set gain source to register */
		taiko_config_gain_compander(codec, comp, false);
		write_hph_poweramp_regs();
		break;
	default:
		break;
	}
	return 0;
}

static const char *taiko_anc_func_text[] = {"OFF", "ON"};
static const struct soc_enum taiko_anc_func_enum =
		SOC_ENUM_SINGLE_EXT(2, taiko_anc_func_text);

static const char *taiko_ear_pa_gain_text[] = {"POS_6_DB", "POS_2_DB"};
static const struct soc_enum taiko_ear_pa_gain_enum[] = {
		SOC_ENUM_SINGLE_EXT(2, taiko_ear_pa_gain_text),
};

/*cut off frequency for high pass filter*/
static const char *cf_text[] = {
	"MIN_3DB_4Hz", "MIN_3DB_75Hz", "MIN_3DB_150Hz"
};

static const struct soc_enum cf_dec1_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX1_MUX_CTL, 4, 3, cf_text);

static const struct soc_enum cf_dec2_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX2_MUX_CTL, 4, 3, cf_text);

static const struct soc_enum cf_dec3_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX3_MUX_CTL, 4, 3, cf_text);

static const struct soc_enum cf_dec4_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX4_MUX_CTL, 4, 3, cf_text);

static const struct soc_enum cf_dec5_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX5_MUX_CTL, 4, 3, cf_text);

static const struct soc_enum cf_dec6_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX6_MUX_CTL, 4, 3, cf_text);

static const struct soc_enum cf_dec7_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX7_MUX_CTL, 4, 3, cf_text);

static const struct soc_enum cf_dec8_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX8_MUX_CTL, 4, 3, cf_text);

static const struct soc_enum cf_dec9_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX9_MUX_CTL, 4, 3, cf_text);

static const struct soc_enum cf_dec10_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_TX10_MUX_CTL, 4, 3, cf_text);
#if 0
static const struct soc_enum cf_rxmix1_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_RX1_B4_CTL, 0, 3, cf_text);

static const struct soc_enum cf_rxmix2_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_RX2_B4_CTL, 0, 3, cf_text);
#endif
static const struct soc_enum cf_rxmix3_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_RX3_B4_CTL, 0, 3, cf_text);

static const struct soc_enum cf_rxmix4_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_RX4_B4_CTL, 0, 3, cf_text);

static const struct soc_enum cf_rxmix5_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_RX5_B4_CTL, 0, 3, cf_text)
;
static const struct soc_enum cf_rxmix6_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_RX6_B4_CTL, 0, 3, cf_text);
#if 0
static const struct soc_enum cf_rxmix7_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_RX7_B4_CTL, 0, 3, cf_text);
#endif
static const char * const class_h_dsm_text[] = {
	"ZERO", "DSM_HPHL_RX1", "DSM_SPKR_RX7"
};

static const struct soc_enum class_h_dsm_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_CLSH_CTL, 4, 3, class_h_dsm_text);

static const struct snd_kcontrol_new class_h_dsm_mux =
	SOC_DAPM_ENUM("CLASS_H_DSM MUX Mux", class_h_dsm_enum);

static const char *rx1_interpolator_text[] = {
	"ZERO", "RX1 MIX1"
};
static const struct soc_enum rx1_interpolator_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CLK_RX_B1_CTL, 0, 2, rx1_interpolator_text);

static const struct snd_kcontrol_new rx1_interpolator =
	SOC_DAPM_ENUM("RX1 INTERP Mux", rx1_interpolator_enum);

static const char *rx2_interpolator_text[] = {
	"ZERO", "RX2 MIX1"
};
static const struct soc_enum rx2_interpolator_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CLK_RX_B1_CTL, 1, 2, rx2_interpolator_text);

static const struct snd_kcontrol_new rx2_interpolator =
	SOC_DAPM_ENUM("RX2 INTERP Mux", rx2_interpolator_enum);

static const char *taiko_conn_mad_text[] = {
	"ADC_MB", "ADC1", "ADC2", "ADC3", "ADC4", "ADC5", "ADC6", "NOTUSED1",
	"DMIC1", "DMIC2", "DMIC3", "DMIC4", "DMIC5", "DMIC6", "NOTUSED2",
	"NOTUSED3"};

static const struct soc_enum taiko_conn_mad_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(taiko_conn_mad_text),
			taiko_conn_mad_text);


static int taiko_mad_input_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	u8 taiko_mad_input;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	taiko_mad_input = snd_soc_read(codec, TAIKO_A_CDC_CONN_MAD);

	taiko_mad_input = taiko_mad_input & 0x0F;

	ucontrol->value.integer.value[0] = taiko_mad_input;

	pr_debug("%s: taiko_mad_input = %s\n", __func__,
			taiko_conn_mad_text[taiko_mad_input]);

	return 0;
}

static int taiko_mad_input_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	u8 taiko_mad_input;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct snd_soc_card *card = codec->card;
	char mad_amic_input_widget[6];
	u32 adc;
	const char *mad_input_widget;
	u32  mic_bias_found = 0;
	u32 i;
	int ret = 0;

	taiko_mad_input = ucontrol->value.integer.value[0];

	if (taiko_mad_input >= ARRAY_SIZE(taiko_conn_mad_text)) {
		dev_err(codec->dev,
			"%s: taiko_mad_input = %d out of bounds\n",
			__func__, taiko_mad_input);
		return -EINVAL;
	}

	pr_debug("%s: taiko_mad_input = %s\n", __func__,
			taiko_conn_mad_text[taiko_mad_input]);

	if (!strcmp(taiko_conn_mad_text[taiko_mad_input], "NOTUSED1") ||
		!strcmp(taiko_conn_mad_text[taiko_mad_input], "NOTUSED2") ||
		!strcmp(taiko_conn_mad_text[taiko_mad_input], "NOTUSED3") ||
		!strcmp(taiko_conn_mad_text[taiko_mad_input], "ADC_MB")) {
		pr_debug("%s: taiko mad input is set to unsupported input = %s\n",
				__func__, taiko_conn_mad_text[taiko_mad_input]);
		return -EINVAL;
	}

	if (strnstr(taiko_conn_mad_text[taiko_mad_input],
				"ADC", sizeof("ADC"))) {
		ret = kstrtouint(strpbrk(taiko_conn_mad_text[taiko_mad_input]
					, "123456"), 10, &adc);
		if ((ret < 0) || (adc > 6)) {
			pr_debug("%s: Invalid ADC = %s\n", __func__,
				taiko_conn_mad_text[taiko_mad_input]);
			ret =  -EINVAL;
		}

		snprintf(mad_amic_input_widget, 6, "%s%u", "AMIC", adc);

		mad_input_widget = mad_amic_input_widget;
		pr_debug("%s: taiko amic input widget = %s\n", __func__,
			  mad_amic_input_widget);
	} else {
		/* DMIC type input widget*/
		mad_input_widget = taiko_conn_mad_text[taiko_mad_input];
	}

	pr_debug("%s: taiko input widget = %s\n", __func__, mad_input_widget);

	for (i = 0; i < card->num_dapm_routes; i++) {

		if (!strncmp(card->dapm_routes[i].sink,
				mad_input_widget, strlen(mad_input_widget))) {

			if (strnstr(card->dapm_routes[i].source,
				"MIC BIAS1", sizeof("MIC BIAS1"))) {
				mic_bias_found = 1;
				break;
			} else if (strnstr(card->dapm_routes[i].source,
				"MIC BIAS2", sizeof("MIC BIAS2"))) {
				mic_bias_found = 2;
				break;
			} else if (strnstr(card->dapm_routes[i].source,
				"MIC BIAS3", sizeof("MIC BIAS3"))) {
				mic_bias_found = 3;
				break;
			} else if (strnstr(card->dapm_routes[i].source,
				"MIC BIAS4", sizeof("MIC BIAS4"))) {
				mic_bias_found = 4;
				break;
			}
		}
	}

	if (mic_bias_found) {
		pr_debug("%s: source mic bias = %s. sink = %s\n", __func__,
				card->dapm_routes[i].source,
				card->dapm_routes[i].sink);

		snd_soc_update_bits(codec, TAIKO_A_CDC_CONN_MAD,
					0x0F, taiko_mad_input);
		snd_soc_update_bits(codec, TAIKO_A_MAD_ANA_CTRL,
					0x07, mic_bias_found);
		return 0;
	} else {
		pr_debug("%s: mic bias source not found for input = %s\n",
				__func__, mad_input_widget);
		return -EINVAL;
	}
}


static const struct snd_kcontrol_new taiko_snd_controls[] = {

	SOC_SINGLE_S8_TLV("RX1 Digital Volume", TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX2 Digital Volume", TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX3 Digital Volume", TAIKO_A_CDC_RX3_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX4 Digital Volume", TAIKO_A_CDC_RX4_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX5 Digital Volume", TAIKO_A_CDC_RX5_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX6 Digital Volume", TAIKO_A_CDC_RX6_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX7 Digital Volume", TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),

	SOC_SINGLE_S8_TLV("DEC1 Volume", TAIKO_A_CDC_TX1_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC2 Volume", TAIKO_A_CDC_TX2_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC3 Volume", TAIKO_A_CDC_TX3_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC4 Volume", TAIKO_A_CDC_TX4_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC5 Volume", TAIKO_A_CDC_TX5_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC6 Volume", TAIKO_A_CDC_TX6_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC7 Volume", TAIKO_A_CDC_TX7_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC8 Volume", TAIKO_A_CDC_TX8_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC9 Volume", TAIKO_A_CDC_TX9_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC10 Volume", TAIKO_A_CDC_TX10_VOL_CTL_GAIN, -84,
		40, digital_gain),

	SOC_SINGLE_S8_TLV("IIR1 INP1 Volume", TAIKO_A_CDC_IIR1_GAIN_B1_CTL, -84,
		40, digital_gain),
	SOC_SINGLE_S8_TLV("IIR1 INP2 Volume", TAIKO_A_CDC_IIR1_GAIN_B2_CTL, -84,
		40, digital_gain),
	SOC_SINGLE_S8_TLV("IIR1 INP3 Volume", TAIKO_A_CDC_IIR1_GAIN_B3_CTL, -84,
		40, digital_gain),
	SOC_SINGLE_S8_TLV("IIR1 INP4 Volume", TAIKO_A_CDC_IIR1_GAIN_B4_CTL, -84,
		40, digital_gain),
	SOC_SINGLE_S8_TLV("IIR2 INP1 Volume", TAIKO_A_CDC_IIR2_GAIN_B1_CTL, -84,
		40, digital_gain),
	SOC_SINGLE_S8_TLV("IIR2 INP2 Volume", TAIKO_A_CDC_IIR2_GAIN_B2_CTL, -84,
		40, digital_gain),
	SOC_SINGLE_S8_TLV("IIR2 INP3 Volume", TAIKO_A_CDC_IIR2_GAIN_B3_CTL, -84,
		40, digital_gain),
	SOC_SINGLE_S8_TLV("IIR2 INP4 Volume", TAIKO_A_CDC_IIR2_GAIN_B4_CTL, -84,
		40, digital_gain),

	SOC_SINGLE_EXT("ANC Slot", SND_SOC_NOPM, 0, 100, 0, taiko_get_anc_slot,
		taiko_put_anc_slot),
	SOC_ENUM_EXT("ANC Function", taiko_anc_func_enum, taiko_get_anc_func,
		taiko_put_anc_func),

	SOC_ENUM("TX1 HPF cut off", cf_dec1_enum),
	SOC_ENUM("TX2 HPF cut off", cf_dec2_enum),
	SOC_ENUM("TX3 HPF cut off", cf_dec3_enum),
	SOC_ENUM("TX4 HPF cut off", cf_dec4_enum),
	SOC_ENUM("TX5 HPF cut off", cf_dec5_enum),
	SOC_ENUM("TX6 HPF cut off", cf_dec6_enum),
	SOC_ENUM("TX7 HPF cut off", cf_dec7_enum),
	SOC_ENUM("TX8 HPF cut off", cf_dec8_enum),
	SOC_ENUM("TX9 HPF cut off", cf_dec9_enum),
	SOC_ENUM("TX10 HPF cut off", cf_dec10_enum),

	SOC_SINGLE("TX1 HPF Switch", TAIKO_A_CDC_TX1_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX2 HPF Switch", TAIKO_A_CDC_TX2_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX3 HPF Switch", TAIKO_A_CDC_TX3_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX4 HPF Switch", TAIKO_A_CDC_TX4_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX5 HPF Switch", TAIKO_A_CDC_TX5_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX6 HPF Switch", TAIKO_A_CDC_TX6_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX7 HPF Switch", TAIKO_A_CDC_TX7_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX8 HPF Switch", TAIKO_A_CDC_TX8_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX9 HPF Switch", TAIKO_A_CDC_TX9_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX10 HPF Switch", TAIKO_A_CDC_TX10_MUX_CTL, 3, 1, 0),

//	SOC_SINGLE("RX1 HPF Switch", TAIKO_A_CDC_RX1_B5_CTL, 2, 1, 0),
//	SOC_SINGLE("RX2 HPF Switch", TAIKO_A_CDC_RX2_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX3 HPF Switch", TAIKO_A_CDC_RX3_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX4 HPF Switch", TAIKO_A_CDC_RX4_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX5 HPF Switch", TAIKO_A_CDC_RX5_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX6 HPF Switch", TAIKO_A_CDC_RX6_B5_CTL, 2, 1, 0),
//	SOC_SINGLE("RX7 HPF Switch", TAIKO_A_CDC_RX7_B5_CTL, 2, 1, 0),

//	SOC_ENUM("RX1 HPF cut off", cf_rxmix1_enum),
//	SOC_ENUM("RX2 HPF cut off", cf_rxmix2_enum),
	SOC_ENUM("RX3 HPF cut off", cf_rxmix3_enum),
	SOC_ENUM("RX4 HPF cut off", cf_rxmix4_enum),
	SOC_ENUM("RX5 HPF cut off", cf_rxmix5_enum),
	SOC_ENUM("RX6 HPF cut off", cf_rxmix6_enum),
//	SOC_ENUM("RX7 HPF cut off", cf_rxmix7_enum),
/*
#define SOC_SINGLE_EXT(xname, xreg, xshift, xmax, xinvert,\
	 xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = SOC_SINGLE_VALUE(xreg, xshift, xmax, xinvert) }
*/
	SOC_SINGLE_EXT("IIR1 Enable Band1", IIR1, BAND1, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
	SOC_SINGLE_EXT("IIR1 Enable Band2", IIR1, BAND2, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
	SOC_SINGLE_EXT("IIR1 Enable Band3", IIR1, BAND3, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
	SOC_SINGLE_EXT("IIR1 Enable Band4", IIR1, BAND4, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
	SOC_SINGLE_EXT("IIR1 Enable Band5", IIR1, BAND5, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
	SOC_SINGLE_EXT("IIR2 Enable Band1", IIR2, BAND1, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
	SOC_SINGLE_EXT("IIR2 Enable Band2", IIR2, BAND2, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
	SOC_SINGLE_EXT("IIR2 Enable Band3", IIR2, BAND3, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
	SOC_SINGLE_EXT("IIR2 Enable Band4", IIR2, BAND4, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
	SOC_SINGLE_EXT("IIR2 Enable Band5", IIR2, BAND5, 1, 0,
	taiko_get_iir_enable_audio_mixer, taiko_put_iir_enable_audio_mixer),
/*
#define SOC_SINGLE_MULTI_EXT(xname, xreg, xshift, xmax, xinvert, xcount,\
	xhandler_get, xhandler_put) \
{      .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_multi_ext, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = (unsigned long)&(struct soc_multi_mixer_control) \
		{.reg = xreg, .shift = xshift, .rshift = xshift, .max = xmax, \
		.count = xcount, .platform_max = xmax, .invert = xinvert} }
*/
	SOC_SINGLE_MULTI_EXT("IIR1 Band1", IIR1, BAND1, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("IIR1 Band2", IIR1, BAND2, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("IIR1 Band3", IIR1, BAND3, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("IIR1 Band4", IIR1, BAND4, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("IIR1 Band5", IIR1, BAND5, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("IIR2 Band1", IIR2, BAND1, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("IIR2 Band2", IIR2, BAND2, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("IIR2 Band3", IIR2, BAND3, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("IIR2 Band4", IIR2, BAND4, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),
	SOC_SINGLE_MULTI_EXT("IIR2 Band5", IIR2, BAND5, 255, 0, 5,
	taiko_get_iir_band_audio_mixer, taiko_put_iir_band_audio_mixer),

	SOC_SINGLE_EXT("COMP0 Switch", SND_SOC_NOPM, COMPANDER_0, 1, 0,
		       taiko_get_compander, taiko_set_compander),
	SOC_SINGLE_EXT("COMP1 Switch", SND_SOC_NOPM, COMPANDER_1, 1, 0,
		       taiko_get_compander, taiko_set_compander),
	SOC_SINGLE_EXT("COMP2 Switch", SND_SOC_NOPM, COMPANDER_2, 1, 0,
		       taiko_get_compander, taiko_set_compander),

	SOC_ENUM_EXT("MAD Input", taiko_conn_mad_enum,
			taiko_mad_input_get, taiko_mad_input_put),

};

static int taiko_pa_gain_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u8 ear_pa_gain;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	ear_pa_gain = snd_soc_read(codec, TAIKO_A_RX_EAR_GAIN);

	ear_pa_gain = ear_pa_gain >> 5;

	ucontrol->value.integer.value[0] = ear_pa_gain;

	pr_debug("%s: ear_pa_gain: hex:0x%x dec:%u\n", __func__, ear_pa_gain, ear_pa_gain);

	return 0;
}

static int taiko_pa_gain_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u8 ear_pa_gain;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	pr_debug("%s: ucontrol->value.integer.value[0]  = %ld\n", __func__,
			ucontrol->value.integer.value[0]);

	ear_pa_gain =  ucontrol->value.integer.value[0] << 5;

	snd_soc_update_bits(codec, TAIKO_A_RX_EAR_GAIN, 0xE0, ear_pa_gain);
	return 0;
}

static const char * const taiko_2_x_ear_pa_gain_text[] = {
	"POS_6_DB", "POS_4P5_DB", "POS_3_DB", "POS_1P5_DB",
	"POS_0_DB", "NEG_2P5_DB", "UNDEFINED", "NEG_12_DB"
};

static const struct soc_enum taiko_2_x_ear_pa_gain_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(taiko_2_x_ear_pa_gain_text),
			taiko_2_x_ear_pa_gain_text);

static const struct snd_kcontrol_new taiko_2_x_analog_gain_controls[] = {

	SOC_ENUM_EXT("EAR PA Gain", taiko_2_x_ear_pa_gain_enum,
		taiko_pa_gain_get, taiko_pa_gain_put),

	SOC_SINGLE_TLV("HPHL Volume", WCD9XXX_A_RX_HPH_L_GAIN, 0, 20, 1,
		line_gain),
	SOC_SINGLE_TLV("HPHR Volume", WCD9XXX_A_RX_HPH_R_GAIN, 0, 20, 1,
		line_gain),

	SOC_SINGLE_TLV("LINEOUT1 Volume", TAIKO_A_RX_LINE_1_GAIN, 0, 20, 1,
		line_gain),
	SOC_SINGLE_TLV("LINEOUT2 Volume", TAIKO_A_RX_LINE_2_GAIN, 0, 20, 1,
		line_gain),
	SOC_SINGLE_TLV("LINEOUT3 Volume", TAIKO_A_RX_LINE_3_GAIN, 0, 20, 1,
		line_gain),
	SOC_SINGLE_TLV("LINEOUT4 Volume", TAIKO_A_RX_LINE_4_GAIN, 0, 20, 1,
		line_gain),

	SOC_SINGLE_TLV("SPK DRV Volume", TAIKO_A_SPKR_DRV_GAIN, 3, 8, 1,
		line_gain),

	SOC_SINGLE_TLV("ADC1 Volume", TAIKO_A_CDC_TX_1_GAIN, 2, 19, 0,
			analog_gain),
	SOC_SINGLE_TLV("ADC2 Volume", TAIKO_A_CDC_TX_2_GAIN, 2, 19, 0,
			analog_gain),
	SOC_SINGLE_TLV("ADC3 Volume", TAIKO_A_CDC_TX_3_GAIN, 2, 19, 0,
			analog_gain),
	SOC_SINGLE_TLV("ADC4 Volume", TAIKO_A_CDC_TX_4_GAIN, 2, 19, 0,
			analog_gain),
	SOC_SINGLE_TLV("ADC5 Volume", TAIKO_A_CDC_TX_5_GAIN, 2, 19, 0,
			analog_gain),
	SOC_SINGLE_TLV("ADC6 Volume", TAIKO_A_CDC_TX_6_GAIN, 2, 19, 0,
			analog_gain),
};

static int taiko_hph_impedance_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
#if !defined(CONFIG_SAMSUNG_JACK) && !defined(CONFIG_MUIC_DET_JACK)
	uint32_t zl, zr;
	bool hphr;
	struct soc_multi_mixer_control *mc;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct taiko_priv *priv = snd_soc_codec_get_drvdata(codec);

	mc = (struct soc_multi_mixer_control *)(kcontrol->private_value);

	hphr = mc->shift;
	wcd9xxx_mbhc_get_impedance(&priv->mbhc, &zl, &zr);
	pr_debug("%s: zl %u, zr %u\n", __func__, zl, zr);
	ucontrol->value.integer.value[0] = hphr ? zr : zl;
#endif
	ucontrol->value.integer.value[0] = 0;
	return 0;
}

static const struct snd_kcontrol_new impedance_detect_controls[] = {
	SOC_SINGLE_EXT("HPHL Impedance", 0, 0, UINT_MAX, 0,
		       taiko_hph_impedance_get, NULL),
	SOC_SINGLE_EXT("HPHR Impedance", 0, 1, UINT_MAX, 0,
		       taiko_hph_impedance_get, NULL),
};

static const char * const rx_mix1_text[] = {
	"ZERO", "SRC1", "SRC2", "IIR1", "IIR2", "RX1", "RX2", "RX3", "RX4",
		"RX5", "RX6", "RX7"
};

static const char * const rx_mix2_text[] = {
	"ZERO", "SRC1", "SRC2", "IIR1", "IIR2", "RX1", "RX2", "RX3", "RX4",
		"RX5", "RX6", "RX7"
};

static const char * const rx_rdac5_text[] = {
	"DEM4", "DEM3_INV"
};

static const char * const rx_rdac7_text[] = {
	"DEM6", "DEM5_INV"
};


static const char * const sb_tx1_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC1"
};

static const char * const sb_tx2_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC2"
};

static const char * const sb_tx3_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC3"
};

static const char * const sb_tx4_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC4"
};

static const char * const sb_tx5_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC5"
};

static const char * const sb_tx6_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC6"
};

static const char * const sb_tx7_to_tx10_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
		"DEC9", "DEC10"
};

static const char * const dec1_mux_text[] = {
	"ZERO", "DMIC1", "ADC6",
};

static const char * const dec2_mux_text[] = {
	"ZERO", "DMIC2", "ADC5",
};

static const char * const dec3_mux_text[] = {
	"ZERO", "DMIC3", "ADC4",
};

static const char * const dec4_mux_text[] = {
	"ZERO", "DMIC4", "ADC3",
};

static const char * const dec5_mux_text[] = {
	"ZERO", "DMIC5", "ADC2",
};

static const char * const dec6_mux_text[] = {
	"ZERO", "DMIC6", "ADC1",
};

static const char * const dec7_mux_text[] = {
	"ZERO", "DMIC1", "DMIC6", "ADC1", "ADC6", "ANC1_FB", "ANC2_FB",
};

static const char * const dec8_mux_text[] = {
	"ZERO", "DMIC2", "DMIC5", "ADC2", "ADC5",
};

static const char * const dec9_mux_text[] = {
	"ZERO", "DMIC4", "DMIC5", "ADC2", "ADC3", "ADCMB", "ANC1_FB", "ANC2_FB",
};

static const char * const dec10_mux_text[] = {
	"ZERO", "DMIC3", "DMIC6", "ADC1", "ADC4", "ADCMB", "ANC1_FB", "ANC2_FB",
};

static const char * const anc_mux_text[] = {
	"ZERO", "ADC1", "ADC2", "ADC3", "ADC4", "ADC5", "ADC6", "ADC_MB",
		"RSVD_1", "DMIC1", "DMIC2", "DMIC3", "DMIC4", "DMIC5", "DMIC6"
};

static const char * const anc1_fb_mux_text[] = {
	"ZERO", "EAR_HPH_L", "EAR_LINE_1",
};

static const char * const iir_inp1_text[] = {
	"ZERO", "DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
	"DEC9", "DEC10", "RX1", "RX2", "RX3", "RX4", "RX5", "RX6", "RX7"
};

static const char * const iir_inp2_text[] = {
	"ZERO", "DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
	"DEC9", "DEC10", "RX1", "RX2", "RX3", "RX4", "RX5", "RX6", "RX7"
};

static const char * const iir_inp3_text[] = {
	"ZERO", "DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
	"DEC9", "DEC10", "RX1", "RX2", "RX3", "RX4", "RX5", "RX6", "RX7"
};

static const char * const iir_inp4_text[] = {
	"ZERO", "DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
	"DEC9", "DEC10", "RX1", "RX2", "RX3", "RX4", "RX5", "RX6", "RX7"
};

static const struct soc_enum rx_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX1_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX1_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx_mix1_inp3_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX1_B2_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx2_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX2_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx2_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX2_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx2_mix1_inp3_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX2_B2_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx3_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX3_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx3_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX3_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx4_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX4_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx4_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX4_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx5_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX5_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx5_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX5_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx6_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX6_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx6_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX6_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx7_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX7_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx7_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX7_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx1_mix2_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX1_B3_CTL, 0, 12, rx_mix2_text);

static const struct soc_enum rx1_mix2_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX1_B3_CTL, 3, 12, rx_mix2_text);

static const struct soc_enum rx2_mix2_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX2_B3_CTL, 0, 12, rx_mix2_text);

static const struct soc_enum rx2_mix2_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX2_B3_CTL, 3, 12, rx_mix2_text);

static const struct soc_enum rx7_mix2_inp1_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX7_B3_CTL, 0, 12, rx_mix2_text);

static const struct soc_enum rx7_mix2_inp2_chain_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_RX7_B3_CTL, 3, 12, rx_mix2_text);

static const struct soc_enum rx_rdac5_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_MISC, 2, 2, rx_rdac5_text);

static const struct soc_enum rx_rdac7_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_MISC, 1, 2, rx_rdac7_text);

static const struct soc_enum sb_tx1_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B1_CTL, 0, 9, sb_tx1_mux_text);

static const struct soc_enum sb_tx2_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B2_CTL, 0, 9, sb_tx2_mux_text);

static const struct soc_enum sb_tx3_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B3_CTL, 0, 9, sb_tx3_mux_text);

static const struct soc_enum sb_tx4_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B4_CTL, 0, 9, sb_tx4_mux_text);

static const struct soc_enum sb_tx5_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B5_CTL, 0, 9, sb_tx5_mux_text);

static const struct soc_enum sb_tx6_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B6_CTL, 0, 9, sb_tx6_mux_text);

static const struct soc_enum sb_tx7_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B7_CTL, 0, 18,
			sb_tx7_to_tx10_mux_text);

static const struct soc_enum sb_tx8_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B8_CTL, 0, 18,
			sb_tx7_to_tx10_mux_text);

static const struct soc_enum sb_tx9_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B9_CTL, 0, 18,
			sb_tx7_to_tx10_mux_text);

static const struct soc_enum sb_tx10_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_SB_B10_CTL, 0, 18,
			sb_tx7_to_tx10_mux_text);

static const struct soc_enum dec1_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B1_CTL, 0, 3, dec1_mux_text);

static const struct soc_enum dec2_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B1_CTL, 2, 3, dec2_mux_text);

static const struct soc_enum dec3_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B1_CTL, 4, 3, dec3_mux_text);

static const struct soc_enum dec4_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B1_CTL, 6, 3, dec4_mux_text);

static const struct soc_enum dec5_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B2_CTL, 0, 3, dec5_mux_text);

static const struct soc_enum dec6_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B2_CTL, 2, 3, dec6_mux_text);

static const struct soc_enum dec7_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B2_CTL, 4, 7, dec7_mux_text);

static const struct soc_enum dec8_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B3_CTL, 0, 7, dec8_mux_text);

static const struct soc_enum dec9_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B3_CTL, 3, 8, dec9_mux_text);

static const struct soc_enum dec10_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_TX_B4_CTL, 0, 8, dec10_mux_text);

static const struct soc_enum anc1_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_ANC_B1_CTL, 0, 16, anc_mux_text);

static const struct soc_enum anc2_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_ANC_B1_CTL, 4, 16, anc_mux_text);

static const struct soc_enum anc1_fb_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_ANC_B2_CTL, 0, 3, anc1_fb_mux_text);

static const struct soc_enum iir1_inp1_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ1_B1_CTL, 0, 18, iir_inp1_text);

static const struct soc_enum iir1_inp2_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ1_B2_CTL, 0, 18, iir_inp2_text);

static const struct soc_enum iir1_inp3_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ1_B3_CTL, 0, 18, iir_inp3_text);

static const struct soc_enum iir1_inp4_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ1_B4_CTL, 0, 18, iir_inp4_text);

static const struct soc_enum iir2_inp1_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ2_B1_CTL, 0, 18, iir_inp1_text);

static const struct soc_enum iir2_inp2_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ2_B2_CTL, 0, 18, iir_inp2_text);

static const struct soc_enum iir2_inp3_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ2_B3_CTL, 0, 18, iir_inp3_text);

static const struct soc_enum iir2_inp4_mux_enum =
	SOC_ENUM_SINGLE(TAIKO_A_CDC_CONN_EQ2_B4_CTL, 0, 18, iir_inp4_text);

static const struct snd_kcontrol_new rx_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX1 MIX1 INP1 Mux", rx_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX1 MIX1 INP2 Mux", rx_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx_mix1_inp3_mux =
	SOC_DAPM_ENUM("RX1 MIX1 INP3 Mux", rx_mix1_inp3_chain_enum);

static const struct snd_kcontrol_new rx2_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX2 MIX1 INP1 Mux", rx2_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx2_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX2 MIX1 INP2 Mux", rx2_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx2_mix1_inp3_mux =
	SOC_DAPM_ENUM("RX2 MIX1 INP3 Mux", rx2_mix1_inp3_chain_enum);

static const struct snd_kcontrol_new rx3_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX3 MIX1 INP1 Mux", rx3_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx3_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX3 MIX1 INP2 Mux", rx3_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx4_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX4 MIX1 INP1 Mux", rx4_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx4_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX4 MIX1 INP2 Mux", rx4_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx5_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX5 MIX1 INP1 Mux", rx5_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx5_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX5 MIX1 INP2 Mux", rx5_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx6_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX6 MIX1 INP1 Mux", rx6_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx6_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX6 MIX1 INP2 Mux", rx6_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx7_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX7 MIX1 INP1 Mux", rx7_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx7_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX7 MIX1 INP2 Mux", rx7_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx1_mix2_inp1_mux =
	SOC_DAPM_ENUM("RX1 MIX2 INP1 Mux", rx1_mix2_inp1_chain_enum);

static const struct snd_kcontrol_new rx1_mix2_inp2_mux =
	SOC_DAPM_ENUM("RX1 MIX2 INP2 Mux", rx1_mix2_inp2_chain_enum);

static const struct snd_kcontrol_new rx2_mix2_inp1_mux =
	SOC_DAPM_ENUM("RX2 MIX2 INP1 Mux", rx2_mix2_inp1_chain_enum);

static const struct snd_kcontrol_new rx2_mix2_inp2_mux =
	SOC_DAPM_ENUM("RX2 MIX2 INP2 Mux", rx2_mix2_inp2_chain_enum);

static const struct snd_kcontrol_new rx7_mix2_inp1_mux =
	SOC_DAPM_ENUM("RX7 MIX2 INP1 Mux", rx7_mix2_inp1_chain_enum);

static const struct snd_kcontrol_new rx7_mix2_inp2_mux =
	SOC_DAPM_ENUM("RX7 MIX2 INP2 Mux", rx7_mix2_inp2_chain_enum);

static const struct snd_kcontrol_new rx_dac5_mux =
	SOC_DAPM_ENUM("RDAC5 MUX Mux", rx_rdac5_enum);

static const struct snd_kcontrol_new rx_dac7_mux =
	SOC_DAPM_ENUM("RDAC7 MUX Mux", rx_rdac7_enum);

static const struct snd_kcontrol_new sb_tx1_mux =
	SOC_DAPM_ENUM("SLIM TX1 MUX Mux", sb_tx1_mux_enum);

static const struct snd_kcontrol_new sb_tx2_mux =
	SOC_DAPM_ENUM("SLIM TX2 MUX Mux", sb_tx2_mux_enum);

static const struct snd_kcontrol_new sb_tx3_mux =
	SOC_DAPM_ENUM("SLIM TX3 MUX Mux", sb_tx3_mux_enum);

static const struct snd_kcontrol_new sb_tx4_mux =
	SOC_DAPM_ENUM("SLIM TX4 MUX Mux", sb_tx4_mux_enum);

static const struct snd_kcontrol_new sb_tx5_mux =
	SOC_DAPM_ENUM("SLIM TX5 MUX Mux", sb_tx5_mux_enum);

static const struct snd_kcontrol_new sb_tx6_mux =
	SOC_DAPM_ENUM("SLIM TX6 MUX Mux", sb_tx6_mux_enum);

static const struct snd_kcontrol_new sb_tx7_mux =
	SOC_DAPM_ENUM("SLIM TX7 MUX Mux", sb_tx7_mux_enum);

static const struct snd_kcontrol_new sb_tx8_mux =
	SOC_DAPM_ENUM("SLIM TX8 MUX Mux", sb_tx8_mux_enum);

static const struct snd_kcontrol_new sb_tx9_mux =
	SOC_DAPM_ENUM("SLIM TX9 MUX Mux", sb_tx9_mux_enum);

static const struct snd_kcontrol_new sb_tx10_mux =
	SOC_DAPM_ENUM("SLIM TX10 MUX Mux", sb_tx10_mux_enum);


static int wcd9320_put_dec_enum(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget_list *wlist = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_widget *w = wlist->widgets[0];
	struct snd_soc_codec *codec = w->codec;
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int dec_mux, decimator;
	char *dec_name = NULL;
	char *widget_name = NULL;
	char *temp;
	u16 tx_mux_ctl_reg;
	u8 adc_dmic_sel = 0x0;
	int ret = 0;

	if (ucontrol->value.enumerated.item[0] > e->max - 1)
		return -EINVAL;

	dec_mux = ucontrol->value.enumerated.item[0];

	widget_name = kstrndup(w->name, 15, GFP_KERNEL);
	if (!widget_name)
		return -ENOMEM;
	temp = widget_name;

	dec_name = strsep(&widget_name, " ");
	widget_name = temp;
	if (!dec_name) {
		pr_debug("%s: Invalid decimator = %s\n", __func__, w->name);
		ret =  -EINVAL;
		goto out;
	}

	ret = kstrtouint(strpbrk(dec_name, "123456789"), 10, &decimator);
	if (ret < 0) {
		pr_debug("%s: Invalid decimator = %s\n", __func__, dec_name);
		ret =  -EINVAL;
		goto out;
	}

	dev_dbg(w->dapm->dev, "%s(): widget = %s decimator = %u dec_mux = %u\n"
		, __func__, w->name, decimator, dec_mux);


	switch (decimator) {
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
		if (dec_mux == 1)
			adc_dmic_sel = 0x1;
		else
			adc_dmic_sel = 0x0;
		break;
	case 7:
	case 8:
	case 9:
	case 10:
		if ((dec_mux == 1) || (dec_mux == 2))
			adc_dmic_sel = 0x1;
		else
			adc_dmic_sel = 0x0;
		break;
	default:
		pr_debug("%s: Invalid Decimator = %u\n", __func__, decimator);
		ret = -EINVAL;
		goto out;
	}

	tx_mux_ctl_reg = TAIKO_A_CDC_TX1_MUX_CTL + 8 * (decimator - 1);

	snd_soc_update_bits(codec, tx_mux_ctl_reg, 0x1, adc_dmic_sel);

	ret = snd_soc_dapm_put_enum_double(kcontrol, ucontrol);

out:
	kfree(widget_name);
	return ret;
}

#define WCD9320_DEC_ENUM(xname, xenum) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_enum_double, \
	.get = snd_soc_dapm_get_enum_double, \
	.put = wcd9320_put_dec_enum, \
	.private_value = (unsigned long)&xenum }

static const struct snd_kcontrol_new dec1_mux =
	WCD9320_DEC_ENUM("DEC1 MUX Mux", dec1_mux_enum);

static const struct snd_kcontrol_new dec2_mux =
	WCD9320_DEC_ENUM("DEC2 MUX Mux", dec2_mux_enum);

static const struct snd_kcontrol_new dec3_mux =
	WCD9320_DEC_ENUM("DEC3 MUX Mux", dec3_mux_enum);

static const struct snd_kcontrol_new dec4_mux =
	WCD9320_DEC_ENUM("DEC4 MUX Mux", dec4_mux_enum);

static const struct snd_kcontrol_new dec5_mux =
	WCD9320_DEC_ENUM("DEC5 MUX Mux", dec5_mux_enum);

static const struct snd_kcontrol_new dec6_mux =
	WCD9320_DEC_ENUM("DEC6 MUX Mux", dec6_mux_enum);

static const struct snd_kcontrol_new dec7_mux =
	WCD9320_DEC_ENUM("DEC7 MUX Mux", dec7_mux_enum);

static const struct snd_kcontrol_new dec8_mux =
	WCD9320_DEC_ENUM("DEC8 MUX Mux", dec8_mux_enum);

static const struct snd_kcontrol_new dec9_mux =
	WCD9320_DEC_ENUM("DEC9 MUX Mux", dec9_mux_enum);

static const struct snd_kcontrol_new dec10_mux =
	WCD9320_DEC_ENUM("DEC10 MUX Mux", dec10_mux_enum);

static const struct snd_kcontrol_new iir1_inp1_mux =
	SOC_DAPM_ENUM("IIR1 INP1 Mux", iir1_inp1_mux_enum);

static const struct snd_kcontrol_new iir1_inp2_mux =
	SOC_DAPM_ENUM("IIR1 INP2 Mux", iir1_inp2_mux_enum);

static const struct snd_kcontrol_new iir1_inp3_mux =
	SOC_DAPM_ENUM("IIR1 INP3 Mux", iir1_inp3_mux_enum);

static const struct snd_kcontrol_new iir1_inp4_mux =
	SOC_DAPM_ENUM("IIR1 INP4 Mux", iir1_inp4_mux_enum);

static const struct snd_kcontrol_new iir2_inp1_mux =
	SOC_DAPM_ENUM("IIR2 INP1 Mux", iir2_inp1_mux_enum);

static const struct snd_kcontrol_new iir2_inp2_mux =
	SOC_DAPM_ENUM("IIR2 INP2 Mux", iir2_inp2_mux_enum);

static const struct snd_kcontrol_new iir2_inp3_mux =
	SOC_DAPM_ENUM("IIR2 INP3 Mux", iir2_inp3_mux_enum);

static const struct snd_kcontrol_new iir2_inp4_mux =
	SOC_DAPM_ENUM("IIR2 INP4 Mux", iir2_inp4_mux_enum);

static const struct snd_kcontrol_new anc1_mux =
	SOC_DAPM_ENUM("ANC1 MUX Mux", anc1_mux_enum);

static const struct snd_kcontrol_new anc2_mux =
	SOC_DAPM_ENUM("ANC2 MUX Mux", anc2_mux_enum);

static const struct snd_kcontrol_new anc1_fb_mux =
	SOC_DAPM_ENUM("ANC1 FB MUX Mux", anc1_fb_mux_enum);

static const struct snd_kcontrol_new dac1_switch[] = {
	SOC_DAPM_SINGLE("Switch", TAIKO_A_RX_EAR_EN, 5, 1, 0)
};
static const struct snd_kcontrol_new hphl_switch[] = {
	SOC_DAPM_SINGLE("Switch", TAIKO_A_RX_HPH_L_DAC_CTL, 6, 1, 0)
};

static const struct snd_kcontrol_new hphl_pa_mix[] = {
	SOC_DAPM_SINGLE("AUX_PGA_L Switch", TAIKO_A_RX_PA_AUX_IN_CONN,
					7, 1, 0),
};

static const struct snd_kcontrol_new hphr_pa_mix[] = {
	SOC_DAPM_SINGLE("AUX_PGA_R Switch", TAIKO_A_RX_PA_AUX_IN_CONN,
					6, 1, 0),
};

static const struct snd_kcontrol_new ear_pa_mix[] = {
	SOC_DAPM_SINGLE("AUX_PGA_L Switch", TAIKO_A_RX_PA_AUX_IN_CONN,
					5, 1, 0),
};
static const struct snd_kcontrol_new lineout1_pa_mix[] = {
	SOC_DAPM_SINGLE("AUX_PGA_L Switch", TAIKO_A_RX_PA_AUX_IN_CONN,
					4, 1, 0),
};

static const struct snd_kcontrol_new lineout2_pa_mix[] = {
	SOC_DAPM_SINGLE("AUX_PGA_R Switch", TAIKO_A_RX_PA_AUX_IN_CONN,
					3, 1, 0),
};

static const struct snd_kcontrol_new lineout3_pa_mix[] = {
	SOC_DAPM_SINGLE("AUX_PGA_L Switch", TAIKO_A_RX_PA_AUX_IN_CONN,
					2, 1, 0),
};

static const struct snd_kcontrol_new lineout4_pa_mix[] = {
	SOC_DAPM_SINGLE("AUX_PGA_R Switch", TAIKO_A_RX_PA_AUX_IN_CONN,
					1, 1, 0),
};

static const struct snd_kcontrol_new lineout3_ground_switch =
	SOC_DAPM_SINGLE("Switch", TAIKO_A_RX_LINE_3_DAC_CTL, 6, 1, 0);

static const struct snd_kcontrol_new lineout4_ground_switch =
	SOC_DAPM_SINGLE("Switch", TAIKO_A_RX_LINE_4_DAC_CTL, 6, 1, 0);

static const struct snd_kcontrol_new aif4_mad_switch =
	SOC_DAPM_SINGLE("Switch", TAIKO_A_CDC_CLK_OTHR_CTL, 4, 1, 0);

static const struct snd_kcontrol_new aif4_vi_switch =
	SOC_DAPM_SINGLE("Switch", TAIKO_A_SPKR_PROT_EN, 3, 1, 0);
/* virtual port entries */
static int slim_tx_mixer_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget_list *wlist = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_widget *widget = wlist->widgets[0];

	ucontrol->value.integer.value[0] = widget->value;
	return 0;
}

static int slim_tx_mixer_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget_list *wlist = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_widget *widget = wlist->widgets[0];
	struct snd_soc_codec *codec = widget->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);
	struct wcd9xxx *core = dev_get_drvdata(codec->dev->parent);
	struct soc_multi_mixer_control *mixer =
		((struct soc_multi_mixer_control *)kcontrol->private_value);
	u32 dai_id = widget->shift;
	u32 port_id = mixer->shift;
	u32 enable = ucontrol->value.integer.value[0];
	u32 vtable = vport_check_table[dai_id];


	pr_debug("%s: wname %s cname %s value %u shift %d item %ld\n", __func__,
		widget->name, ucontrol->id.name, widget->value, widget->shift,
		ucontrol->value.integer.value[0]);

	mutex_lock(&codec->mutex);

	if (taiko_p->intf_type != WCD9XXX_INTERFACE_TYPE_SLIMBUS) {
		if (dai_id != AIF1_CAP) {
			dev_dbg(codec->dev, "%s: invalid AIF for I2C mode\n",
				__func__);
			mutex_unlock(&codec->mutex);
			return -EINVAL;
		}
	}
		switch (dai_id) {
		case AIF1_CAP:
		case AIF2_CAP:
		case AIF3_CAP:
			/* only add to the list if value not set
			 */
			if (enable && !(widget->value & 1 << port_id)) {

				if (taiko_p->intf_type ==
					WCD9XXX_INTERFACE_TYPE_SLIMBUS)
					vtable = vport_check_table[dai_id];
				if (taiko_p->intf_type ==
					WCD9XXX_INTERFACE_TYPE_I2C)
					vtable = vport_i2s_check_table[dai_id];

				if (wcd9xxx_tx_vport_validation(
						vtable,
						port_id,
						taiko_p->dai, NUM_CODEC_DAIS)) {
					dev_dbg(codec->dev, "%s: TX%u is used by other virtual port\n",
						__func__, port_id + 1);
					mutex_unlock(&codec->mutex);
					return 0;
				}
				widget->value |= 1 << port_id;
				list_add_tail(&core->tx_chs[port_id].list,
				      &taiko_p->dai[dai_id].wcd9xxx_ch_list
					      );
			} else if (!enable && (widget->value & 1 << port_id)) {
				widget->value &= ~(1 << port_id);
				list_del_init(&core->tx_chs[port_id].list);
			} else {
				if (enable)
					dev_dbg(codec->dev, "%s: TX%u port is used by\n"
						"this virtual port\n",
						__func__, port_id + 1);
				else
					dev_dbg(codec->dev, "%s: TX%u port is not used by\n"
						"this virtual port\n",
						__func__, port_id + 1);
				/* avoid update power function */
				mutex_unlock(&codec->mutex);
				return 0;
			}
			break;
		default:
			pr_debug("Unknown AIF %d\n", dai_id);
			mutex_unlock(&codec->mutex);
			return -EINVAL;
		}
	pr_debug("%s: name %s sname %s updated value %u shift %d\n", __func__,
		widget->name, widget->sname, widget->value, widget->shift);

	mutex_unlock(&codec->mutex);
	snd_soc_dapm_mixer_update_power(widget, kcontrol, enable);

	return 0;
}

static int slim_rx_mux_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget_list *wlist = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_widget *widget = wlist->widgets[0];

	ucontrol->value.enumerated.item[0] = widget->value;
	return 0;
}

static const char *slim_rx_mux_text[] = {
	"ZERO", "AIF1_PB", "AIF2_PB", "AIF3_PB"
};

static int slim_rx_mux_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget_list *wlist = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_widget *widget = wlist->widgets[0];
	struct snd_soc_codec *codec = widget->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);
	struct wcd9xxx *core = dev_get_drvdata(codec->dev->parent);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	u32 port_id = widget->shift;

	pr_debug("%s: wname %s cname %s value %u shift %d item %ld\n", __func__,
		widget->name, ucontrol->id.name, widget->value, widget->shift,
		ucontrol->value.integer.value[0]);

	widget->value = ucontrol->value.enumerated.item[0];

	mutex_lock(&codec->mutex);

	if (taiko_p->intf_type != WCD9XXX_INTERFACE_TYPE_SLIMBUS) {
		if (widget->value > 2) {
			dev_dbg(codec->dev, "%s: invalid AIF for I2C mode\n",
				__func__);
			goto err;
		}
	}
	/* value need to match the Virtual port and AIF number
	 */
	switch (widget->value) {
        case 0:
            list_del_init(&core->rx_chs[port_id].list);
            break;
    	case 1:
    		if (wcd9xxx_rx_vport_validation(port_id +
    			TAIKO_RX_PORT_START_NUMBER,
    			&taiko_p->dai[AIF1_PB].wcd9xxx_ch_list)) {
    			dev_dbg(codec->dev, "%s: RX%u is used by current requesting AIF_PB itself\n",
    				__func__, port_id + 1);
    			goto rtn;
    		}
    		list_add_tail(&core->rx_chs[port_id].list,
    			      &taiko_p->dai[AIF1_PB].wcd9xxx_ch_list);
            break;
    	case 2:
    		if (wcd9xxx_rx_vport_validation(port_id +
    			TAIKO_RX_PORT_START_NUMBER,
    			&taiko_p->dai[AIF2_PB].wcd9xxx_ch_list)) {
    			dev_dbg(codec->dev, "%s: RX%u is used by current requesting AIF_PB itself\n",
    				__func__, port_id + 1);
    			goto rtn;
    		}
    		list_add_tail(&core->rx_chs[port_id].list,
    			      &taiko_p->dai[AIF2_PB].wcd9xxx_ch_list);
            break;
    	case 3:
    		if (wcd9xxx_rx_vport_validation(port_id +
    			TAIKO_RX_PORT_START_NUMBER,
    			&taiko_p->dai[AIF3_PB].wcd9xxx_ch_list)) {
    			dev_dbg(codec->dev, "%s: RX%u is used by current requesting AIF_PB itself\n",
    				__func__, port_id + 1);
    			goto rtn;
		}
    		list_add_tail(&core->rx_chs[port_id].list,
    			      &taiko_p->dai[AIF3_PB].wcd9xxx_ch_list);
            break;
    	default:
    		pr_debug("Unknown AIF %d\n", widget->value);
    		goto err;
   	}
rtn:
	mutex_unlock(&codec->mutex);
	snd_soc_dapm_mux_update_power(widget, kcontrol, 1, widget->value, e);
	return 0;
err:
	mutex_unlock(&codec->mutex);
	return -EINVAL;
}

static const struct soc_enum slim_rx_mux_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(slim_rx_mux_text), slim_rx_mux_text);

static const struct snd_kcontrol_new slim_rx_mux[TAIKO_RX_MAX] = {
	SOC_DAPM_ENUM_EXT("SLIM RX1 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX2 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX3 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX4 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX5 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX6 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX7 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
};

static const struct snd_kcontrol_new aif_cap_mixer[] = {
	SOC_SINGLE_EXT("SLIM TX1", SND_SOC_NOPM, TAIKO_TX1, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
	SOC_SINGLE_EXT("SLIM TX2", SND_SOC_NOPM, TAIKO_TX2, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
	SOC_SINGLE_EXT("SLIM TX3", SND_SOC_NOPM, TAIKO_TX3, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
	SOC_SINGLE_EXT("SLIM TX4", SND_SOC_NOPM, TAIKO_TX4, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
	SOC_SINGLE_EXT("SLIM TX5", SND_SOC_NOPM, TAIKO_TX5, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
	SOC_SINGLE_EXT("SLIM TX6", SND_SOC_NOPM, TAIKO_TX6, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
	SOC_SINGLE_EXT("SLIM TX7", SND_SOC_NOPM, TAIKO_TX7, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
	SOC_SINGLE_EXT("SLIM TX8", SND_SOC_NOPM, TAIKO_TX8, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
	SOC_SINGLE_EXT("SLIM TX9", SND_SOC_NOPM, TAIKO_TX9, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
	SOC_SINGLE_EXT("SLIM TX10", SND_SOC_NOPM, TAIKO_TX10, 1, 0,
			slim_tx_mixer_get, slim_tx_mixer_put),
};

static void taiko_codec_enable_adc_block(struct snd_soc_codec *codec,
					 int enable)
{
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s %d\n", __func__, enable);

	if (enable) {
		taiko->adc_count++;
		snd_soc_update_bits(codec, WCD9XXX_A_CDC_CLK_OTHR_CTL,
						0x2, 0x2);
	} else {
		taiko->adc_count--;
		if (!taiko->adc_count)
			snd_soc_update_bits(codec, WCD9XXX_A_CDC_CLK_OTHR_CTL,
					    0x2, 0x0);
	}
}

static int taiko_codec_enable_adc(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	u16 adc_reg;
	u8 init_bit_shift;
	struct wcd9xxx *core = dev_get_drvdata(codec->dev->parent);

	pr_debug("%s %d\n", __func__, event);

	switch (w->reg) {
	case TAIKO_A_CDC_TX_1_GAIN:
		adc_reg = TAIKO_A_TX_1_2_TEST_CTL;
		init_bit_shift = 7;
		break;
	case TAIKO_A_CDC_TX_2_GAIN:
		adc_reg = TAIKO_A_TX_1_2_TEST_CTL;
		init_bit_shift = 6;
		break;
	case TAIKO_A_CDC_TX_3_GAIN:
		adc_reg = TAIKO_A_TX_3_4_TEST_CTL;
		init_bit_shift = 7;
		break;
	case TAIKO_A_CDC_TX_4_GAIN:
		adc_reg = TAIKO_A_TX_3_4_TEST_CTL;
		init_bit_shift = 6;
		break;
	case TAIKO_A_CDC_TX_5_GAIN:
		adc_reg = TAIKO_A_TX_5_6_TEST_CTL;
		init_bit_shift = 7;
		break;
	case TAIKO_A_CDC_TX_6_GAIN:
		adc_reg = TAIKO_A_TX_5_6_TEST_CTL;
		init_bit_shift = 6;
		break;
	default:
		pr_debug("%s: Error, invalid adc register\n", __func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		taiko_codec_enable_adc_block(codec, 1);
		snd_soc_update_bits(codec, adc_reg, 1 << init_bit_shift,
				1 << init_bit_shift);
		break;
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(codec, adc_reg, 1 << init_bit_shift, 0x00);
		break;
	case SND_SOC_DAPM_POST_PMD:
		taiko_codec_enable_adc_block(codec, 0);
		break;
	}
	return 0;
}

static int taiko_codec_enable_aux_pga(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s: %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		WCD9XXX_BG_CLK_LOCK(&taiko->resmgr);
		wcd9xxx_resmgr_get_bandgap(&taiko->resmgr,
					   WCD9XXX_BANDGAP_AUDIO_MODE);
		/* AUX PGA requires RCO or MCLK */
		wcd9xxx_resmgr_get_clk_block(&taiko->resmgr, WCD9XXX_CLK_RCO);
		wcd9xxx_resmgr_enable_rx_bias(&taiko->resmgr, 1);
		WCD9XXX_BG_CLK_UNLOCK(&taiko->resmgr);
		break;

	case SND_SOC_DAPM_POST_PMD:
		WCD9XXX_BG_CLK_LOCK(&taiko->resmgr);
		wcd9xxx_resmgr_enable_rx_bias(&taiko->resmgr, 0);
		wcd9xxx_resmgr_put_bandgap(&taiko->resmgr,
					   WCD9XXX_BANDGAP_AUDIO_MODE);
		wcd9xxx_resmgr_put_clk_block(&taiko->resmgr, WCD9XXX_CLK_RCO);
		WCD9XXX_BG_CLK_UNLOCK(&taiko->resmgr);
		break;
	}
	return 0;
}

static int taiko_codec_enable_lineout(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	u16 lineout_gain_reg;

	pr_debug("%s %d %s\n", __func__, event, w->name);

	switch (w->shift) {
	case 0:
		lineout_gain_reg = TAIKO_A_RX_LINE_1_GAIN;
		break;
	case 1:
		lineout_gain_reg = TAIKO_A_RX_LINE_2_GAIN;
		break;
	case 2:
		lineout_gain_reg = TAIKO_A_RX_LINE_3_GAIN;
		break;
	case 3:
		lineout_gain_reg = TAIKO_A_RX_LINE_4_GAIN;
		break;
	default:
		pr_debug("%s: Error, incorrect lineout register value\n",
			__func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, lineout_gain_reg, 0x40, 0x40);
		break;
	case SND_SOC_DAPM_POST_PMU:
		wcd9xxx_clsh_fsm(codec, &taiko->clsh_d,
						 WCD9XXX_CLSH_STATE_LO,
						 WCD9XXX_CLSH_REQ_ENABLE,
						 WCD9XXX_CLSH_EVENT_POST_PA);
		pr_debug("%s: sleeping 5 ms after %s PA turn on\n",
				__func__, w->name);
		/* Wait for CnP time after PA enable */
		usleep_range(5000, 5100);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, lineout_gain_reg, 0x40, 0x00);
		pr_debug("%s: sleeping 5 ms after %s PA turn off\n",
				__func__, w->name);
		/* Wait for CnP time after PA disable */
		usleep_range(5000, 5100);
		break;
	}
	return 0;
}

static int taiko_codec_enable_spk_pa(struct snd_soc_dapm_widget *w,
				     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s: %d %s\n", __func__, event, w->name);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TAIKO_A_SPKR_DRV_EN, 0x80, 0x80);
		taiko->spkr_pa_widget_on = true;
		spkwidget = true;
		write_speaker_hdc(1);
		update_speaker_gain();
		write_hpf_cutoff(TAIKO_A_CDC_RX7_B4_CTL);
		write_hpf_bypass(TAIKO_A_CDC_RX7_B5_CTL);
		break;
	case SND_SOC_DAPM_POST_PMD:
		taiko->spkr_pa_widget_on = false;
		spkwidget = false;
		update_speaker_gain();
		write_speaker_hdc(0);
		snd_soc_update_bits(codec, TAIKO_A_SPKR_DRV_EN, 0x80, 0x00);
		break;
	}
	return 0;
}

static int taiko_codec_enable_dmic(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	u8  dmic_clk_en;
	u16 dmic_clk_reg;
	s32 *dmic_clk_cnt;
	unsigned int dmic;
	int ret;

	ret = kstrtouint(strpbrk(w->name, "123456"), 10, &dmic);
	if (ret < 0) {
		pr_debug("%s: Invalid DMIC line on the codec\n", __func__);
		return -EINVAL;
	}

	switch (dmic) {
	case 1:
	case 2:
		dmic_clk_en = 0x01;
		dmic_clk_cnt = &(taiko->dmic_1_2_clk_cnt);
		dmic_clk_reg = TAIKO_A_CDC_CLK_DMIC_B1_CTL;
		pr_debug("%s() event %d DMIC%d dmic_1_2_clk_cnt %d\n",
			__func__, event,  dmic, *dmic_clk_cnt);

		break;

	case 3:
	case 4:
		dmic_clk_en = 0x10;
		dmic_clk_cnt = &(taiko->dmic_3_4_clk_cnt);
		dmic_clk_reg = TAIKO_A_CDC_CLK_DMIC_B1_CTL;

		pr_debug("%s() event %d DMIC%d dmic_3_4_clk_cnt %d\n",
			__func__, event,  dmic, *dmic_clk_cnt);
		break;

	case 5:
	case 6:
		dmic_clk_en = 0x01;
		dmic_clk_cnt = &(taiko->dmic_5_6_clk_cnt);
		dmic_clk_reg = TAIKO_A_CDC_CLK_DMIC_B2_CTL;

		pr_debug("%s() event %d DMIC%d dmic_5_6_clk_cnt %d\n",
			__func__, event,  dmic, *dmic_clk_cnt);

		break;

	default:
		pr_debug("%s: Invalid DMIC Selection\n", __func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:

		(*dmic_clk_cnt)++;
		if (*dmic_clk_cnt == 1)
			snd_soc_update_bits(codec, dmic_clk_reg,
					dmic_clk_en, dmic_clk_en);

		break;
	case SND_SOC_DAPM_POST_PMD:

		(*dmic_clk_cnt)--;
		if (*dmic_clk_cnt  == 0)
			snd_soc_update_bits(codec, dmic_clk_reg,
					dmic_clk_en, 0);
		break;
	}
	return 0;
}

static int taiko_codec_config_mad(struct snd_soc_codec *codec)
{
	int ret;
	const struct firmware *fw;
	struct mad_audio_cal *mad_cal;
	const char *filename = TAIKO_MAD_AUDIO_FIRMWARE_PATH;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s: enter\n", __func__);
	/* wakeup for codec calibration access */
	pm_qos_add_request(&taiko->pm_qos_req,
			   PM_QOS_CPU_DMA_LATENCY,
			   PM_QOS_DEFAULT_VALUE);
	pm_qos_update_request(&taiko->pm_qos_req,
			      msm_cpuidle_get_deep_idle_latency());
	ret = request_firmware(&fw, filename, codec->dev);
	if (ret != 0) {
		pr_debug("Failed to acquire MAD firwmare data %s: %d\n", filename,
		       ret);
		return -ENODEV;
	}

	if (fw->size < sizeof(struct mad_audio_cal)) {
		pr_debug("%s: incorrect firmware size %u\n", __func__, fw->size);
		release_firmware(fw);
		return -ENOMEM;
	}

	mad_cal = (struct mad_audio_cal *)(fw->data);
	if (!mad_cal) {
		pr_debug("%s: Invalid calibration data\n", __func__);
		release_firmware(fw);
		return -EINVAL;
	}

	snd_soc_write(codec, TAIKO_A_CDC_MAD_MAIN_CTL_2,
		      mad_cal->microphone_info.cycle_time);
	snd_soc_update_bits(codec, TAIKO_A_CDC_MAD_MAIN_CTL_1, 0xFF << 3,
			    ((uint16_t)mad_cal->microphone_info.settle_time)
			    << 3);

	/* Audio */
	snd_soc_write(codec, TAIKO_A_CDC_MAD_AUDIO_CTL_8,
		      mad_cal->audio_info.rms_omit_samples);
	snd_soc_update_bits(codec, TAIKO_A_CDC_MAD_AUDIO_CTL_1,
			    0x07 << 4, mad_cal->audio_info.rms_comp_time << 4);
	snd_soc_update_bits(codec, TAIKO_A_CDC_MAD_AUDIO_CTL_2, 0x03 << 2,
			    mad_cal->audio_info.detection_mechanism << 2);
	snd_soc_write(codec, TAIKO_A_CDC_MAD_AUDIO_CTL_7,
		      mad_cal->audio_info.rms_diff_threshold & 0x3F);
	snd_soc_write(codec, TAIKO_A_CDC_MAD_AUDIO_CTL_5,
		      mad_cal->audio_info.rms_threshold_lsb);
	snd_soc_write(codec, TAIKO_A_CDC_MAD_AUDIO_CTL_6,
		      mad_cal->audio_info.rms_threshold_msb);


	/* Beacon */
	snd_soc_write(codec, TAIKO_A_CDC_MAD_BEACON_CTL_8,
		      mad_cal->beacon_info.rms_omit_samples);
	snd_soc_update_bits(codec, TAIKO_A_CDC_MAD_BEACON_CTL_1,
			    0x07 << 4, mad_cal->beacon_info.rms_comp_time);
	snd_soc_update_bits(codec, TAIKO_A_CDC_MAD_BEACON_CTL_2, 0x03 << 2,
			    mad_cal->beacon_info.detection_mechanism << 2);
	snd_soc_write(codec, TAIKO_A_CDC_MAD_BEACON_CTL_7,
		      mad_cal->beacon_info.rms_diff_threshold & 0x1F);
	snd_soc_write(codec, TAIKO_A_CDC_MAD_BEACON_CTL_5,
		      mad_cal->beacon_info.rms_threshold_lsb);
	snd_soc_write(codec, TAIKO_A_CDC_MAD_BEACON_CTL_6,
		      mad_cal->beacon_info.rms_threshold_msb);

	/* Ultrasound */
	snd_soc_update_bits(codec, TAIKO_A_CDC_MAD_BEACON_CTL_1,
			    0x07 << 4, mad_cal->beacon_info.rms_comp_time);
	snd_soc_update_bits(codec, TAIKO_A_CDC_MAD_ULTR_CTL_2, 0x03 << 2,
			    mad_cal->ultrasound_info.detection_mechanism);
	snd_soc_write(codec, TAIKO_A_CDC_MAD_ULTR_CTL_7,
		      mad_cal->ultrasound_info.rms_diff_threshold & 0x1F);
	snd_soc_write(codec, TAIKO_A_CDC_MAD_ULTR_CTL_5,
		      mad_cal->ultrasound_info.rms_threshold_lsb);
	snd_soc_write(codec, TAIKO_A_CDC_MAD_ULTR_CTL_6,
		      mad_cal->ultrasound_info.rms_threshold_msb);

	release_firmware(fw);
	pr_debug("%s: leave ret %d\n", __func__, ret);
	pm_qos_update_request(&taiko->pm_qos_req,
			      PM_QOS_DEFAULT_VALUE);
	pm_qos_remove_request(&taiko->pm_qos_req);
	return ret;
}

static int taiko_codec_enable_mad(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	int ret = 0;

	pr_debug("%s %d\n", __func__, event);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = taiko_codec_config_mad(codec);
		if (ret) {
			pr_debug("%s: Failed to config MAD\n", __func__);
			break;
		}
		break;
	}
	return ret;
}

static int taiko_codec_enable_micbias(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	u16 micb_int_reg = 0, micb_ctl_reg = 0;
	u8 cfilt_sel_val = 0;
	char *internal1_text = "Internal1";
	char *internal2_text = "Internal2";
	char *internal3_text = "Internal3";
	enum wcd9xxx_notify_event e_post_off, e_pre_on, e_post_on;

	pr_debug("%s: w->name %s event %d\n", __func__, w->name, event);
	if (strnstr(w->name, "MIC BIAS1", sizeof("MIC BIAS1"))) {
		micb_ctl_reg = TAIKO_A_MICB_1_CTL;
		micb_int_reg = TAIKO_A_MICB_1_INT_RBIAS;
		cfilt_sel_val = taiko->resmgr.pdata->micbias.bias1_cfilt_sel;
		e_pre_on = WCD9XXX_EVENT_PRE_MICBIAS_1_ON;
		e_post_on = WCD9XXX_EVENT_POST_MICBIAS_1_ON;
		e_post_off = WCD9XXX_EVENT_POST_MICBIAS_1_OFF;
	} else if (strnstr(w->name, "MIC BIAS2", sizeof("MIC BIAS2"))) {
		micb_ctl_reg = TAIKO_A_MICB_2_CTL;
		micb_int_reg = TAIKO_A_MICB_2_INT_RBIAS;
		cfilt_sel_val = taiko->resmgr.pdata->micbias.bias2_cfilt_sel;
		e_pre_on = WCD9XXX_EVENT_PRE_MICBIAS_2_ON;
		e_post_on = WCD9XXX_EVENT_POST_MICBIAS_2_ON;
		e_post_off = WCD9XXX_EVENT_POST_MICBIAS_2_OFF;
	} else if (strnstr(w->name, "MIC BIAS3", sizeof("MIC BIAS3"))) {
		micb_ctl_reg = TAIKO_A_MICB_3_CTL;
		micb_int_reg = TAIKO_A_MICB_3_INT_RBIAS;
		cfilt_sel_val = taiko->resmgr.pdata->micbias.bias3_cfilt_sel;
		e_pre_on = WCD9XXX_EVENT_PRE_MICBIAS_3_ON;
		e_post_on = WCD9XXX_EVENT_POST_MICBIAS_3_ON;
		e_post_off = WCD9XXX_EVENT_POST_MICBIAS_3_OFF;
	} else if (strnstr(w->name, "MIC BIAS4", sizeof("MIC BIAS4"))) {
		micb_ctl_reg = TAIKO_A_MICB_4_CTL;
		micb_int_reg = taiko->resmgr.reg_addr->micb_4_int_rbias;
		cfilt_sel_val = taiko->resmgr.pdata->micbias.bias4_cfilt_sel;
		e_pre_on = WCD9XXX_EVENT_PRE_MICBIAS_4_ON;
		e_post_on = WCD9XXX_EVENT_POST_MICBIAS_4_ON;
		e_post_off = WCD9XXX_EVENT_POST_MICBIAS_4_OFF;
	} else {
		pr_debug("%s: Error, invalid micbias %s\n", __func__, w->name);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* Let MBHC module know so micbias switch to be off */
		wcd9xxx_resmgr_notifier_call(&taiko->resmgr, e_pre_on);

		/* Get cfilt */
		wcd9xxx_resmgr_cfilt_get(&taiko->resmgr, cfilt_sel_val);

		if (strnstr(w->name, internal1_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0xE0, 0xE0);
		else if (strnstr(w->name, internal2_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x1C, 0x1C);
		else if (strnstr(w->name, internal3_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x3, 0x3);

		if (taiko->mbhc_started && micb_ctl_reg == TAIKO_A_MICB_2_CTL) {
			if (++taiko->micb_2_users == 1) {
				if (taiko->resmgr.pdata->
				    micbias.bias2_is_headset_only)
					wcd9xxx_resmgr_add_cond_update_bits(
							 &taiko->resmgr,
							 WCD9XXX_COND_HPH_MIC,
							 micb_ctl_reg, w->shift,
							 false);
				else
					snd_soc_update_bits(codec, micb_ctl_reg,
							    1 << w->shift,
							    1 << w->shift);
			}
			pr_debug("%s: micb_2_users %d\n", __func__,
				 taiko->micb_2_users);
		} else {
			snd_soc_update_bits(codec, micb_ctl_reg, 1 << w->shift,
					    1 << w->shift);
		}
		break;
	case SND_SOC_DAPM_POST_PMU:
		usleep_range(20000, 20000);
		/* Let MBHC module know so micbias is on */
		wcd9xxx_resmgr_notifier_call(&taiko->resmgr, e_post_on);
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (taiko->mbhc_started && micb_ctl_reg == TAIKO_A_MICB_2_CTL) {
			if (--taiko->micb_2_users == 0) {
				if (taiko->resmgr.pdata->
				    micbias.bias2_is_headset_only)
					wcd9xxx_resmgr_rm_cond_update_bits(
							&taiko->resmgr,
							WCD9XXX_COND_HPH_MIC,
							micb_ctl_reg, 7, false);
				else
					snd_soc_update_bits(codec, micb_ctl_reg,
							    1 << w->shift, 0);
			}
			pr_debug("%s: micb_2_users %d\n", __func__,
				 taiko->micb_2_users);
			WARN(taiko->micb_2_users < 0,
			     "Unexpected micbias users %d\n",
			     taiko->micb_2_users);
		} else {
			snd_soc_update_bits(codec, micb_ctl_reg, 1 << w->shift,
					    0);
		}

		/* Let MBHC module know so micbias switch to be off */
		wcd9xxx_resmgr_notifier_call(&taiko->resmgr, e_post_off);

		if (strnstr(w->name, internal1_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x80, 0x00);
		else if (strnstr(w->name, internal2_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x10, 0x00);
		else if (strnstr(w->name, internal3_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x2, 0x0);

		/* Put cfilt */
		wcd9xxx_resmgr_cfilt_put(&taiko->resmgr, cfilt_sel_val);
		break;
	}

	return 0;
}

/* called under codec_resource_lock acquisition */
static int taiko_enable_mbhc_micbias(struct snd_soc_codec *codec, bool enable,
				     enum wcd9xxx_micbias_num micb_num)
{
	int rc;
	const char *micbias;

	if (micb_num != MBHC_MICBIAS3 &&
	    micb_num != MBHC_MICBIAS2)
		return -EINVAL;

	micbias = (micb_num == MBHC_MICBIAS3) ?
			DAPM_MICBIAS3_EXTERNAL_STANDALONE :
			DAPM_MICBIAS2_EXTERNAL_STANDALONE;

	if (enable)
		rc = snd_soc_dapm_force_enable_pin(&codec->dapm,
					     micbias);
	else
		rc = snd_soc_dapm_disable_pin(&codec->dapm,
					     micbias);
	if (!rc)
		snd_soc_dapm_sync(&codec->dapm);
	pr_debug("%s: leave ret %d\n", __func__, rc);
	return rc;
}

static void tx_hpf_corner_freq_callback(struct work_struct *work)
{
	struct delayed_work *hpf_delayed_work;
	struct hpf_work *hpf_work;
	struct taiko_priv *taiko;
	struct snd_soc_codec *codec;
	u16 tx_mux_ctl_reg;
	u8 hpf_cut_off_freq;

	hpf_delayed_work = to_delayed_work(work);
	hpf_work = container_of(hpf_delayed_work, struct hpf_work, dwork);
	taiko = hpf_work->taiko;
	codec = hpf_work->taiko->codec;
	hpf_cut_off_freq = hpf_work->tx_hpf_cut_off_freq;

	tx_mux_ctl_reg = TAIKO_A_CDC_TX1_MUX_CTL +
			(hpf_work->decimator - 1) * 8;

	pr_debug("%s(): decimator %u hpf_cut_off_freq 0x%x\n", __func__,
		hpf_work->decimator, (unsigned int)hpf_cut_off_freq);

	snd_soc_update_bits(codec, tx_mux_ctl_reg, 0x30, hpf_cut_off_freq << 4);
}

#define  TX_MUX_CTL_CUT_OFF_FREQ_MASK	0x30
#define  CF_MIN_3DB_4HZ			0x0
#define  CF_MIN_3DB_75HZ		0x1
#define  CF_MIN_3DB_150HZ		0x2

static int taiko_codec_enable_dec(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	unsigned int decimator;
	char *dec_name = NULL;
	char *widget_name = NULL;
	char *temp;
	int ret = 0;
	u16 dec_reset_reg, tx_vol_ctl_reg, tx_mux_ctl_reg;
	u8 dec_hpf_cut_off_freq;
	int offset;


	pr_debug("%s %d\n", __func__, event);

	widget_name = kstrndup(w->name, 15, GFP_KERNEL);
	if (!widget_name)
		return -ENOMEM;
	temp = widget_name;

	dec_name = strsep(&widget_name, " ");
	widget_name = temp;
	if (!dec_name) {
		pr_debug("%s: Invalid decimator = %s\n", __func__, w->name);
		ret =  -EINVAL;
		goto out;
	}

	ret = kstrtouint(strpbrk(dec_name, "123456789"), 10, &decimator);
	if (ret < 0) {
		pr_debug("%s: Invalid decimator = %s\n", __func__, dec_name);
		ret =  -EINVAL;
		goto out;
	}

	pr_debug("%s(): widget = %s dec_name = %s decimator = %u\n", __func__,
			w->name, dec_name, decimator);

	if (w->reg == TAIKO_A_CDC_CLK_TX_CLK_EN_B1_CTL) {
		dec_reset_reg = TAIKO_A_CDC_CLK_TX_RESET_B1_CTL;
		offset = 0;
	} else if (w->reg == TAIKO_A_CDC_CLK_TX_CLK_EN_B2_CTL) {
		dec_reset_reg = TAIKO_A_CDC_CLK_TX_RESET_B2_CTL;
		offset = 8;
	} else {
		pr_debug("%s: Error, incorrect dec\n", __func__);
		return -EINVAL;
	}

	tx_vol_ctl_reg = TAIKO_A_CDC_TX1_VOL_CTL_CFG + 8 * (decimator - 1);
	tx_mux_ctl_reg = TAIKO_A_CDC_TX1_MUX_CTL + 8 * (decimator - 1);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:

		/* Enableable TX digital mute */
		snd_soc_update_bits(codec, tx_vol_ctl_reg, 0x01, 0x01);

		snd_soc_update_bits(codec, dec_reset_reg, 1 << w->shift,
			1 << w->shift);
		snd_soc_update_bits(codec, dec_reset_reg, 1 << w->shift, 0x0);

		dec_hpf_cut_off_freq = snd_soc_read(codec, tx_mux_ctl_reg);

		dec_hpf_cut_off_freq = (dec_hpf_cut_off_freq & 0x30) >> 4;

		tx_hpf_work[decimator - 1].tx_hpf_cut_off_freq =
			dec_hpf_cut_off_freq;

		if ((dec_hpf_cut_off_freq != CF_MIN_3DB_150HZ)) {

			/* set cut of freq to CF_MIN_3DB_150HZ (0x2); */
			snd_soc_update_bits(codec, tx_mux_ctl_reg, 0x30,
					    CF_MIN_3DB_150HZ << 4);
		}

		/* enable HPF */
		snd_soc_update_bits(codec, tx_mux_ctl_reg , 0x08, 0x00);

		break;

	case SND_SOC_DAPM_POST_PMU:

		/* Disable TX digital mute */
		snd_soc_update_bits(codec, tx_vol_ctl_reg, 0x01, 0x00);

		if (tx_hpf_work[decimator - 1].tx_hpf_cut_off_freq !=
				CF_MIN_3DB_150HZ) {

			schedule_delayed_work(&tx_hpf_work[decimator - 1].dwork,
					msecs_to_jiffies(300));
		}
		/* apply the digital gain after the decimator is enabled*/
		if ((w->shift + offset) < ARRAY_SIZE(tx_digital_gain_reg))
			snd_soc_write(codec,
				  tx_digital_gain_reg[w->shift + offset],
				  snd_soc_read(codec,
				  tx_digital_gain_reg[w->shift + offset])
				  );

		break;

	case SND_SOC_DAPM_PRE_PMD:

		snd_soc_update_bits(codec, tx_vol_ctl_reg, 0x01, 0x01);
		cancel_delayed_work_sync(&tx_hpf_work[decimator - 1].dwork);
		break;

	case SND_SOC_DAPM_POST_PMD:

		snd_soc_update_bits(codec, tx_mux_ctl_reg, 0x08, 0x08);
		snd_soc_update_bits(codec, tx_mux_ctl_reg, 0x30,
			(tx_hpf_work[decimator - 1].tx_hpf_cut_off_freq) << 4);

		break;
	}
out:
	kfree(widget_name);
	return ret;
}

static int taiko_codec_enable_vdd_spkr(struct snd_soc_dapm_widget *w,
				       struct snd_kcontrol *kcontrol, int event)
{
	int ret = 0;
	struct snd_soc_codec *codec = w->codec;
	struct wcd9xxx *core = dev_get_drvdata(codec->dev->parent);
	struct taiko_priv *priv = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s: %d %s\n", __func__, event, w->name);

	WARN_ONCE(!priv->spkdrv_reg, "SPKDRV supply %s isn't defined\n",
		  WCD9XXX_VDD_SPKDRV_NAME);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (priv->spkdrv_reg) {
			ret = regulator_enable(priv->spkdrv_reg);
			if (ret)
				pr_debug("%s: Failed to enable spkdrv_reg %s\n",
				       __func__, WCD9XXX_VDD_SPKDRV_NAME);
		}
		if (spkr_drv_wrnd > 0) {
			WARN_ON(!(snd_soc_read(codec, TAIKO_A_SPKR_DRV_EN) &
				  0x80));
			snd_soc_update_bits(codec, TAIKO_A_SPKR_DRV_EN, 0x80,
					    0x00);
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (spkr_drv_wrnd > 0) {
			WARN_ON(!!(snd_soc_read(codec, TAIKO_A_SPKR_DRV_EN) &
				   0x80));
			snd_soc_update_bits(codec, TAIKO_A_SPKR_DRV_EN, 0x80,
					    0x80);
		}
		if (priv->spkdrv_reg) {
			ret = regulator_disable(priv->spkdrv_reg);
			if (ret)
				pr_debug("%s: Failed to disable spkdrv_reg %s\n",
				       __func__, WCD9XXX_VDD_SPKDRV_NAME);
		}
		break;
	}

	return ret;
}

static int taiko_codec_enable_interpolator(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	pr_debug("%s %d %s\n", __func__, event, w->name);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RX_RESET_CTL,
			1 << w->shift, 1 << w->shift);
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RX_RESET_CTL,
			1 << w->shift, 0x0);
		break;
	case SND_SOC_DAPM_POST_PMU:
		/* apply the digital gain after the interpolator is enabled*/
		if ((w->shift) < ARRAY_SIZE(rx_digital_gain_reg))
			snd_soc_write(codec,
				  rx_digital_gain_reg[w->shift],
				  snd_soc_read(codec,
				  rx_digital_gain_reg[w->shift])
				  );
		break;
	}
    update_control_regs();
	return 0;
}

/* called under codec_resource_lock acquisition */
static int __taiko_codec_enable_ldo_h(struct snd_soc_dapm_widget *w,
				      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *priv = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s: enter\n", __func__);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/*
		 * ldo_h_users is protected by codec->mutex, don't need
		 * additional mutex
		 */
		if (++priv->ldo_h_users == 1) {
			WCD9XXX_BG_CLK_LOCK(&priv->resmgr);
			wcd9xxx_resmgr_get_bandgap(&priv->resmgr,
						   WCD9XXX_BANDGAP_AUDIO_MODE);
			wcd9xxx_resmgr_get_clk_block(&priv->resmgr,
						     WCD9XXX_CLK_RCO);
			snd_soc_update_bits(codec, TAIKO_A_LDO_H_MODE_1, 1 << 7,
					    1 << 7);
			wcd9xxx_resmgr_put_clk_block(&priv->resmgr,
						     WCD9XXX_CLK_RCO);
			WCD9XXX_BG_CLK_UNLOCK(&priv->resmgr);
			pr_debug("%s: ldo_h_users %d\n", __func__,
				 priv->ldo_h_users);
			/* LDO enable requires 1ms to settle down */
			usleep_range(1000, 1000);
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (--priv->ldo_h_users == 0) {
			WCD9XXX_BG_CLK_LOCK(&priv->resmgr);
			wcd9xxx_resmgr_get_clk_block(&priv->resmgr,
						     WCD9XXX_CLK_RCO);
			snd_soc_update_bits(codec, TAIKO_A_LDO_H_MODE_1, 1 << 7,
					    0);
			wcd9xxx_resmgr_put_clk_block(&priv->resmgr,
						     WCD9XXX_CLK_RCO);
			wcd9xxx_resmgr_put_bandgap(&priv->resmgr,
						   WCD9XXX_BANDGAP_AUDIO_MODE);
			WCD9XXX_BG_CLK_UNLOCK(&priv->resmgr);
			pr_debug("%s: ldo_h_users %d\n", __func__,
				 priv->ldo_h_users);
		}
		WARN(priv->ldo_h_users < 0, "Unexpected ldo_h users %d\n",
		     priv->ldo_h_users);
		break;
	}
	pr_debug("%s: leave\n", __func__);
	return 0;
}

static int taiko_codec_enable_ldo_h(struct snd_soc_dapm_widget *w,
				    struct snd_kcontrol *kcontrol, int event)
{
	int rc;
	rc = __taiko_codec_enable_ldo_h(w, kcontrol, event);
	return rc;
}

static int taiko_codec_enable_rx_bias(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		wcd9xxx_resmgr_enable_rx_bias(&taiko->resmgr, 1);
		break;
	case SND_SOC_DAPM_POST_PMD:
		wcd9xxx_resmgr_enable_rx_bias(&taiko->resmgr, 0);
		break;
	}
	return 0;
}

static int taiko_hphl_dac_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);
	/* uint32_t impedl, impedr; */
	/* int ret = 0; */

	pr_debug("%s %s %d\n", __func__, w->name, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RDAC_CLK_EN_CTL,
							0x02, 0x02);

		if (high_perf_mode) {
			wcd9xxx_enable_high_perf_mode(codec, &taiko_p->clsh_d,
						WCD9XXX_CLSAB_STATE_HPHL,
						WCD9XXX_CLSAB_REQ_ENABLE);
			class_ab_left_active = true;
		} else {
			wcd9xxx_clsh_fsm(codec, &taiko_p->clsh_d,
						 WCD9XXX_CLSH_STATE_HPHL,
						 WCD9XXX_CLSH_REQ_ENABLE,
						 WCD9XXX_CLSH_EVENT_PRE_DAC);
			class_ab_left_active = false;
		}
		/*ret = wcd9xxx_mbhc_get_impedance(&taiko_p->mbhc,
					&impedl, &impedr);
		if (!ret) */
		wcd9xxx_clsh_imped_config(codec, 0);
		/* else
			dev_dbg(codec->dev, "Failed to get mbhc impedance %d\n",
						ret); */
		break;
	case SND_SOC_DAPM_POST_PMU:
		write_hpf_cutoff(TAIKO_A_CDC_RX1_B4_CTL);
		write_hpf_bypass(TAIKO_A_CDC_RX1_B5_CTL);
        write_hph_poweramp_regs();
        write_hdc_left(1);
		break;
	case SND_SOC_DAPM_PRE_PMD:
        write_hdc_left(0);
		write_hpf_cutoff(TAIKO_A_CDC_RX1_B4_CTL);
		write_hpf_bypass(TAIKO_A_CDC_RX1_B5_CTL);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RDAC_CLK_EN_CTL,
							0x02, 0x00);
		write_hph_poweramp_regs();

		if (high_perf_mode || class_ab_left_active) {
			wcd9xxx_enable_high_perf_mode(codec, &taiko_p->clsh_d,
						WCD9XXX_CLSAB_STATE_HPHL,
						WCD9XXX_CLSAB_REQ_DISABLE);
			class_ab_left_active = false;
		} else {
			wcd9xxx_clsh_fsm(codec, &taiko_p->clsh_d,
						 WCD9XXX_CLSH_STATE_HPHL,
						 WCD9XXX_CLSH_REQ_DISABLE,
						 WCD9XXX_CLSH_EVENT_POST_PA);
		}
	}
	return 0;
}

static int taiko_hphr_dac_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s %s %d\n", __func__, w->name, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RDAC_CLK_EN_CTL,
							0x04, 0x04);
		snd_soc_update_bits(codec, w->reg, 0x40, 0x40);

		if (high_perf_mode) {
			wcd9xxx_enable_high_perf_mode(codec, &taiko_p->clsh_d,
						WCD9XXX_CLSAB_STATE_HPHR,
						WCD9XXX_CLSAB_REQ_ENABLE);
			class_ab_right_active = true;
		} else {
			wcd9xxx_clsh_fsm(codec, &taiko_p->clsh_d,
						 WCD9XXX_CLSH_STATE_HPHR,
						 WCD9XXX_CLSH_REQ_ENABLE,
						 WCD9XXX_CLSH_EVENT_PRE_DAC);
			class_ab_right_active = false;
		}
		break;
	case SND_SOC_DAPM_POST_PMU:
		write_hpf_cutoff(TAIKO_A_CDC_RX2_B4_CTL);
		write_hpf_bypass(TAIKO_A_CDC_RX2_B5_CTL);
		write_hph_poweramp_regs();
        if (headphone_hdc) {
            write_hdc_right(1);
        }
		break;
	case SND_SOC_DAPM_PRE_PMD:
		write_hpf_cutoff(TAIKO_A_CDC_RX2_B4_CTL);
		write_hpf_bypass(TAIKO_A_CDC_RX2_B5_CTL);
        write_hdc_right(0);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RDAC_CLK_EN_CTL,
							0x04, 0x00);
		snd_soc_update_bits(codec, w->reg, 0x40, 0x00);
		write_hph_poweramp_regs();

		if (high_perf_mode || class_ab_right_active) {
			wcd9xxx_enable_high_perf_mode(codec, &taiko_p->clsh_d,
						WCD9XXX_CLSAB_STATE_HPHR,
						WCD9XXX_CLSAB_REQ_DISABLE);
			class_ab_right_active = false;
		} else {
			wcd9xxx_clsh_fsm(codec, &taiko_p->clsh_d,
						 WCD9XXX_CLSH_STATE_HPHR,
						 WCD9XXX_CLSH_REQ_DISABLE,
						 WCD9XXX_CLSH_EVENT_POST_PA);
		}
		break;
	}
	return 0;
}

static int taiko_codec_enable_anc(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	const char *filename;
	const struct firmware *fw;
	int i;
	int ret;
	int num_anc_slots;
	struct wcd9xxx_anc_header *anc_head;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	u32 anc_writes_size = 0;
	int anc_size_remaining;
	u32 *anc_ptr;
	u16 reg;
	u8 mask, val, old_val;


	if (!taiko->anc_func)
		return 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		filename = "wcd9320/wcd9320_anc.bin";

		ret = request_firmware(&fw, filename, codec->dev);
		if (ret != 0) {
			dev_dbg(codec->dev, "Failed to acquire ANC data: %d\n",
				ret);
			return -ENODEV;
		}

		if (fw->size < sizeof(struct wcd9xxx_anc_header)) {
			dev_dbg(codec->dev, "Not enough data\n");
			release_firmware(fw);
			return -ENOMEM;
		}

		/* First number is the number of register writes */
		anc_head = (struct wcd9xxx_anc_header *)(fw->data);
		anc_ptr = (u32 *)((u32)fw->data +
				  sizeof(struct wcd9xxx_anc_header));
		anc_size_remaining = fw->size -
				     sizeof(struct wcd9xxx_anc_header);
		num_anc_slots = anc_head->num_anc_slots;

		if (taiko->anc_slot >= num_anc_slots) {
			dev_dbg(codec->dev, "Invalid ANC slot selected\n");
			release_firmware(fw);
			return -EINVAL;
		}
		for (i = 0; i < num_anc_slots; i++) {
			if (anc_size_remaining < TAIKO_PACKED_REG_SIZE) {
				dev_dbg(codec->dev, "Invalid register format\n");
				release_firmware(fw);
				return -EINVAL;
			}
			anc_writes_size = (u32)(*anc_ptr);
			anc_size_remaining -= sizeof(u32);
			anc_ptr += 1;

			if (anc_writes_size * TAIKO_PACKED_REG_SIZE
				> anc_size_remaining) {
				dev_dbg(codec->dev, "Invalid register format\n");
				release_firmware(fw);
				return -ENOMEM;
			}

			if (taiko->anc_slot == i)
				break;

			anc_size_remaining -= (anc_writes_size *
				TAIKO_PACKED_REG_SIZE);
			anc_ptr += anc_writes_size;
		}
		if (i == num_anc_slots) {
			dev_dbg(codec->dev, "Selected ANC slot not present\n");
			release_firmware(fw);
			return -ENOMEM;
		}
		for (i = 0; i < anc_writes_size; i++) {
			TAIKO_CODEC_UNPACK_ENTRY(anc_ptr[i], reg,
				mask, val);
			old_val = snd_soc_read(codec, reg);
			snd_soc_write(codec, reg, (old_val & ~mask) |
				(val & mask));
		}
		release_firmware(fw);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		msleep(40);
		snd_soc_update_bits(codec, TAIKO_A_CDC_ANC1_B1_CTL, 0x01, 0x00);
		snd_soc_update_bits(codec, TAIKO_A_CDC_ANC2_B1_CTL, 0x02, 0x00);
		msleep(20);
		snd_soc_write(codec, TAIKO_A_CDC_CLK_ANC_RESET_CTL, 0x0F);
		snd_soc_write(codec, TAIKO_A_CDC_CLK_ANC_CLK_EN_CTL, 0);
		snd_soc_write(codec, TAIKO_A_CDC_CLK_ANC_RESET_CTL, 0xFF);
		break;
	}
	return 0;
}

static int taiko_hph_pa_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	enum wcd9xxx_notify_event e_pre_on, e_post_off;
	u8 req_clsh_state;
	u8 req_clsab_state;

	pr_debug("%s: %s event = %d\n", __func__, w->name, event);
	if (w->shift == 5) {
		e_pre_on = WCD9XXX_EVENT_PRE_HPHL_PA_ON;
		e_post_off = WCD9XXX_EVENT_POST_HPHL_PA_OFF;
		req_clsh_state = WCD9XXX_CLSH_STATE_HPHL;
		req_clsab_state = WCD9XXX_CLSAB_STATE_HPHL;
	} else if (w->shift == 4) {
		e_pre_on = WCD9XXX_EVENT_PRE_HPHR_PA_ON;
		e_post_off = WCD9XXX_EVENT_POST_HPHR_PA_OFF;
		req_clsh_state = WCD9XXX_CLSH_STATE_HPHR;
		req_clsab_state = WCD9XXX_CLSAB_STATE_HPHR;
	} else {
		 pr_err("%s: Invalid w->shift %d\n", __func__, w->shift);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* Let MBHC module know PA is turning on */
		wcd9xxx_resmgr_notifier_call(&taiko->resmgr, e_pre_on);
		break;

	case SND_SOC_DAPM_POST_PMU:
		usleep_range(5000, 5000);
//		pr_debug("%s: sleep 5000us after %s PA enable\n", __func__, w->name);
		if (!high_perf_mode) {
			wcd9xxx_clsh_fsm(codec, &taiko->clsh_d,
						 req_clsh_state,
						 WCD9XXX_CLSH_REQ_ENABLE,
						 WCD9XXX_CLSH_EVENT_POST_PA);
		}

		if (w->shift == 5)
			hpwidget_left = true;
		else if (w->shift == 4)
			hpwidget_right = true;

		update_headphone_gain();
#if 0
        update_iir_gain();
        update_crossfeed_gain();
#endif
		if (hph_pa_enabled) {
			if (w->shift == 4 || w->shift == 5)
				write_hph_poweramp_regs();
		}

		if (w->shift == 5) {
			pr_debug("%s: hpwidget_left enabled\n", __func__);
            write_hdc_left(1);
		} else if (w->shift == 4) {
			pr_debug("%s: hpwidget_right enabled\n", __func__);
            write_hdc_right(1);
        }
        update_control_regs();
		break;
	case SND_SOC_DAPM_PRE_PMD:
		if (hph_pa_enabled) {
			if ( w->shift == 4 || w->shift == 5)
				write_hph_poweramp_regs();
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (w->shift == 5)
			hpwidget_left = false;
		else if (w->shift == 4)
			hpwidget_right = false;

		usleep_range(13000, 13000);
//		pr_debug("%s: sleep 13000us after %s PA disable\n", __func__, w->name);
		if (w->shift == 5) {
			pr_debug("%s: hpwidget_left disabled\n", __func__);
            write_hdc_left(0);
		} else if (w->shift == 4) {
			pr_debug("%s: hpwidget_right disabled\n", __func__);
            write_hdc_right(0);
        }
		/* Let MBHC module know PA turned off */
		wcd9xxx_resmgr_notifier_call(&taiko->resmgr, e_post_off);

		break;
	}
	return 0;
}

static int taiko_codec_enable_anc_hph(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = taiko_hph_pa_event(w, kcontrol, event);
		if (w->shift == 4) {
			ret |= taiko_codec_enable_anc(w, kcontrol, event);
			msleep(50);
		}
		break;
	case SND_SOC_DAPM_POST_PMU:
		if (w->shift == 4) {
			snd_soc_update_bits(codec,
					TAIKO_A_RX_HPH_CNP_EN, 0x30, 0x30);
			msleep(30);
		}
		ret = taiko_hph_pa_event(w, kcontrol, event);
        update_control_regs();
		break;
	case SND_SOC_DAPM_PRE_PMD:
		if (w->shift == 5) {
			snd_soc_update_bits(codec,
					TAIKO_A_RX_HPH_CNP_EN, 0x30, 0x00);
			msleep(40);
			snd_soc_update_bits(codec,
					TAIKO_A_TX_7_MBHC_EN, 0x80, 00);
			ret |= taiko_codec_enable_anc(w, kcontrol, event);
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		ret = taiko_hph_pa_event(w, kcontrol, event);
		break;
	}
	return ret;
}

static const struct snd_soc_dapm_widget taiko_dapm_i2s_widgets[] = {
	SND_SOC_DAPM_SUPPLY("RX_I2S_CLK", TAIKO_A_CDC_CLK_RX_I2S_CTL,
	4, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("TX_I2S_CLK", TAIKO_A_CDC_CLK_TX_I2S_CTL, 4,
	0, NULL, 0),
};

static int taiko_lineout_dac_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s %s %d\n", __func__, w->name, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		wcd9xxx_clsh_fsm(codec, &taiko->clsh_d,
						 WCD9XXX_CLSH_STATE_LO,
						 WCD9XXX_CLSH_REQ_ENABLE,
						 WCD9XXX_CLSH_EVENT_PRE_DAC);
		snd_soc_update_bits(codec, w->reg, 0x40, 0x40);
		break;

	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, w->reg, 0x40, 0x00);
		wcd9xxx_clsh_fsm(codec, &taiko->clsh_d,
						 WCD9XXX_CLSH_STATE_LO,
						 WCD9XXX_CLSH_REQ_DISABLE,
						 WCD9XXX_CLSH_EVENT_POST_PA);
		break;
	}
	return 0;
}

static int taiko_spk_dac_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	pr_debug("%s %s %d\n", __func__, w->name, event);
    if (event == SND_SOC_DAPM_PRE_PMU || event == SND_SOC_DAPM_POST_PMD) {
    	write_hpf_cutoff(TAIKO_A_CDC_RX7_B4_CTL);
    	write_hpf_bypass(TAIKO_A_CDC_RX7_B5_CTL);
    }
	return 0;
}

static const struct snd_soc_dapm_route audio_i2s_map[] = {
	{"SLIM RX1", NULL, "RX_I2S_CLK"},
	{"SLIM RX2", NULL, "RX_I2S_CLK"},
	{"SLIM RX3", NULL, "RX_I2S_CLK"},
	{"SLIM RX4", NULL, "RX_I2S_CLK"},

	{"SLIM TX7 MUX", NULL, "TX_I2S_CLK"},
	{"SLIM TX8 MUX", NULL, "TX_I2S_CLK"},
	{"SLIM TX9 MUX", NULL, "TX_I2S_CLK"},
	{"SLIM TX10 MUX", NULL, "TX_I2S_CLK"},
};

static const struct snd_soc_dapm_route audio_i2s_map_1_0[] = {
	{"RX_I2S_CLK", NULL, "CDC_CONN"},
};

static const struct snd_soc_dapm_route audio_i2s_map_2_0[] = {
	{"RX_I2S_CLK", NULL, "CDC_I2S_RX_CONN"},
};

static const struct snd_soc_dapm_route audio_map[] = {
	/* SLIMBUS Connections */
	{"AIF1 CAP", NULL, "AIF1_CAP Mixer"},
	{"AIF2 CAP", NULL, "AIF2_CAP Mixer"},
	{"AIF3 CAP", NULL, "AIF3_CAP Mixer"},
	/* VI Feedback */
	{"AIF4 VI", NULL, "VIONOFF"},
	{"VIONOFF", "Switch", "VIINPUT"},

	/* MAD */
	{"AIF4 MAD", NULL, "CDC_CONN"},
	{"MADONOFF", "Switch", "MADINPUT"},
	{"AIF4 MAD", NULL, "MADONOFF"},

	/* SLIM_MIXER("AIF1_CAP Mixer"),*/
	{"AIF1_CAP Mixer", "SLIM TX1", "SLIM TX1 MUX"},
	{"AIF1_CAP Mixer", "SLIM TX2", "SLIM TX2 MUX"},
	{"AIF1_CAP Mixer", "SLIM TX3", "SLIM TX3 MUX"},
	{"AIF1_CAP Mixer", "SLIM TX4", "SLIM TX4 MUX"},
	{"AIF1_CAP Mixer", "SLIM TX5", "SLIM TX5 MUX"},
	{"AIF1_CAP Mixer", "SLIM TX6", "SLIM TX6 MUX"},
	{"AIF1_CAP Mixer", "SLIM TX7", "SLIM TX7 MUX"},
	{"AIF1_CAP Mixer", "SLIM TX8", "SLIM TX8 MUX"},
	{"AIF1_CAP Mixer", "SLIM TX9", "SLIM TX9 MUX"},
	{"AIF1_CAP Mixer", "SLIM TX10", "SLIM TX10 MUX"},
	/* SLIM_MIXER("AIF2_CAP Mixer"),*/
	{"AIF2_CAP Mixer", "SLIM TX1", "SLIM TX1 MUX"},
	{"AIF2_CAP Mixer", "SLIM TX2", "SLIM TX2 MUX"},
	{"AIF2_CAP Mixer", "SLIM TX3", "SLIM TX3 MUX"},
	{"AIF2_CAP Mixer", "SLIM TX4", "SLIM TX4 MUX"},
	{"AIF2_CAP Mixer", "SLIM TX5", "SLIM TX5 MUX"},
	{"AIF2_CAP Mixer", "SLIM TX6", "SLIM TX6 MUX"},
	{"AIF2_CAP Mixer", "SLIM TX7", "SLIM TX7 MUX"},
	{"AIF2_CAP Mixer", "SLIM TX8", "SLIM TX8 MUX"},
	{"AIF2_CAP Mixer", "SLIM TX9", "SLIM TX9 MUX"},
	{"AIF2_CAP Mixer", "SLIM TX10", "SLIM TX10 MUX"},
	/* SLIM_MIXER("AIF3_CAP Mixer"),*/
	{"AIF3_CAP Mixer", "SLIM TX1", "SLIM TX1 MUX"},
	{"AIF3_CAP Mixer", "SLIM TX2", "SLIM TX2 MUX"},
	{"AIF3_CAP Mixer", "SLIM TX3", "SLIM TX3 MUX"},
	{"AIF3_CAP Mixer", "SLIM TX4", "SLIM TX4 MUX"},
	{"AIF3_CAP Mixer", "SLIM TX5", "SLIM TX5 MUX"},
	{"AIF3_CAP Mixer", "SLIM TX6", "SLIM TX6 MUX"},
	{"AIF3_CAP Mixer", "SLIM TX7", "SLIM TX7 MUX"},
	{"AIF3_CAP Mixer", "SLIM TX8", "SLIM TX8 MUX"},
	{"AIF3_CAP Mixer", "SLIM TX9", "SLIM TX9 MUX"},
	{"AIF3_CAP Mixer", "SLIM TX10", "SLIM TX10 MUX"},

	{"SLIM TX1 MUX", "DEC1", "DEC1 MUX"},

	{"SLIM TX2 MUX", "DEC2", "DEC2 MUX"},

	{"SLIM TX3 MUX", "DEC3", "DEC3 MUX"},
	{"SLIM TX3 MUX", "RMIX1", "RX1 MIX1"},
	{"SLIM TX3 MUX", "RMIX2", "RX2 MIX1"},
	{"SLIM TX3 MUX", "RMIX3", "RX3 MIX1"},
	{"SLIM TX3 MUX", "RMIX4", "RX4 MIX1"},
	{"SLIM TX3 MUX", "RMIX5", "RX5 MIX1"},
	{"SLIM TX3 MUX", "RMIX6", "RX6 MIX1"},
	{"SLIM TX3 MUX", "RMIX7", "RX7 MIX1"},

	{"SLIM TX4 MUX", "DEC4", "DEC4 MUX"},

	{"SLIM TX5 MUX", "DEC5", "DEC5 MUX"},
	{"SLIM TX5 MUX", "RMIX1", "RX1 MIX1"},
	{"SLIM TX5 MUX", "RMIX2", "RX2 MIX1"},
	{"SLIM TX5 MUX", "RMIX3", "RX3 MIX1"},
	{"SLIM TX5 MUX", "RMIX4", "RX4 MIX1"},
	{"SLIM TX5 MUX", "RMIX5", "RX5 MIX1"},
	{"SLIM TX5 MUX", "RMIX6", "RX6 MIX1"},
	{"SLIM TX5 MUX", "RMIX7", "RX7 MIX1"},

	{"SLIM TX6 MUX", "DEC6", "DEC6 MUX"},

	{"SLIM TX7 MUX", "DEC1", "DEC1 MUX"},
	{"SLIM TX7 MUX", "DEC2", "DEC2 MUX"},
	{"SLIM TX7 MUX", "DEC3", "DEC3 MUX"},
	{"SLIM TX7 MUX", "DEC4", "DEC4 MUX"},
	{"SLIM TX7 MUX", "DEC5", "DEC5 MUX"},
	{"SLIM TX7 MUX", "DEC6", "DEC6 MUX"},
	{"SLIM TX7 MUX", "DEC7", "DEC7 MUX"},
	{"SLIM TX7 MUX", "DEC8", "DEC8 MUX"},
	{"SLIM TX7 MUX", "DEC9", "DEC9 MUX"},
	{"SLIM TX7 MUX", "DEC10", "DEC10 MUX"},
	{"SLIM TX7 MUX", "RMIX1", "RX1 MIX1"},
	{"SLIM TX7 MUX", "RMIX2", "RX2 MIX1"},
	{"SLIM TX7 MUX", "RMIX3", "RX3 MIX1"},
	{"SLIM TX7 MUX", "RMIX4", "RX4 MIX1"},
	{"SLIM TX7 MUX", "RMIX5", "RX5 MIX1"},
	{"SLIM TX7 MUX", "RMIX6", "RX6 MIX1"},
	{"SLIM TX7 MUX", "RMIX7", "RX7 MIX1"},

	{"SLIM TX8 MUX", "DEC1", "DEC1 MUX"},
	{"SLIM TX8 MUX", "DEC2", "DEC2 MUX"},
	{"SLIM TX8 MUX", "DEC3", "DEC3 MUX"},
	{"SLIM TX8 MUX", "DEC4", "DEC4 MUX"},
	{"SLIM TX8 MUX", "DEC5", "DEC5 MUX"},
	{"SLIM TX8 MUX", "DEC6", "DEC6 MUX"},
	{"SLIM TX8 MUX", "DEC7", "DEC7 MUX"},
	{"SLIM TX8 MUX", "DEC8", "DEC8 MUX"},
	{"SLIM TX8 MUX", "DEC9", "DEC9 MUX"},
	{"SLIM TX8 MUX", "DEC10", "DEC10 MUX"},

	{"SLIM TX9 MUX", "DEC1", "DEC1 MUX"},
	{"SLIM TX9 MUX", "DEC2", "DEC2 MUX"},
	{"SLIM TX9 MUX", "DEC3", "DEC3 MUX"},
	{"SLIM TX9 MUX", "DEC4", "DEC4 MUX"},
	{"SLIM TX9 MUX", "DEC5", "DEC5 MUX"},
	{"SLIM TX9 MUX", "DEC6", "DEC6 MUX"},
	{"SLIM TX9 MUX", "DEC7", "DEC7 MUX"},
	{"SLIM TX9 MUX", "DEC8", "DEC8 MUX"},
	{"SLIM TX9 MUX", "DEC9", "DEC9 MUX"},
	{"SLIM TX9 MUX", "DEC10", "DEC10 MUX"},

	{"SLIM TX10 MUX", "DEC1", "DEC1 MUX"},
	{"SLIM TX10 MUX", "DEC2", "DEC2 MUX"},
	{"SLIM TX10 MUX", "DEC3", "DEC3 MUX"},
	{"SLIM TX10 MUX", "DEC4", "DEC4 MUX"},
	{"SLIM TX10 MUX", "DEC5", "DEC5 MUX"},
	{"SLIM TX10 MUX", "DEC6", "DEC6 MUX"},
	{"SLIM TX10 MUX", "DEC7", "DEC7 MUX"},
	{"SLIM TX10 MUX", "DEC8", "DEC8 MUX"},
	{"SLIM TX10 MUX", "DEC9", "DEC9 MUX"},
	{"SLIM TX10 MUX", "DEC10", "DEC10 MUX"},

	/* Earpiece (RX MIX1) */
	{"EAR", NULL, "EAR PA"},
	{"EAR PA", NULL, "EAR_PA_MIXER"},
	{"EAR_PA_MIXER", NULL, "DAC1"},
	{"DAC1", NULL, "RX_BIAS"},

	{"ANC EAR", NULL, "ANC EAR PA"},
	{"ANC EAR PA", NULL, "EAR_PA_MIXER"},
	{"ANC1 FB MUX", "EAR_HPH_L", "RX1 MIX2"},
	{"ANC1 FB MUX", "EAR_LINE_1", "RX2 MIX2"},

	/* Headset (RX MIX1 and RX MIX2) */
	{"HEADPHONE", NULL, "HPHL"},
	{"HEADPHONE", NULL, "HPHR"},

	{"HPHL", NULL, "HPHL_PA_MIXER"},
	{"HPHL_PA_MIXER", NULL, "HPHL DAC"},
	{"HPHL DAC", NULL, "RX_BIAS"},

	{"HPHR", NULL, "HPHR_PA_MIXER"},
	{"HPHR_PA_MIXER", NULL, "HPHR DAC"},
	{"HPHR DAC", NULL, "RX_BIAS"},

	{"ANC HEADPHONE", NULL, "ANC HPHL"},
	{"ANC HEADPHONE", NULL, "ANC HPHR"},

	{"ANC HPHL", NULL, "HPHL_PA_MIXER"},
	{"ANC HPHR", NULL, "HPHR_PA_MIXER"},

	{"ANC1 MUX", "ADC1", "ADC1"},
	{"ANC1 MUX", "ADC2", "ADC2"},
	{"ANC1 MUX", "ADC3", "ADC3"},
	{"ANC1 MUX", "ADC4", "ADC4"},
	{"ANC1 MUX", "DMIC1", "DMIC1"},
	{"ANC1 MUX", "DMIC2", "DMIC2"},
	{"ANC1 MUX", "DMIC3", "DMIC3"},
	{"ANC1 MUX", "DMIC4", "DMIC4"},
	{"ANC1 MUX", "DMIC5", "DMIC5"},
	{"ANC1 MUX", "DMIC6", "DMIC6"},
	{"ANC2 MUX", "ADC1", "ADC1"},
	{"ANC2 MUX", "ADC2", "ADC2"},
	{"ANC2 MUX", "ADC3", "ADC3"},
	{"ANC2 MUX", "ADC4", "ADC4"},

	{"ANC HPHR", NULL, "CDC_CONN"},

	{"DAC1", "Switch", "CLASS_H_DSM MUX"},
	{"HPHL DAC", "Switch", "CLASS_H_DSM MUX"},
	{"HPHR DAC", NULL, "RX2 CHAIN"},

	{"LINEOUT1", NULL, "LINEOUT1 PA"},
	{"LINEOUT2", NULL, "LINEOUT2 PA"},
	{"LINEOUT3", NULL, "LINEOUT3 PA"},
	{"LINEOUT4", NULL, "LINEOUT4 PA"},
	{"SPK_OUT", NULL, "SPK PA"},

	{"LINEOUT1 PA", NULL, "LINEOUT1_PA_MIXER"},
	{"LINEOUT1_PA_MIXER", NULL, "LINEOUT1 DAC"},

	{"LINEOUT2 PA", NULL, "LINEOUT2_PA_MIXER"},
	{"LINEOUT2_PA_MIXER", NULL, "LINEOUT2 DAC"},

	{"LINEOUT3 PA", NULL, "LINEOUT3_PA_MIXER"},
	{"LINEOUT3_PA_MIXER", NULL, "LINEOUT3 DAC"},

	{"LINEOUT4 PA", NULL, "LINEOUT4_PA_MIXER"},
	{"LINEOUT4_PA_MIXER", NULL, "LINEOUT4 DAC"},

	{"LINEOUT1 DAC", NULL, "RX3 MIX1"},

	{"RDAC5 MUX", "DEM3_INV", "RX3 MIX1"},
	{"RDAC5 MUX", "DEM4", "RX4 MIX1"},

	{"LINEOUT3 DAC", NULL, "RDAC5 MUX"},

	{"LINEOUT2 DAC", NULL, "RX5 MIX1"},

	{"RDAC7 MUX", "DEM5_INV", "RX5 MIX1"},
	{"RDAC7 MUX", "DEM6", "RX6 MIX1"},

	{"LINEOUT4 DAC", NULL, "RDAC7 MUX"},

	{"SPK PA", NULL, "SPK DAC"},
	{"SPK DAC", NULL, "RX7 MIX2"},
	{"SPK DAC", NULL, "VDD_SPKDRV"},

	{"CLASS_H_DSM MUX", "DSM_HPHL_RX1", "RX1 CHAIN"},

	{"RX1 INTERP", NULL, "RX1 MIX1"},
	{"RX1 CHAIN", NULL, "RX1 INTERP"},
	{"RX2 INTERP", NULL, "RX2 MIX1"},
	{"RX2 CHAIN", NULL, "RX2 INTERP"},

	{"RX1 MIX2", NULL, "ANC1 MUX"},
	{"RX2 MIX2", NULL, "ANC2 MUX"},

	{"LINEOUT1 DAC", NULL, "RX_BIAS"},
	{"LINEOUT2 DAC", NULL, "RX_BIAS"},
	{"LINEOUT3 DAC", NULL, "RX_BIAS"},
	{"LINEOUT4 DAC", NULL, "RX_BIAS"},
	{"SPK DAC", NULL, "RX_BIAS"},

	{"RX7 MIX1", NULL, "COMP0_CLK"},
	{"RX1 MIX1", NULL, "COMP1_CLK"},
	{"RX2 MIX1", NULL, "COMP1_CLK"},
	{"RX3 MIX1", NULL, "COMP2_CLK"},
	{"RX5 MIX1", NULL, "COMP2_CLK"},

	{"RX1 MIX1", NULL, "RX1 MIX1 INP1"},
	{"RX1 MIX1", NULL, "RX1 MIX1 INP2"},
	{"RX1 MIX1", NULL, "RX1 MIX1 INP3"},
	{"RX2 MIX1", NULL, "RX2 MIX1 INP1"},
	{"RX2 MIX1", NULL, "RX2 MIX1 INP2"},
	{"RX2 MIX1", NULL, "RX2 MIX1 INP3"},
	{"RX3 MIX1", NULL, "RX3 MIX1 INP1"},
	{"RX3 MIX1", NULL, "RX3 MIX1 INP2"},
	{"RX4 MIX1", NULL, "RX4 MIX1 INP1"},
	{"RX4 MIX1", NULL, "RX4 MIX1 INP2"},
	{"RX5 MIX1", NULL, "RX5 MIX1 INP1"},
	{"RX5 MIX1", NULL, "RX5 MIX1 INP2"},
	{"RX6 MIX1", NULL, "RX6 MIX1 INP1"},
	{"RX6 MIX1", NULL, "RX6 MIX1 INP2"},
	{"RX7 MIX1", NULL, "RX7 MIX1 INP1"},
	{"RX7 MIX1", NULL, "RX7 MIX1 INP2"},
	{"RX1 MIX2", NULL, "RX1 MIX1"},
	{"RX1 MIX2", NULL, "RX1 MIX2 INP1"},
	{"RX1 MIX2", NULL, "RX1 MIX2 INP2"},
	{"RX2 MIX2", NULL, "RX2 MIX1"},
	{"RX2 MIX2", NULL, "RX2 MIX2 INP1"},
	{"RX2 MIX2", NULL, "RX2 MIX2 INP2"},
	{"RX7 MIX2", NULL, "RX7 MIX1"},
	{"RX7 MIX2", NULL, "RX7 MIX2 INP1"},
	{"RX7 MIX2", NULL, "RX7 MIX2 INP2"},

	/* SLIM_MUX("AIF1_PB", "AIF1 PB"),*/
	{"SLIM RX1 MUX", "AIF1_PB", "AIF1 PB"},
	{"SLIM RX2 MUX", "AIF1_PB", "AIF1 PB"},
	{"SLIM RX3 MUX", "AIF1_PB", "AIF1 PB"},
	{"SLIM RX4 MUX", "AIF1_PB", "AIF1 PB"},
	{"SLIM RX5 MUX", "AIF1_PB", "AIF1 PB"},
	{"SLIM RX6 MUX", "AIF1_PB", "AIF1 PB"},
	{"SLIM RX7 MUX", "AIF1_PB", "AIF1 PB"},
	/* SLIM_MUX("AIF2_PB", "AIF2 PB"),*/
	{"SLIM RX1 MUX", "AIF2_PB", "AIF2 PB"},
	{"SLIM RX2 MUX", "AIF2_PB", "AIF2 PB"},
	{"SLIM RX3 MUX", "AIF2_PB", "AIF2 PB"},
	{"SLIM RX4 MUX", "AIF2_PB", "AIF2 PB"},
	{"SLIM RX5 MUX", "AIF2_PB", "AIF2 PB"},
	{"SLIM RX6 MUX", "AIF2_PB", "AIF2 PB"},
	{"SLIM RX7 MUX", "AIF2_PB", "AIF2 PB"},
	/* SLIM_MUX("AIF3_PB", "AIF3 PB"),*/
	{"SLIM RX1 MUX", "AIF3_PB", "AIF3 PB"},
	{"SLIM RX2 MUX", "AIF3_PB", "AIF3 PB"},
	{"SLIM RX3 MUX", "AIF3_PB", "AIF3 PB"},
	{"SLIM RX4 MUX", "AIF3_PB", "AIF3 PB"},
	{"SLIM RX5 MUX", "AIF3_PB", "AIF3 PB"},
	{"SLIM RX6 MUX", "AIF3_PB", "AIF3 PB"},
	{"SLIM RX7 MUX", "AIF3_PB", "AIF3 PB"},

	{"SLIM RX1", NULL, "SLIM RX1 MUX"},
	{"SLIM RX2", NULL, "SLIM RX2 MUX"},
	{"SLIM RX3", NULL, "SLIM RX3 MUX"},
	{"SLIM RX4", NULL, "SLIM RX4 MUX"},
	{"SLIM RX5", NULL, "SLIM RX5 MUX"},
	{"SLIM RX6", NULL, "SLIM RX6 MUX"},
	{"SLIM RX7", NULL, "SLIM RX7 MUX"},

	{"RX1 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX1 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX1 MIX1 INP1", "RX3", "SLIM RX3"},
	{"RX1 MIX1 INP1", "RX4", "SLIM RX4"},
	{"RX1 MIX1 INP1", "RX5", "SLIM RX5"},
	{"RX1 MIX1 INP1", "RX6", "SLIM RX6"},
	{"RX1 MIX1 INP1", "RX7", "SLIM RX7"},
	{"RX1 MIX1 INP1", "IIR1", "IIR1"},
	{"RX1 MIX1 INP1", "IIR2", "IIR2"},
	{"RX1 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX1 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX1 MIX1 INP2", "RX3", "SLIM RX3"},
	{"RX1 MIX1 INP2", "RX4", "SLIM RX4"},
	{"RX1 MIX1 INP2", "RX5", "SLIM RX5"},
	{"RX1 MIX1 INP2", "RX6", "SLIM RX6"},
	{"RX1 MIX1 INP2", "RX7", "SLIM RX7"},
	{"RX1 MIX1 INP2", "IIR1", "IIR1"},
	{"RX1 MIX1 INP2", "IIR2", "IIR2"},
	{"RX1 MIX1 INP3", "RX1", "SLIM RX1"},
	{"RX1 MIX1 INP3", "RX2", "SLIM RX2"},
	{"RX1 MIX1 INP3", "RX3", "SLIM RX3"},
	{"RX1 MIX1 INP3", "RX4", "SLIM RX4"},
	{"RX1 MIX1 INP3", "RX5", "SLIM RX5"},
	{"RX1 MIX1 INP3", "RX6", "SLIM RX6"},
	{"RX1 MIX1 INP3", "RX7", "SLIM RX7"},
	{"RX1 MIX1 INP3", "IIR1", "IIR1"},
	{"RX1 MIX1 INP3", "IIR2", "IIR2"},
	{"RX2 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX2 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX2 MIX1 INP1", "RX3", "SLIM RX3"},
	{"RX2 MIX1 INP1", "RX4", "SLIM RX4"},
	{"RX2 MIX1 INP1", "RX5", "SLIM RX5"},
	{"RX2 MIX1 INP1", "RX6", "SLIM RX6"},
	{"RX2 MIX1 INP1", "RX7", "SLIM RX7"},
	{"RX2 MIX1 INP1", "IIR1", "IIR1"},
	{"RX2 MIX1 INP1", "IIR2", "IIR2"},
	{"RX2 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX2 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX2 MIX1 INP2", "RX3", "SLIM RX3"},
	{"RX2 MIX1 INP2", "RX4", "SLIM RX4"},
	{"RX2 MIX1 INP2", "RX5", "SLIM RX5"},
	{"RX2 MIX1 INP2", "RX6", "SLIM RX6"},
	{"RX2 MIX1 INP2", "RX7", "SLIM RX7"},
	{"RX2 MIX1 INP2", "IIR1", "IIR1"},
	{"RX2 MIX1 INP2", "IIR2", "IIR2"},
	{"RX2 MIX1 INP3", "RX1", "SLIM RX1"},
	{"RX2 MIX1 INP3", "RX2", "SLIM RX2"},
	{"RX2 MIX1 INP3", "RX3", "SLIM RX3"},
	{"RX2 MIX1 INP3", "RX4", "SLIM RX4"},
	{"RX2 MIX1 INP3", "RX5", "SLIM RX5"},
	{"RX2 MIX1 INP3", "RX6", "SLIM RX6"},
	{"RX2 MIX1 INP3", "RX7", "SLIM RX7"},
	{"RX2 MIX1 INP3", "IIR1", "IIR1"},
	{"RX2 MIX1 INP3", "IIR2", "IIR2"},
	{"RX3 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX3 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX3 MIX1 INP1", "RX3", "SLIM RX3"},
	{"RX3 MIX1 INP1", "RX4", "SLIM RX4"},
	{"RX3 MIX1 INP1", "RX5", "SLIM RX5"},
	{"RX3 MIX1 INP1", "RX6", "SLIM RX6"},
	{"RX3 MIX1 INP1", "RX7", "SLIM RX7"},
	{"RX3 MIX1 INP1", "IIR1", "IIR1"},
	{"RX3 MIX1 INP1", "IIR2", "IIR2"},
	{"RX3 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX3 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX3 MIX1 INP2", "RX3", "SLIM RX3"},
	{"RX3 MIX1 INP2", "RX4", "SLIM RX4"},
	{"RX3 MIX1 INP2", "RX5", "SLIM RX5"},
	{"RX3 MIX1 INP2", "RX6", "SLIM RX6"},
	{"RX3 MIX1 INP2", "RX7", "SLIM RX7"},
	{"RX3 MIX1 INP2", "IIR1", "IIR1"},
	{"RX3 MIX1 INP2", "IIR2", "IIR2"},
	{"RX4 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX4 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX4 MIX1 INP1", "RX3", "SLIM RX3"},
	{"RX4 MIX1 INP1", "RX4", "SLIM RX4"},
	{"RX4 MIX1 INP1", "RX5", "SLIM RX5"},
	{"RX4 MIX1 INP1", "RX6", "SLIM RX6"},
	{"RX4 MIX1 INP1", "RX7", "SLIM RX7"},
	{"RX4 MIX1 INP1", "IIR1", "IIR1"},
	{"RX4 MIX1 INP1", "IIR2", "IIR2"},
	{"RX4 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX4 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX4 MIX1 INP2", "RX3", "SLIM RX3"},
	{"RX4 MIX1 INP2", "RX5", "SLIM RX5"},
	{"RX4 MIX1 INP2", "RX4", "SLIM RX4"},
	{"RX4 MIX1 INP2", "RX6", "SLIM RX6"},
	{"RX4 MIX1 INP2", "RX7", "SLIM RX7"},
	{"RX4 MIX1 INP2", "IIR1", "IIR1"},
	{"RX4 MIX1 INP2", "IIR2", "IIR2"},
	{"RX5 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX5 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX5 MIX1 INP1", "RX3", "SLIM RX3"},
	{"RX5 MIX1 INP1", "RX4", "SLIM RX4"},
	{"RX5 MIX1 INP1", "RX5", "SLIM RX5"},
	{"RX5 MIX1 INP1", "RX6", "SLIM RX6"},
	{"RX5 MIX1 INP1", "RX7", "SLIM RX7"},
	{"RX5 MIX1 INP1", "IIR1", "IIR1"},
	{"RX5 MIX1 INP1", "IIR2", "IIR2"},
	{"RX5 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX5 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX5 MIX1 INP2", "RX3", "SLIM RX3"},
	{"RX5 MIX1 INP2", "RX4", "SLIM RX4"},
	{"RX5 MIX1 INP2", "RX5", "SLIM RX5"},
	{"RX5 MIX1 INP2", "RX6", "SLIM RX6"},
	{"RX5 MIX1 INP2", "RX7", "SLIM RX7"},
	{"RX5 MIX1 INP2", "IIR1", "IIR1"},
	{"RX5 MIX1 INP2", "IIR2", "IIR2"},
	{"RX6 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX6 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX6 MIX1 INP1", "RX3", "SLIM RX3"},
	{"RX6 MIX1 INP1", "RX4", "SLIM RX4"},
	{"RX6 MIX1 INP1", "RX5", "SLIM RX5"},
	{"RX6 MIX1 INP1", "RX6", "SLIM RX6"},
	{"RX6 MIX1 INP1", "RX7", "SLIM RX7"},
	{"RX6 MIX1 INP1", "IIR1", "IIR1"},
	{"RX6 MIX1 INP1", "IIR2", "IIR2"},
	{"RX6 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX6 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX6 MIX1 INP2", "RX3", "SLIM RX3"},
	{"RX6 MIX1 INP2", "RX4", "SLIM RX4"},
	{"RX6 MIX1 INP2", "RX5", "SLIM RX5"},
	{"RX6 MIX1 INP2", "RX6", "SLIM RX6"},
	{"RX6 MIX1 INP2", "RX7", "SLIM RX7"},
	{"RX6 MIX1 INP2", "IIR1", "IIR1"},
	{"RX6 MIX1 INP2", "IIR2", "IIR2"},
	{"RX7 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX7 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX7 MIX1 INP1", "RX3", "SLIM RX3"},
	{"RX7 MIX1 INP1", "RX4", "SLIM RX4"},
	{"RX7 MIX1 INP1", "RX5", "SLIM RX5"},
	{"RX7 MIX1 INP1", "RX6", "SLIM RX6"},
	{"RX7 MIX1 INP1", "RX7", "SLIM RX7"},
	{"RX7 MIX1 INP1", "IIR1", "IIR1"},
	{"RX7 MIX1 INP1", "IIR2", "IIR2"},
	{"RX7 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX7 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX7 MIX1 INP2", "RX3", "SLIM RX3"},
	{"RX7 MIX1 INP2", "RX4", "SLIM RX4"},
	{"RX7 MIX1 INP2", "RX5", "SLIM RX5"},
	{"RX7 MIX1 INP2", "RX6", "SLIM RX6"},
	{"RX7 MIX1 INP2", "RX7", "SLIM RX7"},
	{"RX7 MIX1 INP2", "IIR1", "IIR1"},
	{"RX7 MIX1 INP2", "IIR2", "IIR2"},

	/* IIR1, IIR2 inputs to Second RX Mixer on RX1, RX2 and RX7 chains. */
	{"RX1 MIX2 INP1", "IIR1", "IIR1"},
	{"RX1 MIX2 INP2", "IIR1", "IIR1"},
	{"RX2 MIX2 INP1", "IIR1", "IIR1"},
	{"RX2 MIX2 INP2", "IIR1", "IIR1"},
	{"RX7 MIX2 INP1", "IIR1", "IIR1"},
	{"RX7 MIX2 INP2", "IIR1", "IIR1"},
	{"RX1 MIX2 INP1", "IIR2", "IIR2"},
	{"RX1 MIX2 INP2", "IIR2", "IIR2"},
	{"RX2 MIX2 INP1", "IIR2", "IIR2"},
	{"RX2 MIX2 INP2", "IIR2", "IIR2"},
	{"RX7 MIX2 INP1", "IIR2", "IIR2"},
	{"RX7 MIX2 INP2", "IIR2", "IIR2"},

	/* Decimator Inputs */
	{"DEC1 MUX", "DMIC1", "DMIC1"},
	{"DEC1 MUX", "ADC6", "ADC6"},
	{"DEC1 MUX", NULL, "CDC_CONN"},
	{"DEC2 MUX", "DMIC2", "DMIC2"},
	{"DEC2 MUX", "ADC5", "ADC5"},
	{"DEC2 MUX", NULL, "CDC_CONN"},
	{"DEC3 MUX", "DMIC3", "DMIC3"},
	{"DEC3 MUX", "ADC4", "ADC4"},
	{"DEC3 MUX", NULL, "CDC_CONN"},
	{"DEC4 MUX", "DMIC4", "DMIC4"},
	{"DEC4 MUX", "ADC3", "ADC3"},
	{"DEC4 MUX", NULL, "CDC_CONN"},
	{"DEC5 MUX", "DMIC5", "DMIC5"},
	{"DEC5 MUX", "ADC2", "ADC2"},
	{"DEC5 MUX", NULL, "CDC_CONN"},
	{"DEC6 MUX", "DMIC6", "DMIC6"},
	{"DEC6 MUX", "ADC1", "ADC1"},
	{"DEC6 MUX", NULL, "CDC_CONN"},
	{"DEC7 MUX", "DMIC1", "DMIC1"},
	{"DEC7 MUX", "DMIC6", "DMIC6"},
	{"DEC7 MUX", "ADC1", "ADC1"},
	{"DEC7 MUX", "ADC6", "ADC6"},
	{"DEC7 MUX", NULL, "CDC_CONN"},
	{"DEC8 MUX", "DMIC2", "DMIC2"},
	{"DEC8 MUX", "DMIC5", "DMIC5"},
	{"DEC8 MUX", "ADC2", "ADC2"},
	{"DEC8 MUX", "ADC5", "ADC5"},
	{"DEC8 MUX", NULL, "CDC_CONN"},
	{"DEC9 MUX", "DMIC4", "DMIC4"},
	{"DEC9 MUX", "DMIC5", "DMIC5"},
	{"DEC9 MUX", "ADC2", "ADC2"},
	{"DEC9 MUX", "ADC3", "ADC3"},
	{"DEC9 MUX", NULL, "CDC_CONN"},
	{"DEC10 MUX", "DMIC3", "DMIC3"},
	{"DEC10 MUX", "DMIC6", "DMIC6"},
	{"DEC10 MUX", "ADC1", "ADC1"},
	{"DEC10 MUX", "ADC4", "ADC4"},
	{"DEC10 MUX", NULL, "CDC_CONN"},

	/* ADC Connections */
	{"ADC1", NULL, "AMIC1"},
	{"ADC2", NULL, "AMIC2"},
	{"ADC3", NULL, "AMIC3"},
	{"ADC4", NULL, "AMIC4"},
	{"ADC5", NULL, "AMIC5"},
	{"ADC6", NULL, "AMIC6"},

	/* AUX PGA Connections */
	{"EAR_PA_MIXER", "AUX_PGA_L Switch", "AUX_PGA_Left"},
	{"HPHL_PA_MIXER", "AUX_PGA_L Switch", "AUX_PGA_Left"},
	{"HPHR_PA_MIXER", "AUX_PGA_R Switch", "AUX_PGA_Right"},
	{"LINEOUT1_PA_MIXER", "AUX_PGA_L Switch", "AUX_PGA_Left"},
	{"LINEOUT2_PA_MIXER", "AUX_PGA_R Switch", "AUX_PGA_Right"},
	{"LINEOUT3_PA_MIXER", "AUX_PGA_L Switch", "AUX_PGA_Left"},
	{"LINEOUT4_PA_MIXER", "AUX_PGA_R Switch", "AUX_PGA_Right"},
	{"AUX_PGA_Left", NULL, "AMIC5"},
	{"AUX_PGA_Right", NULL, "AMIC6"},

	{"IIR1", NULL, "IIR1 INP1 MUX"},
	{"IIR1 INP1 MUX", "DEC1", "DEC1 MUX"},
	{"IIR1 INP1 MUX", "DEC2", "DEC2 MUX"},
	{"IIR1 INP1 MUX", "DEC3", "DEC3 MUX"},
	{"IIR1 INP1 MUX", "DEC4", "DEC4 MUX"},
	{"IIR1 INP1 MUX", "DEC5", "DEC5 MUX"},
	{"IIR1 INP1 MUX", "DEC6", "DEC6 MUX"},
	{"IIR1 INP1 MUX", "DEC7", "DEC7 MUX"},
	{"IIR1 INP1 MUX", "DEC8", "DEC8 MUX"},
	{"IIR1 INP1 MUX", "DEC9", "DEC9 MUX"},
	{"IIR1 INP1 MUX", "DEC10", "DEC10 MUX"},
	{"IIR1 INP1 MUX", "RX1", "SLIM RX1"},
	{"IIR1 INP1 MUX", "RX2", "SLIM RX2"},
	{"IIR1 INP1 MUX", "RX3", "SLIM RX3"},
	{"IIR1 INP1 MUX", "RX4", "SLIM RX4"},
	{"IIR1 INP1 MUX", "RX5", "SLIM RX5"},
	{"IIR1 INP1 MUX", "RX6", "SLIM RX6"},
	{"IIR1 INP1 MUX", "RX7", "SLIM RX7"},

	{"IIR1", NULL, "IIR1 INP2 MUX"},
	{"IIR1 INP2 MUX", "DEC1", "DEC1 MUX"},
	{"IIR1 INP2 MUX", "DEC2", "DEC2 MUX"},
	{"IIR1 INP2 MUX", "DEC3", "DEC3 MUX"},
	{"IIR1 INP2 MUX", "DEC4", "DEC4 MUX"},
	{"IIR1 INP2 MUX", "DEC5", "DEC5 MUX"},
	{"IIR1 INP2 MUX", "DEC6", "DEC6 MUX"},
	{"IIR1 INP2 MUX", "DEC7", "DEC7 MUX"},
	{"IIR1 INP2 MUX", "DEC8", "DEC8 MUX"},
	{"IIR1 INP2 MUX", "DEC9", "DEC9 MUX"},
	{"IIR1 INP2 MUX", "DEC10", "DEC10 MUX"},
	{"IIR1 INP2 MUX", "RX1", "SLIM RX1"},
	{"IIR1 INP2 MUX", "RX2", "SLIM RX2"},
	{"IIR1 INP2 MUX", "RX3", "SLIM RX3"},
	{"IIR1 INP2 MUX", "RX4", "SLIM RX4"},
	{"IIR1 INP2 MUX", "RX5", "SLIM RX5"},
	{"IIR1 INP2 MUX", "RX6", "SLIM RX6"},
	{"IIR1 INP2 MUX", "RX7", "SLIM RX7"},

	{"IIR1", NULL, "IIR1 INP3 MUX"},
	{"IIR1 INP3 MUX", "DEC1", "DEC1 MUX"},
	{"IIR1 INP3 MUX", "DEC2", "DEC2 MUX"},
	{"IIR1 INP3 MUX", "DEC3", "DEC3 MUX"},
	{"IIR1 INP3 MUX", "DEC4", "DEC4 MUX"},
	{"IIR1 INP3 MUX", "DEC5", "DEC5 MUX"},
	{"IIR1 INP3 MUX", "DEC6", "DEC6 MUX"},
	{"IIR1 INP3 MUX", "DEC7", "DEC7 MUX"},
	{"IIR1 INP3 MUX", "DEC8", "DEC8 MUX"},
	{"IIR1 INP3 MUX", "DEC9", "DEC9 MUX"},
	{"IIR1 INP3 MUX", "DEC10", "DEC10 MUX"},
	{"IIR1 INP3 MUX", "RX1", "SLIM RX1"},
	{"IIR1 INP3 MUX", "RX2", "SLIM RX2"},
	{"IIR1 INP3 MUX", "RX3", "SLIM RX3"},
	{"IIR1 INP3 MUX", "RX4", "SLIM RX4"},
	{"IIR1 INP3 MUX", "RX5", "SLIM RX5"},
	{"IIR1 INP3 MUX", "RX6", "SLIM RX6"},
	{"IIR1 INP3 MUX", "RX7", "SLIM RX7"},

	{"IIR1", NULL, "IIR1 INP4 MUX"},
	{"IIR1 INP4 MUX", "DEC1", "DEC1 MUX"},
	{"IIR1 INP4 MUX", "DEC2", "DEC2 MUX"},
	{"IIR1 INP4 MUX", "DEC3", "DEC3 MUX"},
	{"IIR1 INP4 MUX", "DEC4", "DEC4 MUX"},
	{"IIR1 INP4 MUX", "DEC5", "DEC5 MUX"},
	{"IIR1 INP4 MUX", "DEC6", "DEC6 MUX"},
	{"IIR1 INP4 MUX", "DEC7", "DEC7 MUX"},
	{"IIR1 INP4 MUX", "DEC8", "DEC8 MUX"},
	{"IIR1 INP4 MUX", "DEC9", "DEC9 MUX"},
	{"IIR1 INP4 MUX", "DEC10", "DEC10 MUX"},
	{"IIR1 INP4 MUX", "RX1", "SLIM RX1"},
	{"IIR1 INP4 MUX", "RX2", "SLIM RX2"},
	{"IIR1 INP4 MUX", "RX3", "SLIM RX3"},
	{"IIR1 INP4 MUX", "RX4", "SLIM RX4"},
	{"IIR1 INP4 MUX", "RX5", "SLIM RX5"},
	{"IIR1 INP4 MUX", "RX6", "SLIM RX6"},
	{"IIR1 INP4 MUX", "RX7", "SLIM RX7"},

	{"IIR2", NULL, "IIR2 INP1 MUX"},
	{"IIR2 INP1 MUX", "DEC1", "DEC1 MUX"},
	{"IIR2 INP1 MUX", "DEC2", "DEC2 MUX"},
	{"IIR2 INP1 MUX", "DEC3", "DEC3 MUX"},
	{"IIR2 INP1 MUX", "DEC4", "DEC4 MUX"},
	{"IIR2 INP1 MUX", "DEC5", "DEC5 MUX"},
	{"IIR2 INP1 MUX", "DEC6", "DEC6 MUX"},
	{"IIR2 INP1 MUX", "DEC7", "DEC7 MUX"},
	{"IIR2 INP1 MUX", "DEC8", "DEC8 MUX"},
	{"IIR2 INP1 MUX", "DEC9", "DEC9 MUX"},
	{"IIR2 INP1 MUX", "DEC10", "DEC10 MUX"},
	{"IIR2 INP1 MUX", "RX1", "SLIM RX1"},
	{"IIR2 INP1 MUX", "RX2", "SLIM RX2"},
	{"IIR2 INP1 MUX", "RX3", "SLIM RX3"},
	{"IIR2 INP1 MUX", "RX4", "SLIM RX4"},
	{"IIR2 INP1 MUX", "RX5", "SLIM RX5"},
	{"IIR2 INP1 MUX", "RX6", "SLIM RX6"},
	{"IIR2 INP1 MUX", "RX7", "SLIM RX7"},

	{"IIR2", NULL, "IIR2 INP2 MUX"},
	{"IIR2 INP2 MUX", "DEC1", "DEC1 MUX"},
	{"IIR2 INP2 MUX", "DEC2", "DEC2 MUX"},
	{"IIR2 INP2 MUX", "DEC3", "DEC3 MUX"},
	{"IIR2 INP2 MUX", "DEC4", "DEC4 MUX"},
	{"IIR2 INP2 MUX", "DEC5", "DEC5 MUX"},
	{"IIR2 INP2 MUX", "DEC6", "DEC6 MUX"},
	{"IIR2 INP2 MUX", "DEC7", "DEC7 MUX"},
	{"IIR2 INP2 MUX", "DEC8", "DEC8 MUX"},
	{"IIR2 INP2 MUX", "DEC9", "DEC9 MUX"},
	{"IIR2 INP2 MUX", "DEC10", "DEC10 MUX"},
	{"IIR2 INP2 MUX", "RX1", "SLIM RX1"},
	{"IIR2 INP2 MUX", "RX2", "SLIM RX2"},
	{"IIR2 INP2 MUX", "RX3", "SLIM RX3"},
	{"IIR2 INP2 MUX", "RX4", "SLIM RX4"},
	{"IIR2 INP2 MUX", "RX5", "SLIM RX5"},
	{"IIR2 INP2 MUX", "RX6", "SLIM RX6"},
	{"IIR2 INP2 MUX", "RX7", "SLIM RX7"},

	{"IIR2", NULL, "IIR2 INP3 MUX"},
	{"IIR2 INP3 MUX", "DEC1", "DEC1 MUX"},
	{"IIR2 INP3 MUX", "DEC2", "DEC2 MUX"},
	{"IIR2 INP3 MUX", "DEC3", "DEC3 MUX"},
	{"IIR2 INP3 MUX", "DEC4", "DEC4 MUX"},
	{"IIR2 INP3 MUX", "DEC5", "DEC5 MUX"},
	{"IIR2 INP3 MUX", "DEC6", "DEC6 MUX"},
	{"IIR2 INP3 MUX", "DEC7", "DEC7 MUX"},
	{"IIR2 INP3 MUX", "DEC8", "DEC8 MUX"},
	{"IIR2 INP3 MUX", "DEC9", "DEC9 MUX"},
	{"IIR2 INP3 MUX", "DEC10", "DEC10 MUX"},
	{"IIR2 INP3 MUX", "RX1", "SLIM RX1"},
	{"IIR2 INP3 MUX", "RX2", "SLIM RX2"},
	{"IIR2 INP3 MUX", "RX3", "SLIM RX3"},
	{"IIR2 INP3 MUX", "RX4", "SLIM RX4"},
	{"IIR2 INP3 MUX", "RX5", "SLIM RX5"},
	{"IIR2 INP3 MUX", "RX6", "SLIM RX6"},
	{"IIR2 INP3 MUX", "RX7", "SLIM RX7"},

	{"IIR2", NULL, "IIR2 INP4 MUX"},
	{"IIR2 INP4 MUX", "DEC1", "DEC1 MUX"},
	{"IIR2 INP4 MUX", "DEC2", "DEC2 MUX"},
	{"IIR2 INP4 MUX", "DEC3", "DEC3 MUX"},
	{"IIR2 INP4 MUX", "DEC4", "DEC4 MUX"},
	{"IIR2 INP4 MUX", "DEC5", "DEC5 MUX"},
	{"IIR2 INP4 MUX", "DEC6", "DEC6 MUX"},
	{"IIR2 INP4 MUX", "DEC7", "DEC7 MUX"},
	{"IIR2 INP4 MUX", "DEC8", "DEC8 MUX"},
	{"IIR2 INP4 MUX", "DEC9", "DEC9 MUX"},
	{"IIR2 INP4 MUX", "DEC10", "DEC10 MUX"},
	{"IIR2 INP4 MUX", "RX1", "SLIM RX1"},
	{"IIR2 INP4 MUX", "RX2", "SLIM RX2"},
	{"IIR2 INP4 MUX", "RX3", "SLIM RX3"},
	{"IIR2 INP4 MUX", "RX4", "SLIM RX4"},
	{"IIR2 INP4 MUX", "RX5", "SLIM RX5"},
	{"IIR2 INP4 MUX", "RX6", "SLIM RX6"},
	{"IIR2 INP4 MUX", "RX7", "SLIM RX7"},

	{"MIC BIAS1 Internal1", NULL, "LDO_H"},
	{"MIC BIAS1 Internal2", NULL, "LDO_H"},
	{"MIC BIAS1 External", NULL, "LDO_H"},
	{"MIC BIAS2 Internal1", NULL, "LDO_H"},
	{"MIC BIAS2 Internal2", NULL, "LDO_H"},
	{"MIC BIAS2 Internal3", NULL, "LDO_H"},
	{"MIC BIAS2 External", NULL, "LDO_H"},
	{"MIC BIAS3 Internal1", NULL, "LDO_H"},
	{"MIC BIAS3 Internal2", NULL, "LDO_H"},
	{"MIC BIAS3 External", NULL, "LDO_H"},
	{"MIC BIAS4 External", NULL, "LDO_H"},
	{"Main Mic Bias", NULL, "LDO_H"},
	{DAPM_MICBIAS2_EXTERNAL_STANDALONE, NULL, "LDO_H Standalone"},
	{DAPM_MICBIAS3_EXTERNAL_STANDALONE, NULL, "LDO_H Standalone"},
	{"Ear Mic Bias", NULL, "LDO_H"},
};

static int taiko_readable(struct snd_soc_codec *ssc, unsigned int reg)
{
	return taiko_reg_readable[reg];
}

static unsigned int taiko_is_digital_gain_register(unsigned int reg)
{
	switch (reg) {
		case TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX3_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX4_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX5_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX6_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_TX1_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX2_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX3_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX4_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX5_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX6_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX7_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX8_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX9_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX10_VOL_CTL_GAIN:
			return 1;
			break;
		default:
			break;
	}

	return 0;
}

static int taiko_volatile(struct snd_soc_codec *ssc, unsigned int reg)
{
	int i;

	/* Registers lower than 0x100 are top level registers which can be
	 * written by the Taiko core driver.
	 */

	if ((reg >= TAIKO_A_CDC_MBHC_EN_CTL) || (reg < 0x100))
		return 1;

	/* IIR Coeff registers are not cacheable */
	if ((reg >= TAIKO_A_CDC_IIR1_COEF_B1_CTL) &&
		(reg <= TAIKO_A_CDC_IIR2_COEF_B2_CTL))
		return 1;

	/* ANC filter registers are not cacheable */
	if ((reg >= TAIKO_A_CDC_ANC1_IIR_B1_CTL) &&
		(reg <= TAIKO_A_CDC_ANC1_LPF_B2_CTL))
		return 1;
	if ((reg >= TAIKO_A_CDC_ANC2_IIR_B1_CTL) &&
		(reg <= TAIKO_A_CDC_ANC2_LPF_B2_CTL))
		return 1;

	/* Digital gain register is not cacheable so we have to write
	 * the setting even it is the same
	 */
	if (taiko_is_digital_gain_register(reg))
		return 1;

	/* HPH status registers */
	if (reg == TAIKO_A_RX_HPH_L_STATUS || reg == TAIKO_A_RX_HPH_R_STATUS)
		return 1;

	/* HPH PA Enable */
	if (reg == TAIKO_A_RX_HPH_CNP_EN)
		return 1;

	if (reg == TAIKO_A_MBHC_INSERT_DET_STATUS)
		return 1;

	switch (reg) {
	case TAIKO_A_CDC_SPKR_CLIPDET_VAL0:
	case TAIKO_A_CDC_SPKR_CLIPDET_VAL1:
	case TAIKO_A_CDC_SPKR_CLIPDET_VAL2:
	case TAIKO_A_CDC_SPKR_CLIPDET_VAL3:
	case TAIKO_A_CDC_SPKR_CLIPDET_VAL4:
	case TAIKO_A_CDC_SPKR_CLIPDET_VAL5:
	case TAIKO_A_CDC_SPKR_CLIPDET_VAL6:
	case TAIKO_A_CDC_SPKR_CLIPDET_VAL7:
	case TAIKO_A_CDC_VBAT_GAIN_MON_VAL:
		return 1;
	}

	for (i = 0; i < ARRAY_SIZE(audio_reg_cfg); i++)
		if ((audio_reg_cfg[i].reg_logical_addr -
		    TAIKO_REGISTER_START_OFFSET) == reg)
			return 1;

	return 0;
}

static int taiko_write(struct snd_soc_codec *codec, unsigned int reg,
	unsigned int value)
{
	int ret;
	struct wcd9xxx *wcd9xxx;

	if (reg < 0 || reg > TAIKO_MAX_REGISTER)
		return -ERANGE;

	if (codec == NULL)
		return -ENOMEM;

	wcd9xxx = codec->control_data;

	if (!taiko_volatile(codec, reg)) {
		ret = snd_soc_cache_write(codec, reg, value);
		if (ret != 0)
			pr_debug("%s: Cache write to %x failed: %d\n",
				__func__, reg, ret);
	}

	return wcd9xxx_reg_write(&wcd9xxx->core_res, reg, value);
}

static unsigned int taiko_read(struct snd_soc_codec *codec,
				unsigned int reg)
{
	unsigned int val;
	int ret;

	struct wcd9xxx *wcd9xxx;

	if (reg < 0 || reg > TAIKO_MAX_REGISTER)
		return -ERANGE;

	if (codec == NULL)
		return -ENOMEM;

	wcd9xxx = codec->control_data;

	if (!taiko_volatile(codec, reg) && taiko_readable(codec, reg) &&
		reg < codec->driver->reg_cache_size) {
		ret = snd_soc_cache_read(codec, reg, &val);
		if (ret >= 0) {
			return val;
		}
	pr_debug("%s: snd_soc_cache_read from %x failed: %d\n",
				__func__, reg, ret);
	}

	val = wcd9xxx_reg_read(&wcd9xxx->core_res, reg);
	return val;
}

#ifdef CONFIG_SND_SOC_ES325
static int taiko_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct wcd9xxx *taiko_core = dev_get_drvdata(dai->codec->dev->parent);
	pr_debug("%s(): substream = %s  stream = %d\n" , __func__,
		 substream->name, substream->stream);
	if ((taiko_core != NULL) &&
	    (taiko_core->dev != NULL) &&
	    (taiko_core->dev->parent != NULL)) {
		es325_wrapper_wakeup(dai);
	}

	return 0;
}
#else
static int taiko_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
//	struct wcd9xxx *taiko_core = dev_get_drvdata(dai->codec->dev->parent);
	pr_debug("%s(): substream = %s  stream = %d\n" , __func__,
		 substream->name, substream->stream);
	return 0;
}
#endif

static void taiko_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct wcd9xxx *taiko_core = dev_get_drvdata(dai->codec->dev->parent);
	pr_debug("%s(): substream = %s  stream = %d\n" , __func__,
		 substream->name, substream->stream);
	if ((taiko_core != NULL) &&
	    (taiko_core->dev != NULL) &&
	    (taiko_core->dev->parent != NULL)) {
#ifdef CONFIG_SND_SOC_ES325
		es325_wrapper_sleep(dai->id);
#endif
	}
}

static int taiko_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	int paths, i;
	struct snd_soc_dapm_widget_list *wlist;
	struct snd_soc_codec *codec = dai->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);
	int found_hs_pa = 0;

	if (substream->stream) {
		taiko_p->clsh_d.hs_perf_mode_enabled = false;
        pr_debug("%s: uhqa_mode disabled by substream stream", __func__);
		goto handleshit;
	}

	pr_debug("%s(): substream = %s. stream = %d. dai->name = %s."
		" dai->driver->name = %s. dai stream_name = %s\n",
		__func__, substream->name, substream->stream,
		dai->name, dai->driver->name,
		substream->stream ? dai->driver->capture.stream_name :
		dai->driver->playback.stream_name);

	pr_debug("%s(): dai AIF widget = %s. dai playback stream_name = %s.\n"
		"  rate = %u. bit_width = %u.  hs compander_enabled = %u\n",
		__func__, dai->playback_aif ? dai->playback_aif->name : "NULL",
		dai->driver->playback.stream_name, taiko_p->dai[dai->id].rate,
			taiko_p->dai[dai->id].bit_width,
			taiko_p->comp_enabled[COMPANDER_1]);

    if (chopper_bypass) {
        taiko_p->clsh_d.hs_perf_mode_enabled = false;
        pr_debug("%s: uhqa_mode disabled by chopper_bypass", __func__);
        goto handleshit;
    }

	paths = snd_soc_dapm_codec_dai_get_playback_connected_widgets(dai, &wlist);

	if (!paths) {
		dev_dbg(dai->dev, "%s(): found no audio playback paths\n",
			__func__);
        taiko_p->clsh_d.hs_perf_mode_enabled = false;
		goto handleshit;
	}

	for (i = 0; i < wlist->num_widgets; i++) {
		dev_dbg(dai->dev, " dai stream_name = %s, widget name = %s\n",
			dai->driver->playback.stream_name, wlist->widgets[i]->name);

		if (!strcmp(wlist->widgets[i]->name, "HPHL") ||
		    !strcmp(wlist->widgets[i]->name, "HPHR")) {
			found_hs_pa = 1;
			break;
		}
	}

	kfree(wlist);

	if (!found_hs_pa) {
		taiko_p->clsh_d.hs_perf_mode_enabled = false;
		goto handleshit;
	}

	pr_debug("%s(): rate = %u. bit_width = %u.  hs compander_enabled = %u",
			__func__, taiko_p->dai[dai->id].rate,
			taiko_p->dai[dai->id].bit_width,
			taiko_p->comp_enabled[COMPANDER_1]);

	if (uhqa_mode) {
		taiko_p->clsh_d.hs_perf_mode_enabled = true;
        pr_debug("%s: uhqa_mode enabled by user", __func__);
	} else if (taiko_p->dai[dai->id].rate == 192000 ||
		(taiko_p->dai[dai->id].rate == 96000 &&
	    taiko_p->dai[dai->id].bit_width == 24)) {
		taiko_p->clsh_d.hs_perf_mode_enabled = true;
       	pr_debug("%s: uhqa_mode enabled by audio properties", __func__);
	} else {
		taiko_p->clsh_d.hs_perf_mode_enabled = false;
		pr_debug("%s: uhqa_mode disabled", __func__);
	}
handleshit:
	update_control_regs();
	write_hph_poweramp_regs();
	return 0;
}

int taiko_mclk_enable(struct snd_soc_codec *codec, int mclk_enable, bool dapm)
{
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s: mclk_enable = %u, dapm = %d\n", __func__, mclk_enable,
		 dapm);

	WCD9XXX_BG_CLK_LOCK(&taiko->resmgr);
	if (mclk_enable) {
		wcd9xxx_resmgr_get_bandgap(&taiko->resmgr,
					   WCD9XXX_BANDGAP_AUDIO_MODE);
		wcd9xxx_resmgr_get_clk_block(&taiko->resmgr, WCD9XXX_CLK_MCLK);
	} else {
		/* Put clock and BG */
		wcd9xxx_resmgr_put_clk_block(&taiko->resmgr, WCD9XXX_CLK_MCLK);
		wcd9xxx_resmgr_put_bandgap(&taiko->resmgr,
					   WCD9XXX_BANDGAP_AUDIO_MODE);
	}
	WCD9XXX_BG_CLK_UNLOCK(&taiko->resmgr);

	return 0;
}

static int taiko_set_dai_sysclk(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static int taiko_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	u8 val = 0;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(dai->codec);

	pr_debug("%s\n", __func__);
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		/* CPU is master */
		if (taiko->intf_type == WCD9XXX_INTERFACE_TYPE_I2C) {
			if (dai->id == AIF1_CAP)
				snd_soc_update_bits(dai->codec,
					TAIKO_A_CDC_CLK_TX_I2S_CTL,
					TAIKO_I2S_MASTER_MODE_MASK, 0);
			else if (dai->id == AIF1_PB)
				snd_soc_update_bits(dai->codec,
					TAIKO_A_CDC_CLK_RX_I2S_CTL,
					TAIKO_I2S_MASTER_MODE_MASK, 0);
		}
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
	/* CPU is slave */
		if (taiko->intf_type == WCD9XXX_INTERFACE_TYPE_I2C) {
			val = TAIKO_I2S_MASTER_MODE_MASK;
			if (dai->id == AIF1_CAP)
				snd_soc_update_bits(dai->codec,
					TAIKO_A_CDC_CLK_TX_I2S_CTL, val, val);
			else if (dai->id == AIF1_PB)
				snd_soc_update_bits(dai->codec,
					TAIKO_A_CDC_CLK_RX_I2S_CTL, val, val);
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int taiko_set_channel_map(struct snd_soc_dai *dai,
				unsigned int tx_num, unsigned int *tx_slot,
				unsigned int rx_num, unsigned int *rx_slot)

{
	struct wcd9xxx_codec_dai_data *dai_data = NULL;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(dai->codec);
	struct wcd9xxx *core = dev_get_drvdata(dai->codec->dev->parent);
	if (!tx_slot && !rx_slot) {
		pr_debug("%s: Invalid\n", __func__);
		return -EINVAL;
	}
	pr_debug("%s(): dai_name = %s DAI-ID %x tx_ch %d rx_ch %d\n"
		 "taiko->intf_type %d\n",
		 __func__, dai->name, dai->id, tx_num, rx_num,
		 taiko->intf_type);

	if (taiko->intf_type == WCD9XXX_INTERFACE_TYPE_SLIMBUS) {
		wcd9xxx_init_slimslave(core, core->slim->laddr,
					   tx_num, tx_slot, rx_num, rx_slot);
		/*Reserve tx11 and tx12 for VI feedback path*/
		dai_data = &taiko->dai[AIF4_VIFEED];
		if (dai_data) {
			list_add_tail(&core->tx_chs[TAIKO_TX11].list,
			&dai_data->wcd9xxx_ch_list);
			list_add_tail(&core->tx_chs[TAIKO_TX12].list,
			&dai_data->wcd9xxx_ch_list);
		}
	}
	return 0;
}

static int taiko_get_channel_map(struct snd_soc_dai *dai,
				 unsigned int *tx_num, unsigned int *tx_slot,
				 unsigned int *rx_num, unsigned int *rx_slot)

{
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(dai->codec);
	u32 i = 0;
	struct wcd9xxx_ch *ch;

	switch (dai->id) {
	case AIF1_PB:
	case AIF2_PB:
	case AIF3_PB:
		if (!rx_slot || !rx_num) {
			pr_debug("%s: Invalid rx_slot %d or rx_num %d\n",
				 __func__, (u32) rx_slot, (u32) rx_num);
			return -EINVAL;
		}
		list_for_each_entry(ch, &taiko_p->dai[dai->id].wcd9xxx_ch_list,
				    list) {
			pr_debug("%s: slot_num %u ch->ch_num %d\n",
				 __func__, i, ch->ch_num);
			rx_slot[i++] = ch->ch_num;
		}
		pr_debug("%s: rx_num %d\n", __func__, i);
		*rx_num = i;
		break;
	case AIF1_CAP:
	case AIF2_CAP:
	case AIF3_CAP:
	case AIF4_VIFEED:
	case AIF4_MAD_TX:
		if (!tx_slot || !tx_num) {
			pr_debug("%s: Invalid tx_slot %d or tx_num %d\n",
				 __func__, (u32) tx_slot, (u32) tx_num);
			return -EINVAL;
		}
		list_for_each_entry(ch, &taiko_p->dai[dai->id].wcd9xxx_ch_list,
				    list) {
			pr_debug("%s: slot_num %u ch->ch_num %d\n",
				 __func__, i,  ch->ch_num);
			tx_slot[i++] = ch->ch_num;
		}
		pr_debug("%s: tx_num %d\n", __func__, i);
		*tx_num = i;
		break;

	default:
		pr_debug("%s: Invalid DAI ID %x\n", __func__, dai->id);
		break;
	}

	return 0;
}

static int taiko_set_interpolator_rate(struct snd_soc_dai *dai,
	u8 rx_fs_rate_reg_val, u32 compander_fs)
{
	u32 j;
	u8 rx_mix1_inp;
	u16 rx_mix_1_reg_1, rx_mix_1_reg_2;
	u16 rx_fs_reg;
	u8 rx_mix_1_reg_1_val, rx_mix_1_reg_2_val;
	struct snd_soc_codec *codec = dai->codec;
	struct wcd9xxx_ch *ch;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	list_for_each_entry(ch, &taiko->dai[dai->id].wcd9xxx_ch_list, list) {
		/* for RX port starting from 16 instead of 10 like tabla */
		rx_mix1_inp = ch->port + RX_MIX1_INP_SEL_RX1 -
			      TAIKO_TX_PORT_NUMBER;
		if ((rx_mix1_inp < RX_MIX1_INP_SEL_RX1) ||
			(rx_mix1_inp > RX_MIX1_INP_SEL_RX7)) {
			pr_debug("%s: Invalid TAIKO_RX%u port. Dai ID is %d\n",
				__func__,  rx_mix1_inp - 5 , dai->id);
			return -EINVAL;
		}

		rx_mix_1_reg_1 = TAIKO_A_CDC_CONN_RX1_B1_CTL;

		for (j = 0; j < NUM_INTERPOLATORS; j++) {
			rx_mix_1_reg_2 = rx_mix_1_reg_1 + 1;

			rx_mix_1_reg_1_val = snd_soc_read(codec,
							  rx_mix_1_reg_1);
			rx_mix_1_reg_2_val = snd_soc_read(codec,
							  rx_mix_1_reg_2);

			if (((rx_mix_1_reg_1_val & 0x0F) == rx_mix1_inp) ||
			    (((rx_mix_1_reg_1_val >> 4) & 0x0F)
				== rx_mix1_inp) ||
			    ((rx_mix_1_reg_2_val & 0x0F) == rx_mix1_inp)) {

                if (j)
                    rx_fs_reg = TAIKO_A_CDC_RX1_B5_CTL + (8 * j);
                else
                    rx_fs_reg = TAIKO_A_CDC_RX1_B5_CTL;

				pr_debug("%s: AIF_PB DAI(%d) connected to RX%u\n",
					__func__, dai->id, j + 1);

				pr_debug("%s: set RX%u sample rate",
					__func__, j + 1);

				snd_soc_update_bits(codec, rx_fs_reg,
						0xE0, rx_fs_rate_reg_val);

				if (comp_rx_path[j] < COMPANDER_MAX)
					taiko->comp_fs[comp_rx_path[j]] = compander_fs;
			}
			if (j < 2)
				rx_mix_1_reg_1 += 3;
			else
				rx_mix_1_reg_1 += 2;
		}
	}
	return 0;
}

static int taiko_set_decimator_rate(struct snd_soc_dai *dai,
	u8 tx_fs_rate_reg_val, u32 sample_rate)
{
	struct snd_soc_codec *codec = dai->codec;
	struct wcd9xxx_ch *ch;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	u32 tx_port;
	u16 tx_port_reg, tx_fs_reg;
	u8 tx_port_reg_val;
	s8 decimator;

	list_for_each_entry(ch, &taiko->dai[dai->id].wcd9xxx_ch_list, list) {

		tx_port = ch->port + 1;
		pr_debug("%s: dai->id = %d, tx_port = %d",
			__func__, dai->id, tx_port);

		if ((tx_port < 1) || (tx_port > NUM_DECIMATORS)) {
			pr_debug("%s: Invalid SLIM TX%u port. DAI ID is %d\n",
				__func__, tx_port, dai->id);
			return -EINVAL;
		}

		tx_port_reg = TAIKO_A_CDC_CONN_TX_SB_B1_CTL + (tx_port - 1);
		tx_port_reg_val =  snd_soc_read(codec, tx_port_reg);

		decimator = 0;

		if ((tx_port >= 1) && (tx_port <= 6)) {

			tx_port_reg_val =  tx_port_reg_val & 0x0F;
			if (tx_port_reg_val == 0x8)
				decimator = tx_port;

		} else if ((tx_port >= 7) && (tx_port <= NUM_DECIMATORS)) {

			tx_port_reg_val =  tx_port_reg_val & 0x1F;

			if ((tx_port_reg_val >= 0x8) &&
			    (tx_port_reg_val <= 0x11)) {

				decimator = (tx_port_reg_val - 0x8) + 1;
			}
		}

		if (decimator) { /* SLIM_TX port has a DEC as input */

			tx_fs_reg = TAIKO_A_CDC_TX1_CLK_FS_CTL +
				    8 * (decimator - 1);

			pr_debug("%s: set DEC%u (-> SLIM_TX%u) rate to %u\n",
				__func__, decimator, tx_port, sample_rate);

			snd_soc_update_bits(codec, tx_fs_reg, 0x07,
					    tx_fs_rate_reg_val);

		} else {
			if ((tx_port_reg_val >= 0x1) &&
			    (tx_port_reg_val <= 0x7)) {

				pr_debug("%s: RMIX%u going to SLIM TX%u\n",
					__func__, tx_port_reg_val, tx_port);

			} else if  ((tx_port_reg_val >= 0x8) &&
				    (tx_port_reg_val <= 0x11)) {

				pr_debug("%s: ERROR: Should not be here\n",
				       __func__);
				pr_debug("%s: ERROR: DEC connected to SLIM TX%u\n",
					__func__, tx_port);
				return -EINVAL;

			} else if (tx_port_reg_val == 0) {
				pr_debug("%s: no signal to SLIM TX%u\n",
					__func__, tx_port);
			} else {
				pr_debug("%s: ERROR: wrong signal to SLIM TX%u\n",
					__func__, tx_port);
				pr_debug("%s: ERROR: wrong signal = %u\n",
					__func__, tx_port_reg_val);
				return -EINVAL;
			}
		}
	}
	return 0;
}

static void taiko_set_rxsb_port_format(struct snd_pcm_hw_params *params,
				       struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);
	struct wcd9xxx_codec_dai_data *cdc_dai;
	struct wcd9xxx_ch *ch;
	int port;
	u8 bit_sel;
	u16 sb_ctl_reg, field_shift;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		bit_sel = 0x2;
		taiko_p->dai[dai->id].bit_width = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		bit_sel = 0x0;
		taiko_p->dai[dai->id].bit_width = 24;
		break;
	default:
		dev_dbg(codec->dev, "Invalid format\n");
		return;
	}

	cdc_dai = &taiko_p->dai[dai->id];

	list_for_each_entry(ch, &cdc_dai->wcd9xxx_ch_list, list) {
		port = wcd9xxx_get_slave_port(ch->ch_num);

		if (IS_ERR_VALUE(port) ||
		    !TAIKO_VALIDATE_RX_SBPORT_RANGE(port)) {
			dev_dbg(codec->dev,
				 "%s: invalid port ID %d returned for RX DAI\n",
				 __func__, port);
			return;
		}

		port = TAIKO_CONVERT_RX_SBPORT_ID(port);

		if (port <= 3) {
			sb_ctl_reg = TAIKO_A_CDC_CONN_RX_SB_B1_CTL;
			field_shift = port << 1;
		} else if (port <= 6) {
			sb_ctl_reg = TAIKO_A_CDC_CONN_RX_SB_B2_CTL;
			field_shift = (port - 4) << 1;
		} else { /* should not happen */
			dev_dbg(codec->dev,
				 "%s: bad port ID %d\n", __func__, port);
			return;
		}

		dev_dbg(codec->dev, "%s: sb_ctl_reg %x field_shift %x\n",
			__func__, sb_ctl_reg, field_shift);
		snd_soc_update_bits(codec, sb_ctl_reg, 0x3 << field_shift,
				    bit_sel << field_shift);
	}
}

static int taiko_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(dai->codec);
	u8 tx_fs_rate, rx_fs_rate;
	u32 compander_fs;
	int ret;

	pr_debug("%s: dai_name = %s DAI-ID %x rate %d num_ch %d\n", __func__,
		 dai->name, dai->id, params_rate(params),
		 params_channels(params));

	switch (params_rate(params)) {
	case 8000:
		tx_fs_rate = 0x00;
		rx_fs_rate = 0x00;
		compander_fs = COMPANDER_FS_8KHZ;
		break;
	case 16000:
		tx_fs_rate = 0x01;
		rx_fs_rate = 0x20;
		compander_fs = COMPANDER_FS_16KHZ;
		break;
	case 32000:
		tx_fs_rate = 0x02;
		rx_fs_rate = 0x40;
		compander_fs = COMPANDER_FS_32KHZ;
		break;
	case 48000:
		tx_fs_rate = 0x03;
		rx_fs_rate = 0x60;
		compander_fs = COMPANDER_FS_48KHZ;
		break;
	case 96000:
		tx_fs_rate = 0x04;
		rx_fs_rate = 0x80;
		compander_fs = COMPANDER_FS_96KHZ;
		break;
	case 192000:
		tx_fs_rate = 0x05;
		rx_fs_rate = 0xA0;
		compander_fs = COMPANDER_FS_192KHZ;
		break;
	default:
		pr_debug("%s: Invalid sampling rate %d\n", __func__,
			params_rate(params));
		return -EINVAL;
	}

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_CAPTURE:
		if (dai->id != AIF4_VIFEED) {
			ret = taiko_set_decimator_rate(dai, tx_fs_rate,
							   params_rate(params));
			if (ret < 0) {
				pr_debug("%s: set decimator rate failed %d\n",
					__func__, ret);
				return ret;
			}
		}

		if (taiko->intf_type == WCD9XXX_INTERFACE_TYPE_I2C) {
			switch (params_format(params)) {
			case SNDRV_PCM_FORMAT_S16_LE:
				snd_soc_update_bits(codec,
					TAIKO_A_CDC_CLK_TX_I2S_CTL,
					0x20, 0x20);
				break;
			case SNDRV_PCM_FORMAT_S32_LE:
				snd_soc_update_bits(codec,
					TAIKO_A_CDC_CLK_TX_I2S_CTL,
					0x20, 0x00);
				break;
			default:
				break;
			}
			snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_TX_I2S_CTL,
					    0x07, tx_fs_rate);
		} else {
			taiko->dai[dai->id].rate = params_rate(params);
		}
		break;

	case SNDRV_PCM_STREAM_PLAYBACK:
		ret = taiko_set_interpolator_rate(dai, rx_fs_rate,
						  compander_fs);
		if (ret < 0) {
			pr_debug("%s: set decimator rate failed %d\n", __func__,
				ret);
			return ret;
		}

        pr_debug("%s: setting interpolator rate to %d\n", __func__,
				params_rate(params));
        
		if (taiko->intf_type == WCD9XXX_INTERFACE_TYPE_I2C) {
			switch (params_format(params)) {
			case SNDRV_PCM_FORMAT_S16_LE:
				snd_soc_update_bits(codec,
					TAIKO_A_CDC_CLK_RX_I2S_CTL,
					0x20, 0x20);
				break;
			case SNDRV_PCM_FORMAT_S32_LE:
				snd_soc_update_bits(codec,
					TAIKO_A_CDC_CLK_RX_I2S_CTL,
					0x20, 0x00);
				break;
			default:
				pr_debug("invalid format\n");
				break;
			}
			snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_RX_I2S_CTL,
					    0x03, (rx_fs_rate >> 0x05));
		} else {
			taiko_set_rxsb_port_format(params, dai);
			taiko->dai[dai->id].rate = params_rate(params);
		}
		break;
	default:
		pr_debug("%s: Invalid stream type %d\n", __func__,
			substream->stream);
		return -EINVAL;
	}

	return 0;
}

#if defined(CONFIG_SND_SOC_ESXXX)
int (*remote_route_enable)(struct snd_soc_dai *dai) = REMOTE_ROUTE_ENABLE_CB;
int (*slim_get_channel_map)(struct snd_soc_dai *dai,
		unsigned int *tx_num, unsigned int *tx_slot,
		unsigned int *rx_num, unsigned int *rx_slot)
			= SLIM_GET_CHANNEL_MAP_CB;
int (*slim_set_channel_map)(struct snd_soc_dai *dai,
		unsigned int tx_num, unsigned int *tx_slot,
		unsigned int rx_num, unsigned int *rx_slot)
			= SLIM_SET_CHANNEL_MAP_CB;
int (*slim_hw_params)(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *dai)
		= SLIM_HW_PARAMS_CB;
int (*remote_cfg_slim_rx)(int dai_id)	=	REMOTE_CFG_SLIM_RX_CB;
int (*remote_close_slim_rx)(int dai_id)	=	REMOTE_CLOSE_SLIM_RX_CB;
int (*remote_cfg_slim_tx)(int dai_id)	=	REMOTE_CFG_SLIM_TX_CB;
int (*remote_close_slim_tx)(int dai_id)	=	REMOTE_CLOSE_SLIM_TX_CB;
int (*remote_add_codec_controls)(struct snd_soc_codec *codec)
		= REMOTE_ADD_CODEC_CONTROLS_CB;

static int taiko_esxxx_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	taiko_startup(substream, dai);
/*
	if (es705_remote_route_enable(dai))
		es705_slim_startup(substream, dai);
*/

	return 0;
}

static void taiko_esxxx_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	taiko_shutdown(substream, dai);

/*
	if (es705_remote_route_enable(dai))
		es705_slim_shutdown(substream, dai);
*/
}

static int taiko_esxxx_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *dai)
{
	int rc = 0;
	pr_debug("%s: dai_name = %s DAI-ID %x rate %d num_ch %d\n", __func__,
			dai->name, dai->id, params_rate(params),
			params_channels(params));

	rc = taiko_hw_params(substream, params, dai);

	if (remote_route_enable(dai))
		rc = slim_hw_params(substream, params, dai);

	return rc;
}
static int taiko_esxxx_set_channel_map(struct snd_soc_dai *dai,
				unsigned int tx_num, unsigned int *tx_slot,
				unsigned int rx_num, unsigned int *rx_slot)

{
	unsigned int taiko_tx_num = 0;
	unsigned int taiko_tx_slot[6];
	unsigned int taiko_rx_num = 0;
	unsigned int taiko_rx_slot[6];
	int rc = 0;
	pr_debug("%s(): dai_name = %s DAI-ID %x tx_ch %d rx_ch %d\n",
			__func__, dai->name, dai->id, tx_num, rx_num);

	if (remote_route_enable(dai)) {
		rc = taiko_get_channel_map(dai, &taiko_tx_num, taiko_tx_slot,
					&taiko_rx_num, taiko_rx_slot);

		rc = taiko_set_channel_map(dai, tx_num, taiko_tx_slot, rx_num, taiko_rx_slot);

		rc = slim_set_channel_map(dai, tx_num, tx_slot, rx_num,
					rx_slot);
	} else
		rc = taiko_set_channel_map(dai, tx_num, tx_slot, rx_num, rx_slot);

	return rc;
}

static int taiko_esxxx_get_channel_map(struct snd_soc_dai *dai,
				unsigned int *tx_num, unsigned int *tx_slot,
				unsigned int *rx_num, unsigned int *rx_slot)

{
	int rc = 0;

	pr_debug("%s(): dai_name = %s DAI-ID %d tx_ch %d rx_ch %d\n",
			__func__, dai->name, dai->id, *tx_num, *rx_num);

	if (remote_route_enable(dai))
		rc = slim_get_channel_map(dai, tx_num, tx_slot, rx_num,
					rx_slot);
	else
		rc = taiko_get_channel_map(dai, tx_num, tx_slot, rx_num, rx_slot);

	return rc;
}
static struct snd_soc_dai_ops taiko_dai_ops = {
	.startup = taiko_esxxx_startup, /* taiko_startup, */
	.shutdown = taiko_esxxx_shutdown, /* taiko_shutdown, */
	.prepare = taiko_prepare,
	.hw_params = taiko_esxxx_hw_params, /* taiko_hw_params, */
	.set_sysclk = taiko_set_dai_sysclk,
	.set_fmt = taiko_set_dai_fmt,
	.set_channel_map = taiko_esxxx_set_channel_map,
			/* taiko_set_channel_map, */
	.get_channel_map = taiko_esxxx_get_channel_map,
			/* taiko_get_channel_map, */
};
#elif defined(CONFIG_SND_SOC_ES325)
static int taiko_es325_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *dai)
{
	int rc = 0;
	dev_dbg(dai->dev,"%s: dai_name = %s DAI-ID %x rate %d num_ch %d\n", __func__,
			dai->name, dai->id, params_rate(params),
			params_channels(params));

	rc = taiko_hw_params(substream, params, dai);

	if (es325_remote_route_enable(dai))
		rc = es325_slim_hw_params(substream, params, dai);

	return rc;
}

#define SLIM_BUGFIX
static int taiko_es325_set_channel_map(struct snd_soc_dai *dai,
				unsigned int tx_num, unsigned int *tx_slot,
				unsigned int rx_num, unsigned int *rx_slot)

{
#if !defined(SLIM_BUGFIX)
	unsigned int taiko_tx_num = 0;
#endif
	unsigned int taiko_tx_slot[6];
#if !defined(SLIM_BUGFIX)
	unsigned int taiko_rx_num = 0;
#endif
	unsigned int taiko_rx_slot[6];
#if defined(SLIM_BUGFIX)
	unsigned int temp_tx_num = 0;
	unsigned int temp_rx_num = 0;
#endif
	int rc = 0;

	if (es325_remote_route_enable(dai)) {
#if defined(SLIM_BUGFIX)
		rc = taiko_get_channel_map(dai, &temp_tx_num, taiko_tx_slot,
					&temp_rx_num, taiko_rx_slot);
#else
		rc = taiko_get_channel_map(dai, &taiko_tx_num, taiko_tx_slot,
					&taiko_rx_num, taiko_rx_slot);
#endif

		rc = taiko_set_channel_map(dai, tx_num, taiko_tx_slot, rx_num, taiko_rx_slot);

		rc = es325_slim_set_channel_map(dai, tx_num, tx_slot, rx_num, rx_slot);
	} else
		rc = taiko_set_channel_map(dai, tx_num, tx_slot, rx_num, rx_slot);

	return rc;
}

static int taiko_es325_get_channel_map(struct snd_soc_dai *dai,
				unsigned int *tx_num, unsigned int *tx_slot,
				unsigned int *rx_num, unsigned int *rx_slot)

{
	int rc = 0;

	if (es325_remote_route_enable(dai))
		rc = es325_slim_get_channel_map(dai, tx_num, tx_slot, rx_num, rx_slot);
	else
		rc = taiko_get_channel_map(dai, tx_num, tx_slot, rx_num, rx_slot);

	return rc;
}

static struct snd_soc_dai_ops taiko_dai_ops = {
	.startup = taiko_startup,
	.shutdown = taiko_shutdown,
	.prepare = taiko_prepare,
	.hw_params = taiko_es325_hw_params, /* taiko_hw_params, */
	.set_sysclk = taiko_set_dai_sysclk,
	.set_fmt = taiko_set_dai_fmt,
	.set_channel_map = taiko_set_channel_map, /* taiko_set_channel_map, */
	.get_channel_map = taiko_es325_get_channel_map, /* taiko_get_channel_map, */
};

static struct snd_soc_dai_ops taiko_es325_dai_ops = {
	.startup = taiko_startup,
	.hw_params = taiko_es325_hw_params,
	.set_channel_map = taiko_es325_set_channel_map,
	.get_channel_map = taiko_es325_get_channel_map,
};
#else
static struct snd_soc_dai_ops taiko_dai_ops = {
	.startup = taiko_startup,
	.shutdown = taiko_shutdown,
	.prepare = taiko_prepare,
	.hw_params = taiko_hw_params,
	.set_sysclk = taiko_set_dai_sysclk,
	.set_fmt = taiko_set_dai_fmt,
	.set_channel_map = taiko_set_channel_map,
	.get_channel_map = taiko_get_channel_map,
};
#endif

static struct snd_soc_dai_driver taiko_dai[] = {
	{
		.name = "taiko_rx1",
		.id = AIF1_PB,
		.playback = {
			.stream_name = "AIF1 Playback",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS_S16_S24_LE,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_tx1",
		.id = AIF1_CAP,
		.capture = {
			.stream_name = "AIF1 Capture",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 4,
		},
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_rx2",
		.id = AIF2_PB,
		.playback = {
			.stream_name = "AIF2 Playback",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS_S16_S24_LE,
			.rate_min = 8000,
			.rate_max = 192000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_tx2",
		.id = AIF2_CAP,
		.capture = {
			.stream_name = "AIF2 Capture",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 8,
		},
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_rx3",
		.id = AIF3_PB,
		.playback = {
			.stream_name = "AIF3 Playback",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS_S16_S24_LE,
			.rate_min = 8000,
			.rate_max = 192000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_tx3",
		.id = AIF3_CAP,
		.capture = {
			.stream_name = "AIF3 Capture",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 48000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_vifeedback",
		.id = AIF4_VIFEED,
		.capture = {
			.stream_name = "VIfeed",
			.rates = SNDRV_PCM_RATE_48000,
			.formats = TAIKO_FORMATS,
			.rate_max = 48000,
			.rate_min = 48000,
			.channels_min = 2,
			.channels_max = 2,
	 },
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_mad1",
		.id = AIF4_MAD_TX,
		.capture = {
			.stream_name = "AIF4 MAD TX",
			.rates = SNDRV_PCM_RATE_16000,
			.formats = TAIKO_FORMATS,
			.rate_min = 16000,
			.rate_max = 16000,
			.channels_min = 1,
			.channels_max = 1,
		},
		.ops = &taiko_dai_ops,
	},
#ifdef CONFIG_SND_SOC_ES325
	{
		.name = "taiko_es325_rx1",
		.id = AIF1_PB + ES325_DAI_ID_OFFSET,
		.playback = {
			.stream_name = "AIF1 Playback",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &taiko_es325_dai_ops,
	},
	{
		.name = "taiko_es325_tx1",
		.id = AIF1_CAP + ES325_DAI_ID_OFFSET,
		.capture = {
			.stream_name = "AIF1 Capture",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &taiko_es325_dai_ops,
	},
	{
		.name = "taiko_es325_rx2",
		.id = AIF2_PB + ES325_DAI_ID_OFFSET,
		.playback = {
			.stream_name = "AIF2 Playback",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &taiko_es325_dai_ops,
	},
#endif
};

static struct snd_soc_dai_driver taiko_i2s_dai[] = {
	{
		.name = "taiko_i2s_rx1",
		.id = AIF1_PB,
		.playback = {
			.stream_name = "AIF1 Playback",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 4,
		},
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_i2s_tx1",
		.id = AIF1_CAP,
		.capture = {
			.stream_name = "AIF1 Capture",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 4,
		},
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_i2s_rx2",
		.id = AIF1_PB,
		.playback = {
			.stream_name = "AIF2 Playback",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 4,
		},
		.ops = &taiko_dai_ops,
	},
	{
		.name = "taiko_i2s_tx2",
		.id = AIF1_CAP,
		.capture = {
			.stream_name = "AIF2 Capture",
			.rates = WCD9320_RATES,
			.formats = TAIKO_FORMATS,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 4,
		},
		.ops = &taiko_dai_ops,
	},
};

static int taiko_codec_enable_slim_chmask(struct wcd9xxx_codec_dai_data *dai,
					  bool up)
{
	int ret = 0;
	struct wcd9xxx_ch *ch;

	if (up) {
		list_for_each_entry(ch, &dai->wcd9xxx_ch_list, list) {
			ret = wcd9xxx_get_slave_port(ch->ch_num);
			if (ret < 0) {
				pr_debug("%s: Invalid slave port ID: %d\n",
				       __func__, ret);
				ret = -EINVAL;
			} else {
				set_bit(ret, &dai->ch_mask);
			}
		}
	} else {
		ret = wait_event_timeout(dai->dai_wait, (dai->ch_mask == 0),
					 msecs_to_jiffies(
						     TAIKO_SLIM_CLOSE_TIMEOUT));
		if (!ret) {
			pr_debug("%s: Slim close tx/rx wait timeout\n", __func__);
			ret = -ETIMEDOUT;
		} else {
			ret = 0;
		}
	}
	return ret;
}

static void taiko_codec_enable_int_port(struct wcd9xxx_codec_dai_data *dai,
					  struct snd_soc_codec *codec)
{
	struct wcd9xxx_ch *ch;
	int port_num = 0;
	unsigned short reg = 0;
	u8 val = 0;
	if (!dai || !codec) {
		pr_debug("%s: Invalid params\n", __func__);
		return;
	}
	list_for_each_entry(ch, &dai->wcd9xxx_ch_list, list) {
		if (ch->port >= TAIKO_RX_PORT_START_NUMBER) {
			port_num = ch->port - TAIKO_RX_PORT_START_NUMBER;
			reg = TAIKO_SLIM_PGD_PORT_INT_EN0 + (port_num / 8);
			val = wcd9xxx_interface_reg_read(codec->control_data,
				reg);
			if (!(val & (1 << (port_num % 8)))) {
				val |= (1 << (port_num % 8));
				wcd9xxx_interface_reg_write(
					codec->control_data, reg, val);
				val = wcd9xxx_interface_reg_read(
					codec->control_data, reg);
			}
		} else {
			port_num = ch->port;
			reg = TAIKO_SLIM_PGD_PORT_INT_TX_EN0 + (port_num / 8);
			val = wcd9xxx_interface_reg_read(codec->control_data,
				reg);
			if (!(val & (1 << (port_num % 8)))) {
				val |= (1 << (port_num % 8));
				wcd9xxx_interface_reg_write(codec->control_data,
					reg, val);
				val = wcd9xxx_interface_reg_read(
					codec->control_data, reg);
			}
		}
	}
}

static int taiko_codec_enable_slimrx(struct snd_soc_dapm_widget *w,
				     struct snd_kcontrol *kcontrol,
				     int event)
{
	struct wcd9xxx *core;
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);
	int ret = 0;
	struct wcd9xxx_codec_dai_data *dai;

	core = dev_get_drvdata(codec->dev->parent);

	pr_debug("%s: event called! codec name %s num_dai %d\n"
		"stream name %s event %d\n",
		__func__, w->codec->name, w->codec->num_dai, w->sname, event);

	/* Execute the callback only if interface type is slimbus */
	if (taiko_p->intf_type != WCD9XXX_INTERFACE_TYPE_SLIMBUS)
		return 0;

	dai = &taiko_p->dai[w->shift];
	pr_debug("%s: w->name %s w->shift %d event %d\n",
		 __func__, w->name, w->shift, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		dai->bus_down_in_recovery = false;
		taiko_codec_enable_int_port(dai, codec);
		(void) taiko_codec_enable_slim_chmask(dai, true);
#if defined(CONFIG_SND_SOC_ESXXX)
		ret = remote_cfg_slim_rx(w->shift);
#elif defined(CONFIG_SND_SOC_ES325)
		ret = es325_remote_cfg_slim_rx(w->shift);
#endif
		ret = wcd9xxx_cfg_slim_sch_rx(core, &dai->wcd9xxx_ch_list,
					      dai->rate, dai->bit_width,
					      &dai->grph);
		break;
	case SND_SOC_DAPM_POST_PMD:
#if defined(CONFIG_SND_SOC_ESXXX)
		ret = remote_close_slim_rx(w->shift);
#elif defined(CONFIG_SND_SOC_ES325)
		ret = es325_remote_close_slim_rx(w->shift);
#endif
		ret = wcd9xxx_close_slim_sch_rx(core, &dai->wcd9xxx_ch_list,
						dai->grph);
		if (!dai->bus_down_in_recovery)
			ret = taiko_codec_enable_slim_chmask(dai, false);

		if (ret < 0) {
			ret = wcd9xxx_disconnect_port(core,
						      &dai->wcd9xxx_ch_list,
						      dai->grph);
			pr_debug("%s: Disconnect RX port, ret = %d\n",
				 __func__, ret);
		}
		dai->bus_down_in_recovery = false;
		break;
	}
	return ret;
}

static int taiko_codec_enable_slimvi_feedback(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol,
				int event)
{
	struct wcd9xxx *core = NULL;
	struct snd_soc_codec *codec = NULL;
	struct taiko_priv *taiko_p = NULL;
	u32 ret = 0;
	struct wcd9xxx_codec_dai_data *dai = NULL;

	if (!w || !w->codec) {
		pr_debug("%s invalid params\n", __func__);
		return -EINVAL;
	}
	codec = w->codec;
	taiko_p = snd_soc_codec_get_drvdata(codec);
	core = dev_get_drvdata(codec->dev->parent);

	pr_debug("%s: event called! codec name %s num_dai %d stream name %s\n",
		__func__, w->codec->name, w->codec->num_dai, w->sname);

	/* Execute the callback only if interface type is slimbus */
	if (taiko_p->intf_type != WCD9XXX_INTERFACE_TYPE_SLIMBUS) {
		pr_debug("%s Interface is not correct", __func__);
		return 0;
	}

	pr_debug("%s(): w->name %s event %d w->shift %d\n",
		__func__, w->name, event, w->shift);
	if (w->shift != AIF4_VIFEED) {
		pr_debug("%s Error in enabling the tx path\n", __func__);
		ret = -EINVAL;
		goto out_vi;
	}
	dai = &taiko_p->dai[w->shift];
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/*Enable V&I sensing*/
		snd_soc_update_bits(codec, TAIKO_A_SPKR_PROT_EN,
				0x88, 0x88);
		/*Enable spkr VI clocks*/
		snd_soc_update_bits(codec,
		TAIKO_A_CDC_CLK_TX_CLK_EN_B2_CTL, 0xC, 0xC);
		taiko_codec_enable_int_port(dai, codec);
		dai->bus_down_in_recovery = false;
		(void) taiko_codec_enable_slim_chmask(dai, true);
		ret = wcd9xxx_cfg_slim_sch_tx(core, &dai->wcd9xxx_ch_list,
					dai->rate, dai->bit_width,
					&dai->grph);
		break;
	case SND_SOC_DAPM_POST_PMD:
		ret = wcd9xxx_close_slim_sch_tx(core, &dai->wcd9xxx_ch_list,
						dai->grph);
		if (ret)
			pr_debug("%s error in close_slim_sch_tx %d\n",
				__func__, ret);
		ret = taiko_codec_enable_slim_chmask(dai, false);
		if (ret < 0) {
			ret = wcd9xxx_disconnect_port(core,
						      &dai->wcd9xxx_ch_list,
						      dai->grph);
			pr_debug("%s: Disconnect RX port, ret = %d\n",
				 __func__, ret);
		}

		snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_TX_CLK_EN_B2_CTL,
				0xC, 0x0);
		/*Disable V&I sensing*/
		snd_soc_update_bits(codec, TAIKO_A_SPKR_PROT_EN,
				0x88, 0x00);

		dai->bus_down_in_recovery = false;
		break;
	}
out_vi:
	return ret;
}

static int taiko_codec_enable_slimtx(struct snd_soc_dapm_widget *w,
				     struct snd_kcontrol *kcontrol,
				     int event)
{
	struct wcd9xxx *core;
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);
	u32  ret = 0;
	struct wcd9xxx_codec_dai_data *dai;

	core = dev_get_drvdata(codec->dev->parent);

	pr_debug("%s: event called! codec name %s num_dai %d stream name %s\n",
		__func__, w->codec->name, w->codec->num_dai, w->sname);

	/* Execute the callback only if interface type is slimbus */
	if (taiko_p->intf_type != WCD9XXX_INTERFACE_TYPE_SLIMBUS)
		return 0;

	pr_debug("%s(): w->name %s event %d w->shift %d\n",
		__func__, w->name, event, w->shift);

	dai = &taiko_p->dai[w->shift];

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		taiko_codec_enable_int_port(dai, codec);
		dai->bus_down_in_recovery = false;
		(void) taiko_codec_enable_slim_chmask(dai, true);
		ret = wcd9xxx_cfg_slim_sch_tx(core, &dai->wcd9xxx_ch_list,
					      dai->rate, dai->bit_width,
					      &dai->grph);
#if defined(CONFIG_SND_SOC_ESXXX)
			ret = remote_cfg_slim_tx(w->shift);
#elif defined(CONFIG_SND_SOC_ES325)
		ret = es325_remote_cfg_slim_tx(w->shift);
#endif
		break;
	case SND_SOC_DAPM_POST_PMD:
#if defined(CONFIG_SND_SOC_ESXXX)
		ret = remote_close_slim_tx(w->shift);
#elif defined(CONFIG_SND_SOC_ES325)
		ret = es325_remote_close_slim_tx(w->shift);
#endif
		ret = wcd9xxx_close_slim_sch_tx(core, &dai->wcd9xxx_ch_list,
						dai->grph);
		if (!dai->bus_down_in_recovery)
			ret = taiko_codec_enable_slim_chmask(dai, false);

		if (ret < 0) {
			ret = wcd9xxx_disconnect_port(core,
						      &dai->wcd9xxx_ch_list,
						      dai->grph);
			pr_debug("%s: Disconnect RX port, ret = %d\n",
				 __func__, ret);
		}

		dai->bus_down_in_recovery = false;
		break;
	}
	return ret;
}

static int taiko_codec_enable_ear_pa(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s %s %d\n", __func__, w->name, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		wcd9xxx_clsh_fsm(codec, &taiko_p->clsh_d,
						 WCD9XXX_CLSH_STATE_EAR,
						 WCD9XXX_CLSH_REQ_ENABLE,
						 WCD9XXX_CLSH_EVENT_POST_PA);

		usleep_range(5000, 5000);
		break;
	case SND_SOC_DAPM_POST_PMD:
		wcd9xxx_clsh_fsm(codec, &taiko_p->clsh_d,
						 WCD9XXX_CLSH_STATE_EAR,
						 WCD9XXX_CLSH_REQ_DISABLE,
						 WCD9XXX_CLSH_EVENT_POST_PA);
		usleep_range(5000, 5000);
	}
	return 0;
}

static int taiko_codec_ear_dac_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct taiko_priv *taiko_p = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s %s %d\n", __func__, w->name, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		wcd9xxx_clsh_fsm(codec, &taiko_p->clsh_d,
						 WCD9XXX_CLSH_STATE_EAR,
						 WCD9XXX_CLSH_REQ_ENABLE,
						 WCD9XXX_CLSH_EVENT_PRE_DAC);
		break;
	}

	return 0;
}

static int taiko_codec_dsm_mux_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	u8 reg_val, zoh_mux_val = 0x00;

	pr_debug("%s: event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		reg_val = snd_soc_read(codec, TAIKO_A_CDC_CONN_CLSH_CTL);
		if ((reg_val & 0x30) == 0x10)
			zoh_mux_val = 0x04;
		else if ((reg_val & 0x30) == 0x20)
			zoh_mux_val = 0x08;

		if (zoh_mux_val != 0x00)
			snd_soc_update_bits(codec,
					TAIKO_A_CDC_CONN_CLSH_CTL,
					0x0C, zoh_mux_val);
		write_hph_poweramp_regs();
		break;
	case SND_SOC_DAPM_POST_PMD:
		write_hph_poweramp_regs();
		snd_soc_update_bits(codec, TAIKO_A_CDC_CONN_CLSH_CTL,
							0x0C, 0x00);
		break;
	}
	return 0;
}

static int taiko_codec_enable_anc_ear(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = taiko_codec_enable_anc(w, kcontrol, event);
		msleep(50);
		snd_soc_update_bits(codec, TAIKO_A_RX_EAR_EN, 0x10, 0x10);
		break;
	case SND_SOC_DAPM_POST_PMU:
		ret = taiko_codec_enable_ear_pa(w, kcontrol, event);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_update_bits(codec, TAIKO_A_RX_EAR_EN, 0x10, 0x00);
		msleep(40);
		ret |= taiko_codec_enable_anc(w, kcontrol, event);
		break;
	case SND_SOC_DAPM_POST_PMD:
		ret = taiko_codec_enable_ear_pa(w, kcontrol, event);
		break;
	}
	return ret;
}
#if 0
static int taiko_codec_set_iir_gain(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	int value = 0;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		value = snd_soc_read(codec, TAIKO_A_CDC_IIR1_GAIN_B1_CTL);
		snd_soc_write(codec, TAIKO_A_CDC_IIR1_GAIN_B1_CTL, value);
		break;
	default:
		pr_debug("%s: event = %d not expected\n", __func__, event);
		break;
	}
	return 0;
}
#endif

/* Todo: Have seperate dapm widgets for I2S and Slimbus.
 * Might Need to have callbacks registered only for slimbus
 */
static const struct snd_soc_dapm_widget taiko_dapm_widgets[] = {
	/*RX stuff */
	SND_SOC_DAPM_OUTPUT("EAR"),

	SND_SOC_DAPM_PGA_E("EAR PA", TAIKO_A_RX_EAR_EN, 4, 0, NULL, 0,
			taiko_codec_enable_ear_pa, SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MIXER_E("DAC1", TAIKO_A_RX_EAR_EN, 6, 0, dac1_switch,
		ARRAY_SIZE(dac1_switch), taiko_codec_ear_dac_event,
		SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_AIF_IN_E("AIF1 PB", "AIF1 Playback", 0, SND_SOC_NOPM,
				AIF1_PB, 0, taiko_codec_enable_slimrx,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_IN_E("AIF2 PB", "AIF2 Playback", 0, SND_SOC_NOPM,
				AIF2_PB, 0, taiko_codec_enable_slimrx,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_IN_E("AIF3 PB", "AIF3 Playback", 0, SND_SOC_NOPM,
				AIF3_PB, 0, taiko_codec_enable_slimrx,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX("SLIM RX1 MUX", SND_SOC_NOPM, TAIKO_RX1, 0,
				&slim_rx_mux[TAIKO_RX1]),
	SND_SOC_DAPM_MUX("SLIM RX2 MUX", SND_SOC_NOPM, TAIKO_RX2, 0,
				&slim_rx_mux[TAIKO_RX2]),
	SND_SOC_DAPM_MUX("SLIM RX3 MUX", SND_SOC_NOPM, TAIKO_RX3, 0,
				&slim_rx_mux[TAIKO_RX3]),
	SND_SOC_DAPM_MUX("SLIM RX4 MUX", SND_SOC_NOPM, TAIKO_RX4, 0,
				&slim_rx_mux[TAIKO_RX4]),
	SND_SOC_DAPM_MUX("SLIM RX5 MUX", SND_SOC_NOPM, TAIKO_RX5, 0,
				&slim_rx_mux[TAIKO_RX5]),
	SND_SOC_DAPM_MUX("SLIM RX6 MUX", SND_SOC_NOPM, TAIKO_RX6, 0,
				&slim_rx_mux[TAIKO_RX6]),
	SND_SOC_DAPM_MUX("SLIM RX7 MUX", SND_SOC_NOPM, TAIKO_RX7, 0,
				&slim_rx_mux[TAIKO_RX7]),

	SND_SOC_DAPM_MIXER("SLIM RX1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX4", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX5", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX6", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX7", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* Headphone */
	SND_SOC_DAPM_OUTPUT("HEADPHONE"),
	SND_SOC_DAPM_PGA_E("HPHL", TAIKO_A_RX_HPH_CNP_EN, 5, 0, NULL, 0,
		taiko_hph_pa_event,
        SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
        SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MIXER_E("HPHL DAC", TAIKO_A_RX_HPH_L_DAC_CTL, 7, 0,
		hphl_switch, ARRAY_SIZE(hphl_switch), taiko_hphl_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_PGA_E("HPHR", TAIKO_A_RX_HPH_CNP_EN, 4, 0, NULL, 0,
		taiko_hph_pa_event,
        SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
        SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("HPHR DAC", NULL, TAIKO_A_RX_HPH_R_DAC_CTL, 7, 0,
		taiko_hphr_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	/* Speaker */
	SND_SOC_DAPM_OUTPUT("LINEOUT1"),
	SND_SOC_DAPM_OUTPUT("LINEOUT2"),
	SND_SOC_DAPM_OUTPUT("LINEOUT3"),
	SND_SOC_DAPM_OUTPUT("LINEOUT4"),
	SND_SOC_DAPM_OUTPUT("SPK_OUT"),

	SND_SOC_DAPM_PGA_E("LINEOUT1 PA", TAIKO_A_RX_LINE_CNP_EN, 0, 0, NULL,
			0, taiko_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("LINEOUT2 PA", TAIKO_A_RX_LINE_CNP_EN, 1, 0, NULL,
			0, taiko_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("LINEOUT3 PA", TAIKO_A_RX_LINE_CNP_EN, 2, 0, NULL,
			0, taiko_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("LINEOUT4 PA", TAIKO_A_RX_LINE_CNP_EN, 3, 0, NULL,
			0, taiko_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("SPK PA", SND_SOC_NOPM, 0, 0 , NULL,
			   0, taiko_codec_enable_spk_pa,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_DAC_E("LINEOUT1 DAC", NULL, TAIKO_A_RX_LINE_1_DAC_CTL, 7, 0
		, taiko_lineout_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("LINEOUT2 DAC", NULL, TAIKO_A_RX_LINE_2_DAC_CTL, 7, 0
		, taiko_lineout_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("LINEOUT3 DAC", NULL, TAIKO_A_RX_LINE_3_DAC_CTL, 7, 0
		, taiko_lineout_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SWITCH("LINEOUT3 DAC GROUND", SND_SOC_NOPM, 0, 0,
				&lineout3_ground_switch),
	SND_SOC_DAPM_DAC_E("LINEOUT4 DAC", NULL, TAIKO_A_RX_LINE_4_DAC_CTL, 7, 0
		, taiko_lineout_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SWITCH("LINEOUT4 DAC GROUND", SND_SOC_NOPM, 0, 0,
				&lineout4_ground_switch),

	SND_SOC_DAPM_DAC_E("SPK DAC", NULL, SND_SOC_NOPM, 0, 0,
			   taiko_spk_dac_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("VDD_SPKDRV", SND_SOC_NOPM, 0, 0,
			    taiko_codec_enable_vdd_spkr,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MIXER("RX1 MIX1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("RX2 MIX1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("RX7 MIX1", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_MIXER("RX1 MIX2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("RX2 MIX2", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_MIXER_E("RX3 MIX1", TAIKO_A_CDC_CLK_RX_B1_CTL, 2, 0, NULL,
		0, taiko_codec_enable_interpolator, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MIXER_E("RX4 MIX1", TAIKO_A_CDC_CLK_RX_B1_CTL, 3, 0, NULL,
		0, taiko_codec_enable_interpolator, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MIXER_E("RX5 MIX1", TAIKO_A_CDC_CLK_RX_B1_CTL, 4, 0, NULL,
		0, taiko_codec_enable_interpolator, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MIXER_E("RX6 MIX1", TAIKO_A_CDC_CLK_RX_B1_CTL, 5, 0, NULL,
		0, taiko_codec_enable_interpolator, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MIXER_E("RX7 MIX2", TAIKO_A_CDC_CLK_RX_B1_CTL, 6, 0, NULL,
		0, taiko_codec_enable_interpolator, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_MUX_E("RX1 INTERP", TAIKO_A_CDC_CLK_RX_B1_CTL, 0, 0,
		&rx1_interpolator, taiko_codec_enable_interpolator,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MUX_E("RX2 INTERP", TAIKO_A_CDC_CLK_RX_B1_CTL, 1, 0,
		&rx2_interpolator, taiko_codec_enable_interpolator,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_MIXER("RX1 CHAIN", TAIKO_A_CDC_RX1_B6_CTL, 5, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("RX2 CHAIN", TAIKO_A_CDC_RX2_B6_CTL, 5, 0, NULL, 0),

	SND_SOC_DAPM_MUX("RX1 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX1 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX1 MIX1 INP3", SND_SOC_NOPM, 0, 0,
		&rx_mix1_inp3_mux),
	SND_SOC_DAPM_MUX("RX2 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx2_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX2 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx2_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX2 MIX1 INP3", SND_SOC_NOPM, 0, 0,
		&rx2_mix1_inp3_mux),
	SND_SOC_DAPM_MUX("RX3 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx3_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX3 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx3_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX4 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx4_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX4 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx4_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX5 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx5_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX5 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx5_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX6 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx6_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX6 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx6_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX7 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx7_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX7 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx7_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX1 MIX2 INP1", SND_SOC_NOPM, 0, 0,
		&rx1_mix2_inp1_mux),
	SND_SOC_DAPM_MUX("RX1 MIX2 INP2", SND_SOC_NOPM, 0, 0,
		&rx1_mix2_inp2_mux),
	SND_SOC_DAPM_MUX("RX2 MIX2 INP1", SND_SOC_NOPM, 0, 0,
		&rx2_mix2_inp1_mux),
	SND_SOC_DAPM_MUX("RX2 MIX2 INP2", SND_SOC_NOPM, 0, 0,
		&rx2_mix2_inp2_mux),
	SND_SOC_DAPM_MUX("RX7 MIX2 INP1", SND_SOC_NOPM, 0, 0,
		&rx7_mix2_inp1_mux),
	SND_SOC_DAPM_MUX("RX7 MIX2 INP2", SND_SOC_NOPM, 0, 0,
		&rx7_mix2_inp2_mux),

	SND_SOC_DAPM_MUX("RDAC5 MUX", SND_SOC_NOPM, 0, 0,
		&rx_dac5_mux),
	SND_SOC_DAPM_MUX("RDAC7 MUX", SND_SOC_NOPM, 0, 0,
		&rx_dac7_mux),

	SND_SOC_DAPM_MUX_E("CLASS_H_DSM MUX", SND_SOC_NOPM, 0, 0,
		&class_h_dsm_mux, taiko_codec_dsm_mux_event,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("RX_BIAS", SND_SOC_NOPM, 0, 0,
		taiko_codec_enable_rx_bias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("CDC_I2S_RX_CONN", WCD9XXX_A_CDC_CLK_OTHR_CTL, 5, 0,
			    NULL, 0),

	/* TX */

	SND_SOC_DAPM_SUPPLY("CDC_CONN", WCD9XXX_A_CDC_CLK_OTHR_CTL, 2, 0, NULL,
		0),

	SND_SOC_DAPM_SUPPLY("LDO_H", SND_SOC_NOPM, 7, 0,
			    taiko_codec_enable_ldo_h,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	/*
	 * DAPM 'LDO_H Standalone' is to be powered by mbhc driver after
	 * acquring codec_resource lock.
	 * So call __taiko_codec_enable_ldo_h instead and avoid deadlock.
	 */
	SND_SOC_DAPM_SUPPLY("LDO_H Standalone", SND_SOC_NOPM, 7, 0,
			    __taiko_codec_enable_ldo_h,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("COMP0_CLK", SND_SOC_NOPM, 0, 0,
		taiko_config_compander, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SUPPLY("COMP1_CLK", SND_SOC_NOPM, 1, 0,
		taiko_config_compander, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SUPPLY("COMP2_CLK", SND_SOC_NOPM, 2, 0,
		taiko_config_compander, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_PRE_PMD),


	SND_SOC_DAPM_INPUT("AMIC1"),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS1 External", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS1 Internal1", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS1 Internal2", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("Main Mic Bias", 0, 0, 0,
			       0, SND_SOC_DAPM_PRE_PMU |SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_INPUT("AMIC3"),

	SND_SOC_DAPM_INPUT("AMIC4"),
#if defined(CONFIG_LDO_SUBMIC_BIAS)
	SND_SOC_DAPM_MICBIAS_E("Sub Mic Bias", 0, 0, 0,
				0, SND_SOC_DAPM_PRE_PMU |SND_SOC_DAPM_POST_PMU |
				SND_SOC_DAPM_POST_PMD),
#endif

	SND_SOC_DAPM_INPUT("AMIC5"),

	SND_SOC_DAPM_INPUT("AMIC6"),

	SND_SOC_DAPM_MUX_E("DEC1 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B1_CTL, 0, 0,
		&dec1_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC2 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B1_CTL, 1, 0,
		&dec2_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC3 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B1_CTL, 2, 0,
		&dec3_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC4 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B1_CTL, 3, 0,
		&dec4_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC5 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B1_CTL, 4, 0,
		&dec5_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC6 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B1_CTL, 5, 0,
		&dec6_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC7 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B1_CTL, 6, 0,
		&dec7_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC8 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B1_CTL, 7, 0,
		&dec8_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC9 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B2_CTL, 0, 0,
		&dec9_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC10 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B2_CTL, 1, 0,
		&dec10_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX("ANC1 MUX", SND_SOC_NOPM, 0, 0, &anc1_mux),
	SND_SOC_DAPM_MUX("ANC2 MUX", SND_SOC_NOPM, 0, 0, &anc2_mux),

	SND_SOC_DAPM_OUTPUT("ANC HEADPHONE"),
	SND_SOC_DAPM_PGA_E("ANC HPHL", SND_SOC_NOPM, 5, 0, NULL, 0,
		taiko_codec_enable_anc_hph,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD |
		SND_SOC_DAPM_POST_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_E("ANC HPHR", SND_SOC_NOPM, 4, 0, NULL, 0,
		taiko_codec_enable_anc_hph, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_OUTPUT("ANC EAR"),
	SND_SOC_DAPM_PGA_E("ANC EAR PA", SND_SOC_NOPM, 0, 0, NULL, 0,
		taiko_codec_enable_anc_ear,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MUX("ANC1 FB MUX", SND_SOC_NOPM, 0, 0, &anc1_fb_mux),

	SND_SOC_DAPM_INPUT("AMIC2"),
	SND_SOC_DAPM_MICBIAS_E(DAPM_MICBIAS2_EXTERNAL_STANDALONE, SND_SOC_NOPM,
			       7, 0, taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 External", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 Internal1", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU |
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 Internal2", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 Internal3", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E(DAPM_MICBIAS3_EXTERNAL_STANDALONE, SND_SOC_NOPM,
			       7, 0, taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS3 External", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS3 Internal1", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU |
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS3 Internal2", SND_SOC_NOPM, 7, 0,
			       taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU |
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS4 External", SND_SOC_NOPM, 7,
			       0, taiko_codec_enable_micbias,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			       SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("Ear Mic Bias", 0, 0, 0,
				0, SND_SOC_DAPM_PRE_PMU |SND_SOC_DAPM_POST_PMU |
				SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_AIF_OUT_E("AIF1 CAP", "AIF1 Capture", 0, SND_SOC_NOPM,
		AIF1_CAP, 0, taiko_codec_enable_slimtx,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_AIF_OUT_E("AIF2 CAP", "AIF2 Capture", 0, SND_SOC_NOPM,
		AIF2_CAP, 0, taiko_codec_enable_slimtx,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_AIF_OUT_E("AIF3 CAP", "AIF3 Capture", 0, SND_SOC_NOPM,
		AIF3_CAP, 0, taiko_codec_enable_slimtx,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_AIF_OUT_E("AIF4 VI", "VIfeed", 0, SND_SOC_NOPM,
		AIF4_VIFEED, 0, taiko_codec_enable_slimvi_feedback,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_OUT_E("AIF4 MAD", "AIF4 MAD TX", 0,
			       SND_SOC_NOPM, 0, 0,
			       taiko_codec_enable_mad, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_SWITCH("MADONOFF", SND_SOC_NOPM, 0, 0,
			    &aif4_mad_switch),
	SND_SOC_DAPM_INPUT("MADINPUT"),

	SND_SOC_DAPM_MIXER("AIF1_CAP Mixer", SND_SOC_NOPM, AIF1_CAP, 0,
		aif_cap_mixer, ARRAY_SIZE(aif_cap_mixer)),

	SND_SOC_DAPM_MIXER("AIF2_CAP Mixer", SND_SOC_NOPM, AIF2_CAP, 0,
		aif_cap_mixer, ARRAY_SIZE(aif_cap_mixer)),

	SND_SOC_DAPM_MIXER("AIF3_CAP Mixer", SND_SOC_NOPM, AIF3_CAP, 0,
		aif_cap_mixer, ARRAY_SIZE(aif_cap_mixer)),

	SND_SOC_DAPM_MUX("SLIM TX1 MUX", SND_SOC_NOPM, TAIKO_TX1, 0,
		&sb_tx1_mux),
	SND_SOC_DAPM_MUX("SLIM TX2 MUX", SND_SOC_NOPM, TAIKO_TX2, 0,
		&sb_tx2_mux),
	SND_SOC_DAPM_MUX("SLIM TX3 MUX", SND_SOC_NOPM, TAIKO_TX3, 0,
		&sb_tx3_mux),
	SND_SOC_DAPM_MUX("SLIM TX4 MUX", SND_SOC_NOPM, TAIKO_TX4, 0,
		&sb_tx4_mux),
	SND_SOC_DAPM_MUX("SLIM TX5 MUX", SND_SOC_NOPM, TAIKO_TX5, 0,
		&sb_tx5_mux),
	SND_SOC_DAPM_MUX("SLIM TX6 MUX", SND_SOC_NOPM, TAIKO_TX6, 0,
		&sb_tx6_mux),
	SND_SOC_DAPM_MUX("SLIM TX7 MUX", SND_SOC_NOPM, TAIKO_TX7, 0,
		&sb_tx7_mux),
	SND_SOC_DAPM_MUX("SLIM TX8 MUX", SND_SOC_NOPM, TAIKO_TX8, 0,
		&sb_tx8_mux),
	SND_SOC_DAPM_MUX("SLIM TX9 MUX", SND_SOC_NOPM, TAIKO_TX9, 0,
		&sb_tx9_mux),
	SND_SOC_DAPM_MUX("SLIM TX10 MUX", SND_SOC_NOPM, TAIKO_TX10, 0,
		&sb_tx10_mux),

	/* Digital Mic Inputs */
	SND_SOC_DAPM_ADC_E("DMIC1", NULL, SND_SOC_NOPM, 0, 0,
		taiko_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("DMIC2", NULL, SND_SOC_NOPM, 0, 0,
		taiko_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("DMIC3", NULL, SND_SOC_NOPM, 0, 0,
		taiko_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("DMIC4", NULL, SND_SOC_NOPM, 0, 0,
		taiko_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("DMIC5", NULL, SND_SOC_NOPM, 0, 0,
		taiko_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("DMIC6", NULL, SND_SOC_NOPM, 0, 0,
		taiko_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX_E("DEC10 MUX", TAIKO_A_CDC_CLK_TX_CLK_EN_B2_CTL, 1, 0,
		&dec10_mux, taiko_codec_enable_dec,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	/* Sidetone */
	SND_SOC_DAPM_MUX("IIR1 INP1 MUX", SND_SOC_NOPM, 0, 0, &iir1_inp1_mux),
	SND_SOC_DAPM_MUX("IIR1 INP2 MUX", SND_SOC_NOPM, 0, 0, &iir1_inp2_mux),
	SND_SOC_DAPM_MUX("IIR1 INP3 MUX", SND_SOC_NOPM, 0, 0, &iir1_inp3_mux),
    SND_SOC_DAPM_MUX("IIR1 INP4 MUX", SND_SOC_NOPM, 0, 0, &iir1_inp4_mux),
	SND_SOC_DAPM_MIXER("IIR1", TAIKO_A_CDC_CLK_SD_CTL, 0, 0, NULL, 0),
	SND_SOC_DAPM_MUX("IIR2 INP1 MUX", SND_SOC_NOPM, 0, 0, &iir2_inp1_mux),
    SND_SOC_DAPM_MUX("IIR2 INP2 MUX", SND_SOC_NOPM, 0, 0, &iir2_inp2_mux),
	SND_SOC_DAPM_MUX("IIR2 INP3 MUX", SND_SOC_NOPM, 0, 0, &iir2_inp3_mux),
	SND_SOC_DAPM_MUX("IIR2 INP4 MUX", SND_SOC_NOPM, 0, 0, &iir2_inp4_mux),
	SND_SOC_DAPM_MIXER("IIR2", TAIKO_A_CDC_CLK_SD_CTL, 1, 0, NULL, 0),

	/* AUX PGA */
	SND_SOC_DAPM_ADC_E("AUX_PGA_Left", NULL, TAIKO_A_RX_AUX_SW_CTL, 7, 0,
		taiko_codec_enable_aux_pga, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("AUX_PGA_Right", NULL, TAIKO_A_RX_AUX_SW_CTL, 6, 0,
		taiko_codec_enable_aux_pga, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	/* Lineout, ear and HPH PA Mixers */

	SND_SOC_DAPM_MIXER("EAR_PA_MIXER", SND_SOC_NOPM, 0, 0,
		ear_pa_mix, ARRAY_SIZE(ear_pa_mix)),

	SND_SOC_DAPM_MIXER("HPHL_PA_MIXER", SND_SOC_NOPM, 0, 0,
		hphl_pa_mix, ARRAY_SIZE(hphl_pa_mix)),

	SND_SOC_DAPM_MIXER("HPHR_PA_MIXER", SND_SOC_NOPM, 0, 0,
		hphr_pa_mix, ARRAY_SIZE(hphr_pa_mix)),

	SND_SOC_DAPM_MIXER("LINEOUT1_PA_MIXER", SND_SOC_NOPM, 0, 0,
		lineout1_pa_mix, ARRAY_SIZE(lineout1_pa_mix)),

	SND_SOC_DAPM_MIXER("LINEOUT2_PA_MIXER", SND_SOC_NOPM, 0, 0,
		lineout2_pa_mix, ARRAY_SIZE(lineout2_pa_mix)),

	SND_SOC_DAPM_MIXER("LINEOUT3_PA_MIXER", SND_SOC_NOPM, 0, 0,
		lineout3_pa_mix, ARRAY_SIZE(lineout3_pa_mix)),

	SND_SOC_DAPM_MIXER("LINEOUT4_PA_MIXER", SND_SOC_NOPM, 0, 0,
		lineout4_pa_mix, ARRAY_SIZE(lineout4_pa_mix)),
	SND_SOC_DAPM_SWITCH("VIONOFF", SND_SOC_NOPM, 0, 0,
			    &aif4_vi_switch),
	SND_SOC_DAPM_INPUT("VIINPUT"),

};

static irqreturn_t taiko_slimbus_irq(int irq, void *data)
{
	struct taiko_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;
	unsigned long status = 0;
	int i, j, port_id, k;
	u32 bit;
	u8 val, int_val = 0;
	bool tx, cleared;
	unsigned short reg = 0;

	for (i = TAIKO_SLIM_PGD_PORT_INT_STATUS_RX_0, j = 0;
	     i <= TAIKO_SLIM_PGD_PORT_INT_STATUS_TX_1; i++, j++) {
		val = wcd9xxx_interface_reg_read(codec->control_data, i);
		status |= ((u32)val << (8 * j));
	}

	for_each_set_bit(j, &status, 32) {
		tx = (j >= 16 ? true : false);
		port_id = (tx ? j - 16 : j);
		val = wcd9xxx_interface_reg_read(codec->control_data,
					TAIKO_SLIM_PGD_PORT_INT_RX_SOURCE0 + j);
		if (val & TAIKO_SLIM_IRQ_OVERFLOW)
			pr_debug_ratelimited(
			   "%s: overflow error on %s port %d, value %x\n",
			   __func__, (tx ? "TX" : "RX"), port_id, val);
		if (val & TAIKO_SLIM_IRQ_UNDERFLOW)
			pr_debug_ratelimited(
			   "%s: underflow error on %s port %d, value %x\n",
			   __func__, (tx ? "TX" : "RX"), port_id, val);
		if ((val & TAIKO_SLIM_IRQ_OVERFLOW) ||
			(val & TAIKO_SLIM_IRQ_UNDERFLOW)) {
			if (!tx)
				reg = TAIKO_SLIM_PGD_PORT_INT_EN0 +
					(port_id / 8);
			else
				reg = TAIKO_SLIM_PGD_PORT_INT_TX_EN0 +
					(port_id / 8);
			int_val = wcd9xxx_interface_reg_read(
				codec->control_data, reg);
			if (int_val & (1 << (port_id % 8))) {
				int_val = int_val ^ (1 << (port_id % 8));
				wcd9xxx_interface_reg_write(codec->control_data,
					reg, int_val);
			}
		}
		if (val & TAIKO_SLIM_IRQ_PORT_CLOSED) {
			/*
			 * INT SOURCE register starts from RX to TX
			 * but port number in the ch_mask is in opposite way
			 */
			bit = (tx ? j - 16 : j + 16);
			pr_debug("%s: %s port %d closed value %x, bit %u\n",
				 __func__, (tx ? "TX" : "RX"), port_id, val,
				 bit);
			for (k = 0, cleared = false; k < NUM_CODEC_DAIS; k++) {
				pr_debug("%s: priv->dai[%d].ch_mask = 0x%lx\n",
					 __func__, k, priv->dai[k].ch_mask);
				if (test_and_clear_bit(bit,
						       &priv->dai[k].ch_mask)) {
					cleared = true;
					if (!priv->dai[k].ch_mask)
						wake_up(&priv->dai[k].dai_wait);
					/*
					 * There are cases when multiple DAIs
					 * might be using the same slimbus
					 * channel. Hence don't break here.
					 */
				}
			}
			WARN(!cleared,
			     "Couldn't find slimbus %s port %d for closing\n",
			     (tx ? "TX" : "RX"), port_id);
		}
		wcd9xxx_interface_reg_write(codec->control_data,
					    TAIKO_SLIM_PGD_PORT_INT_CLR_RX_0 +
					    (j / 8),
					    1 << (j % 8));
	}

	return IRQ_HANDLED;
}

static int taiko_handle_pdata(struct taiko_priv *taiko)
{
	struct snd_soc_codec *codec = taiko->codec;
	struct wcd9xxx_pdata *pdata = taiko->resmgr.pdata;
	int k1, k2, k3, rc = 0;
	u8 leg_mode, txfe_bypass, txfe_buff, flag;
	u8 i = 0, j = 0;
	u8 val_txfe = 0, value = 0;
	u8 dmic_sample_rate_value = 0;
	u8 dmic_b1_ctl_value = 0, dmic_b2_ctl_value = 0;
	u8 anc_ctl_value = 0;

	if (!pdata) {
		pr_debug("%s: NULL pdata\n", __func__);
		rc = -ENODEV;
		goto done;
	}

	leg_mode = pdata->amic_settings.legacy_mode;
	txfe_bypass = pdata->amic_settings.txfe_enable;
	txfe_buff = pdata->amic_settings.txfe_buff;
	flag = pdata->amic_settings.use_pdata;

	/* Make sure settings are correct */
	if ((pdata->micbias.ldoh_v > WCD9XXX_LDOH_3P0_V) ||
	    (pdata->micbias.bias1_cfilt_sel > WCD9XXX_CFILT3_SEL) ||
	    (pdata->micbias.bias2_cfilt_sel > WCD9XXX_CFILT3_SEL) ||
	    (pdata->micbias.bias3_cfilt_sel > WCD9XXX_CFILT3_SEL) ||
	    (pdata->micbias.bias4_cfilt_sel > WCD9XXX_CFILT3_SEL)) {
		rc = -EINVAL;
		goto done;
	}
	/* figure out k value */
	k1 = wcd9xxx_resmgr_get_k_val(&taiko->resmgr, pdata->micbias.cfilt1_mv);
	k2 = wcd9xxx_resmgr_get_k_val(&taiko->resmgr, pdata->micbias.cfilt2_mv);
	k3 = wcd9xxx_resmgr_get_k_val(&taiko->resmgr, pdata->micbias.cfilt3_mv);

	if (IS_ERR_VALUE(k1) || IS_ERR_VALUE(k2) || IS_ERR_VALUE(k3)) {
		rc = -EINVAL;
		goto done;
	}
	/* Set voltage level and always use LDO */
	snd_soc_update_bits(codec, TAIKO_A_LDO_H_MODE_1, 0x0C,
			    (pdata->micbias.ldoh_v << 2));

	snd_soc_update_bits(codec, TAIKO_A_MICB_CFILT_1_VAL, 0xFC, (k1 << 2));
	snd_soc_update_bits(codec, TAIKO_A_MICB_CFILT_2_VAL, 0xFC, (k2 << 2));
	snd_soc_update_bits(codec, TAIKO_A_MICB_CFILT_3_VAL, 0xFC, (k3 << 2));

	snd_soc_update_bits(codec, TAIKO_A_MICB_1_CTL, 0x60,
			    (pdata->micbias.bias1_cfilt_sel << 5));
	snd_soc_update_bits(codec, TAIKO_A_MICB_2_CTL, 0x60,
			    (pdata->micbias.bias2_cfilt_sel << 5));
	snd_soc_update_bits(codec, TAIKO_A_MICB_3_CTL, 0x60,
			    (pdata->micbias.bias3_cfilt_sel << 5));
	snd_soc_update_bits(codec, taiko->resmgr.reg_addr->micb_4_ctl, 0x60,
			    (pdata->micbias.bias4_cfilt_sel << 5));

	for (i = 0; i < 6; j++, i += 2) {
		if (flag & (0x01 << i)) {
			value = (leg_mode & (0x01 << i)) ? 0x10 : 0x00;
			val_txfe = (txfe_bypass & (0x01 << i)) ? 0x20 : 0x00;
			val_txfe = val_txfe |
				((txfe_buff & (0x01 << i)) ? 0x10 : 0x00);
			snd_soc_update_bits(codec, TAIKO_A_TX_1_2_EN + j * 10,
				0x10, value);
			snd_soc_update_bits(codec,
				TAIKO_A_TX_1_2_TEST_EN + j * 10,
				0x30, val_txfe);
		}
		if (flag & (0x01 << (i + 1))) {
			value = (leg_mode & (0x01 << (i + 1))) ? 0x01 : 0x00;
			val_txfe = (txfe_bypass &
					(0x01 << (i + 1))) ? 0x02 : 0x00;
			val_txfe |= (txfe_buff &
					(0x01 << (i + 1))) ? 0x01 : 0x00;
			snd_soc_update_bits(codec, TAIKO_A_TX_1_2_EN + j * 10,
				0x01, value);
			snd_soc_update_bits(codec,
				TAIKO_A_TX_1_2_TEST_EN + j * 10,
				0x03, val_txfe);
		}
	}
	if (flag & 0x40) {
		value = (leg_mode & 0x40) ? 0x10 : 0x00;
		value = value | ((txfe_bypass & 0x40) ? 0x02 : 0x00);
		value = value | ((txfe_buff & 0x40) ? 0x01 : 0x00);
		snd_soc_update_bits(codec, TAIKO_A_TX_7_MBHC_EN,
			0x13, value);
	}

	if (pdata->ocp.use_pdata) {
		/* not defined in CODEC specification */
		if (pdata->ocp.hph_ocp_limit == 1 ||
			pdata->ocp.hph_ocp_limit == 5) {
			rc = -EINVAL;
			goto done;
		}
		snd_soc_update_bits(codec, TAIKO_A_RX_COM_OCP_CTL,
			0x0F, pdata->ocp.num_attempts);
		snd_soc_write(codec, TAIKO_A_RX_COM_OCP_COUNT,
			((pdata->ocp.run_time << 4) | pdata->ocp.wait_time));
		snd_soc_update_bits(codec, TAIKO_A_RX_HPH_OCP_CTL,
			0xE0, (pdata->ocp.hph_ocp_limit << 5));
	}

	for (i = 0; i < ARRAY_SIZE(pdata->regulator); i++) {
		if (pdata->regulator[i].name &&
		    !strncmp(pdata->regulator[i].name, "CDC_VDDA_RX", 11)) {
			if (pdata->regulator[i].min_uV == 1800000 &&
			    pdata->regulator[i].max_uV == 1800000) {
				snd_soc_write(codec, TAIKO_A_BIAS_REF_CTL,
					      0x1C);
			} else if (pdata->regulator[i].min_uV == 2200000 &&
				   pdata->regulator[i].max_uV == 2200000) {
				snd_soc_write(codec, TAIKO_A_BIAS_REF_CTL,
					      0x1E);
			} else {
				pr_err("%s: unsupported CDC_VDDA_RX voltage\n"
				       "min %d, max %d\n", __func__,
				       pdata->regulator[i].min_uV,
				       pdata->regulator[i].max_uV);
				rc = -EINVAL;
			}
			break;
		}
	}

	/* Set micbias capless mode with tail current */
	value = (pdata->micbias.bias1_cap_mode == MICBIAS_EXT_BYP_CAP ?
		 0x00 : 0x16);
	snd_soc_update_bits(codec, TAIKO_A_MICB_1_CTL, 0x1E, value);
	value = (pdata->micbias.bias2_cap_mode == MICBIAS_EXT_BYP_CAP ?
		 0x00 : 0x16);
	snd_soc_update_bits(codec, TAIKO_A_MICB_2_CTL, 0x1E, value);
	value = (pdata->micbias.bias3_cap_mode == MICBIAS_EXT_BYP_CAP ?
		 0x00 : 0x16);
	snd_soc_update_bits(codec, TAIKO_A_MICB_3_CTL, 0x1E, value);
	value = (pdata->micbias.bias4_cap_mode == MICBIAS_EXT_BYP_CAP ?
		 0x00 : 0x16);
	snd_soc_update_bits(codec, TAIKO_A_MICB_4_CTL, 0x1E, value);

	/* Set the DMIC sample rate */
	if (pdata->mclk_rate == TAIKO_MCLK_CLK_9P6MHZ) {
		switch (pdata->dmic_sample_rate) {
		case WCD9XXX_DMIC_SAMPLE_RATE_2P4MHZ:
			dmic_sample_rate_value = WCD9XXX_DMIC_SAMPLE_RATE_DIV_4;
			dmic_b1_ctl_value = WCD9XXX_DMIC_B1_CTL_DIV_4;
			dmic_b2_ctl_value = WCD9XXX_DMIC_B2_CTL_DIV_4;
			anc_ctl_value = WCD9XXX_ANC_DMIC_X2_OFF;
			break;
		case WCD9XXX_DMIC_SAMPLE_RATE_4P8MHZ:
			dmic_sample_rate_value = WCD9XXX_DMIC_SAMPLE_RATE_DIV_2;
			dmic_b1_ctl_value = WCD9XXX_DMIC_B1_CTL_DIV_2;
			dmic_b2_ctl_value = WCD9XXX_DMIC_B2_CTL_DIV_2;
			anc_ctl_value = WCD9XXX_ANC_DMIC_X2_ON;
			break;
		case WCD9XXX_DMIC_SAMPLE_RATE_3P2MHZ:
		case WCD9XXX_DMIC_SAMPLE_RATE_UNDEFINED:
			dmic_sample_rate_value = WCD9XXX_DMIC_SAMPLE_RATE_DIV_3;
			dmic_b1_ctl_value = WCD9XXX_DMIC_B1_CTL_DIV_3;
			dmic_b2_ctl_value = WCD9XXX_DMIC_B2_CTL_DIV_3;
			anc_ctl_value = WCD9XXX_ANC_DMIC_X2_OFF;
			break;
		default:
			pr_err("%s Invalid sample rate %d for mclk %d\n",
			__func__, pdata->dmic_sample_rate, pdata->mclk_rate);
			rc = -EINVAL;
			goto done;
			break;
		}
	} else if (pdata->mclk_rate == TAIKO_MCLK_CLK_12P288MHZ) {
		switch (pdata->dmic_sample_rate) {
		case WCD9XXX_DMIC_SAMPLE_RATE_3P072MHZ:
			dmic_sample_rate_value = WCD9XXX_DMIC_SAMPLE_RATE_DIV_4;
			dmic_b1_ctl_value = WCD9XXX_DMIC_B1_CTL_DIV_4;
			dmic_b2_ctl_value = WCD9XXX_DMIC_B2_CTL_DIV_4;
			anc_ctl_value = WCD9XXX_ANC_DMIC_X2_OFF;
			break;
		case WCD9XXX_DMIC_SAMPLE_RATE_6P144MHZ:
			dmic_sample_rate_value = WCD9XXX_DMIC_SAMPLE_RATE_DIV_2;
			dmic_b1_ctl_value = WCD9XXX_DMIC_B1_CTL_DIV_2;
			dmic_b2_ctl_value = WCD9XXX_DMIC_B2_CTL_DIV_2;
			anc_ctl_value = WCD9XXX_ANC_DMIC_X2_ON;
			break;
		case WCD9XXX_DMIC_SAMPLE_RATE_4P096MHZ:
		case WCD9XXX_DMIC_SAMPLE_RATE_UNDEFINED:
			dmic_sample_rate_value = WCD9XXX_DMIC_SAMPLE_RATE_DIV_3;
			dmic_b1_ctl_value = WCD9XXX_DMIC_B1_CTL_DIV_3;
			dmic_b2_ctl_value = WCD9XXX_DMIC_B2_CTL_DIV_3;
			anc_ctl_value = WCD9XXX_ANC_DMIC_X2_OFF;
			break;
		default:
			pr_err("%s Invalid sample rate %d for mclk %d\n",
			__func__, pdata->dmic_sample_rate, pdata->mclk_rate);
			rc = -EINVAL;
			goto done;
			break;
		}
	} else {
		pr_err("%s MCLK is not set!\n", __func__);
		rc = -EINVAL;
		goto done;
	}

	snd_soc_update_bits(codec, TAIKO_A_CDC_TX1_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_TX2_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_TX3_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_TX4_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_TX5_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_TX6_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_TX7_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_TX8_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_TX9_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_TX10_DMIC_CTL,
		0x7, dmic_sample_rate_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_DMIC_B1_CTL,
		0xEE, dmic_b1_ctl_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_CLK_DMIC_B2_CTL,
		0xE, dmic_b2_ctl_value);
	snd_soc_update_bits(codec, TAIKO_A_CDC_ANC1_B2_CTL,
		0x1, anc_ctl_value);

done:
	return rc;
}

static const struct wcd9xxx_reg_mask_val taiko_reg_defaults[] = {

	/* set MCLk to 9.6 */
	TAIKO_REG_VAL(TAIKO_A_CHIP_CTL, 0x02),
	TAIKO_REG_VAL(TAIKO_A_CDC_CLK_POWER_CTL, 0x03),

	/* EAR PA deafults  */
	TAIKO_REG_VAL(TAIKO_A_RX_EAR_CMBUFF, 0x05),

	/* RX defaults */
	TAIKO_REG_VAL(TAIKO_A_CDC_RX1_B5_CTL, 0x78),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX2_B5_CTL, 0x78),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX3_B5_CTL, 0x78),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX4_B5_CTL, 0x78),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX5_B5_CTL, 0x78),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX6_B5_CTL, 0x78),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX7_B5_CTL, 0x78),

	/* RX1 and RX2 defaults */
	TAIKO_REG_VAL(TAIKO_A_CDC_RX1_B6_CTL, 0xA0),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX2_B6_CTL, 0xA0),

	/* RX3 to RX7 defaults */
	TAIKO_REG_VAL(TAIKO_A_CDC_RX3_B6_CTL, 0x80),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX4_B6_CTL, 0x80),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX5_B6_CTL, 0x80),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX6_B6_CTL, 0x80),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX7_B6_CTL, 0x80),

	/* MAD registers */
	TAIKO_REG_VAL(TAIKO_A_MAD_ANA_CTRL, 0xF1),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_MAIN_CTL_1, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_MAIN_CTL_2, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_CTL_1, 0x00),
	/* Set SAMPLE_TX_EN bit */
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_CTL_2, 0x03),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_CTL_3, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_CTL_4, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_CTL_5, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_CTL_6, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_CTL_7, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_CTL_8, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_IIR_CTL_PTR, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_MAD_AUDIO_IIR_CTL_VAL, 0x40),
	TAIKO_REG_VAL(TAIKO_A_CDC_DEBUG_B7_CTL, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_CLK_OTHR_RESET_B1_CTL, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_CLK_OTHR_CTL, 0x00),
	TAIKO_REG_VAL(TAIKO_A_CDC_CONN_MAD, 0x01),

	/* BUCK default */
	TAIKO_REG_VAL(WCD9XXX_A_BUCK_CTRL_CCL_4, 0x51),
	TAIKO_REG_VAL(WCD9XXX_A_BUCK_CTRL_CCL_1, 0x5B),
};

/*
 * Don't update TAIKO_A_CHIP_CTL, TAIKO_A_BUCK_CTRL_CCL_1 and
 * TAIKO_A_RX_EAR_CMBUFF as those are updated in taiko_reg_defaults
 */
static const struct wcd9xxx_reg_mask_val taiko_2_0_reg_defaults[] = {
	TAIKO_REG_VAL(TAIKO_A_CDC_TX_1_GAIN, 0x2),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX_2_GAIN, 0x2),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX_1_2_ADC_IB, 0x44),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX_3_GAIN, 0x2),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX_4_GAIN, 0x2),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX_3_4_ADC_IB, 0x44),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX_5_GAIN, 0x2),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX_6_GAIN, 0x2),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX_5_6_ADC_IB, 0x44),
	TAIKO_REG_VAL(WCD9XXX_A_BUCK_MODE_3, 0xCE),
	TAIKO_REG_VAL(WCD9XXX_A_BUCK_CTRL_VCL_1, 0x8),
	TAIKO_REG_VAL(TAIKO_A_BUCK_CTRL_CCL_4, 0x51),
	TAIKO_REG_VAL(TAIKO_A_NCP_DTEST, 0x10),
	TAIKO_REG_VAL(TAIKO_A_RX_HPH_CHOP_CTL, 0xA4),
	TAIKO_REG_VAL(TAIKO_A_RX_HPH_BIAS_PA, 0x7A),
	TAIKO_REG_VAL(TAIKO_A_RX_HPH_OCP_CTL, 0x6B),
	TAIKO_REG_VAL(TAIKO_A_RX_HPH_CNP_WG_CTL, 0xDA),
	TAIKO_REG_VAL(TAIKO_A_RX_HPH_CNP_WG_TIME, 0x15),
	TAIKO_REG_VAL(TAIKO_A_RX_EAR_BIAS_PA, 0x76),
	TAIKO_REG_VAL(TAIKO_A_RX_EAR_CNP, 0xC0),
	TAIKO_REG_VAL(TAIKO_A_RX_LINE_BIAS_PA, 0x7A),
	TAIKO_REG_VAL(TAIKO_A_RX_LINE_1_TEST, 0x2),
	TAIKO_REG_VAL(TAIKO_A_RX_LINE_2_TEST, 0x2),
	TAIKO_REG_VAL(TAIKO_A_RX_LINE_3_TEST, 0x2),
	TAIKO_REG_VAL(TAIKO_A_RX_LINE_4_TEST, 0x2),
	TAIKO_REG_VAL(TAIKO_A_SPKR_DRV_OCP_CTL, 0x97),
	TAIKO_REG_VAL(TAIKO_A_SPKR_DRV_CLIP_DET, 0x1),
	TAIKO_REG_VAL(TAIKO_A_SPKR_DRV_IEC, 0x0),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX1_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX2_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX3_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX4_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX5_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX6_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX7_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX8_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX9_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_TX10_MUX_CTL, 0x48),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX1_B4_CTL, 0x8),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX2_B4_CTL, 0x8),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX3_B4_CTL, 0x8),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX4_B4_CTL, 0x8),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX5_B4_CTL, 0x8),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX6_B4_CTL, 0x8),
	TAIKO_REG_VAL(TAIKO_A_CDC_RX7_B4_CTL, 0x8),
	TAIKO_REG_VAL(TAIKO_A_CDC_VBAT_GAIN_UPD_MON, 0x0),
	TAIKO_REG_VAL(TAIKO_A_CDC_PA_RAMP_B1_CTL, 0x0),
	TAIKO_REG_VAL(TAIKO_A_CDC_PA_RAMP_B2_CTL, 0x0),
	TAIKO_REG_VAL(TAIKO_A_CDC_PA_RAMP_B3_CTL, 0x0),
	TAIKO_REG_VAL(TAIKO_A_CDC_PA_RAMP_B4_CTL, 0x0),
	TAIKO_REG_VAL(TAIKO_A_CDC_SPKR_CLIPDET_B1_CTL, 0x0),
	TAIKO_REG_VAL(TAIKO_A_CDC_COMP0_B4_CTL, 0x37),
	TAIKO_REG_VAL(TAIKO_A_CDC_COMP0_B5_CTL, 0x7f),
	TAIKO_REG_VAL(TAIKO_A_CDC_COMP0_B5_CTL, 0x7f),
};

static void taiko_update_reg_defaults(struct snd_soc_codec *codec)
{
	u32 i;
	struct wcd9xxx *taiko_core = dev_get_drvdata(codec->dev->parent);

	for (i = 0; i < ARRAY_SIZE(taiko_reg_defaults); i++)
		snd_soc_write(codec, taiko_reg_defaults[i].reg,
			      taiko_reg_defaults[i].val);

	for (i = 0; i < ARRAY_SIZE(taiko_2_0_reg_defaults); i++) {
		snd_soc_write(codec, taiko_2_0_reg_defaults[i].reg,
			      taiko_2_0_reg_defaults[i].val);
    }
	spkr_drv_wrnd = -1;
}

static const struct wcd9xxx_reg_mask_val taiko_codec_reg_init_val[] = {
	/* Initialize current threshold to 350MA
	 * number of wait and run cycles to 4096
	 */
	{TAIKO_A_RX_HPH_OCP_CTL, 0xE1, 0x61},
	{TAIKO_A_RX_COM_OCP_COUNT, 0xFF, 0xFF},
	{TAIKO_A_RX_HPH_L_TEST, 0x01, 0x01},
	{TAIKO_A_RX_HPH_R_TEST, 0x01, 0x01},

	/* Initialize gain registers to use register gain */
	{WCD9XXX_A_RX_HPH_L_GAIN, 0x20, 0x20},
	{WCD9XXX_A_RX_HPH_R_GAIN, 0x20, 0x20},
	{TAIKO_A_RX_LINE_1_GAIN, 0x20, 0x20},
	{TAIKO_A_RX_LINE_2_GAIN, 0x20, 0x20},
	{TAIKO_A_RX_LINE_3_GAIN, 0x20, 0x20},
	{TAIKO_A_RX_LINE_4_GAIN, 0x20, 0x20},
	{TAIKO_A_SPKR_DRV_GAIN, 0x04, 0x04},

	/* Use 16 bit sample size for TX1 to TX6 */
	{TAIKO_A_CDC_CONN_TX_SB_B1_CTL, 0x30, 0x20},
	{TAIKO_A_CDC_CONN_TX_SB_B2_CTL, 0x30, 0x20},
	{TAIKO_A_CDC_CONN_TX_SB_B3_CTL, 0x30, 0x20},
	{TAIKO_A_CDC_CONN_TX_SB_B4_CTL, 0x30, 0x20},
	{TAIKO_A_CDC_CONN_TX_SB_B5_CTL, 0x30, 0x20},
	{TAIKO_A_CDC_CONN_TX_SB_B6_CTL, 0x30, 0x20},

	/* Use 16 bit sample size for TX7 to TX10 */
	{TAIKO_A_CDC_CONN_TX_SB_B7_CTL, 0x60, 0x40},
	{TAIKO_A_CDC_CONN_TX_SB_B8_CTL, 0x60, 0x40},
	{TAIKO_A_CDC_CONN_TX_SB_B9_CTL, 0x60, 0x40},
	{TAIKO_A_CDC_CONN_TX_SB_B10_CTL, 0x60, 0x40},

	/*enable HPF filter for TX paths */
	{TAIKO_A_CDC_TX1_MUX_CTL, 0x8, 0x0},
	{TAIKO_A_CDC_TX2_MUX_CTL, 0x8, 0x0},
	{TAIKO_A_CDC_TX3_MUX_CTL, 0x8, 0x0},
	{TAIKO_A_CDC_TX4_MUX_CTL, 0x8, 0x0},
	{TAIKO_A_CDC_TX5_MUX_CTL, 0x8, 0x0},
	{TAIKO_A_CDC_TX6_MUX_CTL, 0x8, 0x0},
	{TAIKO_A_CDC_TX7_MUX_CTL, 0x8, 0x0},
	{TAIKO_A_CDC_TX8_MUX_CTL, 0x8, 0x0},
	{TAIKO_A_CDC_TX9_MUX_CTL, 0x8, 0x0},
	{TAIKO_A_CDC_TX10_MUX_CTL, 0x8, 0x0},

	/* Compander zone selection */
	{TAIKO_A_CDC_COMP0_B4_CTL, 0x3F, 0x37},
	{TAIKO_A_CDC_COMP1_B4_CTL, 0x3F, 0x37},
	{TAIKO_A_CDC_COMP2_B4_CTL, 0x3F, 0x37},
	{TAIKO_A_CDC_COMP0_B5_CTL, 0x7F, 0x7F},
	{TAIKO_A_CDC_COMP1_B5_CTL, 0x7F, 0x7F},
	{TAIKO_A_CDC_COMP2_B5_CTL, 0x7F, 0x7F},

	/*
	 * Setup wavegen timer to 20msec and disable chopper
	 * as default. This corresponds to Compander OFF
	 */
	{TAIKO_A_RX_HPH_CNP_WG_CTL, 0xFF, 0xDB},
	{TAIKO_A_RX_HPH_CNP_WG_TIME, 0xFF, 0x58},
	{TAIKO_A_RX_HPH_BIAS_WG_OCP, 0xFF, 0x1A},
	{TAIKO_A_RX_HPH_CHOP_CTL, 0xFF, 0x24},

	/* Choose max non-overlap time for NCP */
	{TAIKO_A_NCP_CLK, 0xFF, 0xFC},

	/* Program the 0.85 volt VBG_REFERENCE */
	{TAIKO_A_BIAS_CURR_CTL_2, 0xFF, 0x04},

	/* set MAD input MIC to DMIC1 */
	{TAIKO_A_CDC_CONN_MAD, 0x0F, 0x08},

};

static void taiko_codec_init_reg(struct snd_soc_codec *codec)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(taiko_codec_reg_init_val); i++)
		snd_soc_update_bits(codec, taiko_codec_reg_init_val[i].reg,
				taiko_codec_reg_init_val[i].mask,
				taiko_codec_reg_init_val[i].val);
}

static void taiko_slim_interface_init_reg(struct snd_soc_codec *codec)
{
	int i;

	for (i = 0; i < WCD9XXX_SLIM_NUM_PORT_REG; i++)
		wcd9xxx_interface_reg_write(codec->control_data,
					    TAIKO_SLIM_PGD_PORT_INT_EN0 + i,
					    0xFF);
}

static int taiko_setup_irqs(struct taiko_priv *taiko)
{
	int ret = 0;
	struct snd_soc_codec *codec = taiko->codec;
	struct wcd9xxx *wcd9xxx = codec->control_data;
	struct wcd9xxx_core_resource *core_res =
				&wcd9xxx->core_res;

	ret = wcd9xxx_request_irq(core_res, WCD9XXX_IRQ_SLIMBUS,
				  taiko_slimbus_irq, "SLIMBUS Slave", taiko);
	if (ret)
		pr_debug("%s: Failed to request irq %d\n", __func__,
		       WCD9XXX_IRQ_SLIMBUS);
	else
		taiko_slim_interface_init_reg(codec);

	return ret;
}

static void taiko_cleanup_irqs(struct taiko_priv *taiko)
{
	struct snd_soc_codec *codec = taiko->codec;
	struct wcd9xxx *wcd9xxx = codec->control_data;
	struct wcd9xxx_core_resource *core_res =
				&wcd9xxx->core_res;

	wcd9xxx_free_irq(core_res, WCD9XXX_IRQ_SLIMBUS, taiko);
}

int taiko_hs_detect(struct snd_soc_codec *codec,
		    struct wcd9xxx_mbhc_config *mbhc_cfg)
{
	int rc;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	rc = wcd9xxx_mbhc_start(&taiko->mbhc, mbhc_cfg);
	if (!rc)
		taiko->mbhc_started = true;
	return rc;
}
EXPORT_SYMBOL(taiko_hs_detect);

void taiko_hs_detect_exit(struct snd_soc_codec *codec)
{
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	wcd9xxx_mbhc_stop(&taiko->mbhc);
	taiko->mbhc_started = false;
}
EXPORT_SYMBOL(taiko_hs_detect_exit);

void taiko_event_register(
	int (*machine_event_cb)(struct snd_soc_codec *codec,
				enum wcd9xxx_codec_event),
	struct snd_soc_codec *codec)
{
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);
	taiko->machine_codec_event_cb = machine_event_cb;
}
EXPORT_SYMBOL(taiko_event_register);

static void taiko_init_slim_slave_cfg(struct snd_soc_codec *codec)
{
	struct taiko_priv *priv = snd_soc_codec_get_drvdata(codec);
	struct afe_param_cdc_slimbus_slave_cfg *cfg;
	struct wcd9xxx *wcd9xxx = codec->control_data;
	uint64_t eaddr = 0;

	cfg = &priv->slimbus_slave_cfg;
	cfg->minor_version = 1;
	cfg->tx_slave_port_offset = 0;
	cfg->rx_slave_port_offset = 16;

	memcpy(&eaddr, &wcd9xxx->slim->e_addr, sizeof(wcd9xxx->slim->e_addr));
	WARN_ON(sizeof(wcd9xxx->slim->e_addr) != 6);
	cfg->device_enum_addr_lsw = eaddr & 0xFFFFFFFF;
	cfg->device_enum_addr_msw = eaddr >> 32;

	pr_debug("%s: slimbus logical address 0x%llx\n", __func__, eaddr);
}

static int taiko_device_down(struct wcd9xxx *wcd9xxx)
{
	struct snd_soc_codec *codec;

	codec = (struct snd_soc_codec *)(wcd9xxx->ssr_priv);
	snd_soc_card_change_online_state(codec->card, 0);

	return 0;
}

static int wcd9xxx_prepare_static_pa(struct wcd9xxx_mbhc *mbhc,
				     struct list_head *lh)
{
	int i;
	struct snd_soc_codec *codec = mbhc->codec;

	const struct wcd9xxx_reg_mask_val reg_set_paon[] = {
		{TAIKO_A_RX_HPH_OCP_CTL, 0x18, 0x00},
		{TAIKO_A_RX_HPH_L_TEST, 0x1, 0x0},
		{TAIKO_A_RX_HPH_R_TEST, 0x1, 0x0},
		{TAIKO_A_RX_HPH_BIAS_WG_OCP, 0xff, 0x1A},
		{TAIKO_A_RX_HPH_CNP_WG_CTL, 0xff, 0xDB},
		{TAIKO_A_RX_HPH_CNP_WG_TIME, 0xff, 0x15},
		{TAIKO_A_CDC_RX1_B6_CTL, 0xff, 0x81},
		{TAIKO_A_CDC_CLK_RX_B1_CTL, 0x01, 0x01},
		{TAIKO_A_RX_HPH_CHOP_CTL, 0xff, 0xA4},
//		{TAIKO_A_RX_HPH_L_GAIN, 0xff, 0x2C},
		{TAIKO_A_CDC_RX2_B6_CTL, 0xff, 0x81},
		{TAIKO_A_CDC_CLK_RX_B1_CTL, 0x02, 0x02},
//		{TAIKO_A_RX_HPH_R_GAIN, 0xff, 0x2C},
		{TAIKO_A_NCP_CLK, 0xff, 0xFC},
		{TAIKO_A_BUCK_CTRL_CCL_3, 0xff, 0x60},
		{TAIKO_A_RX_COM_BIAS, 0xff, 0x80},
		{TAIKO_A_BUCK_MODE_3, 0xff, 0xC6},
		{TAIKO_A_BUCK_MODE_4, 0xff, 0xE6},
		{TAIKO_A_BUCK_MODE_5, 0xff, 0x02},
		{TAIKO_A_BUCK_MODE_1, 0xff, 0xA1},
		{TAIKO_A_NCP_EN, 0xff, 0xFF},
		{TAIKO_A_BUCK_MODE_5, 0xff, 0x7B},
		{TAIKO_A_CDC_CLSH_B1_CTL, 0xff, 0xE6},
		{TAIKO_A_RX_HPH_L_DAC_CTL, 0xff, 0xC0},
		{TAIKO_A_RX_HPH_R_DAC_CTL, 0xff, 0xC0},
	};

	for (i = 0; i < ARRAY_SIZE(reg_set_paon); i++) {
		if (chopper_bypass && reg_set_paon[i].reg == TAIKO_A_RX_HPH_CHOP_CTL)
			continue;
		wcd9xxx_soc_update_bits_push(codec, lh,
					     reg_set_paon[i].reg,
					     reg_set_paon[i].mask,
					     reg_set_paon[i].val, 0);
	}
	pr_debug("%s: PAs are prepared\n", __func__);
bypass:
    update_bias();
	update_control_regs();
    write_hph_poweramp_regs();	
	return 0;
}

static int wcd9xxx_enable_static_pa(struct wcd9xxx_mbhc *mbhc, bool enable)
{
	struct snd_soc_codec *codec = mbhc->codec;
	const int wg_time = snd_soc_read(codec, WCD9XXX_A_RX_HPH_CNP_WG_TIME) *
			    TAIKO_WG_TIME_FACTOR_US;

	snd_soc_update_bits(codec, WCD9XXX_A_RX_HPH_CNP_EN, 0x30,
				    enable ? 0x30 : 0x0);
	/* Wait for wave gen time to avoid pop noise */
	usleep_range(wg_time, wg_time + WCD9XXX_USLEEP_RANGE_MARGIN_US);
	pr_debug("%s: PAs are %s as static mode (wg_time %d)\n", __func__,
		 enable ? "enabled" : "disabled", wg_time);
	return 0;
}

static int taiko_setup_zdet(struct wcd9xxx_mbhc *mbhc,
			    enum mbhc_impedance_detect_stages stage)
{
	int ret = 0;
	struct snd_soc_codec *codec = mbhc->codec;
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

#define __wr(reg, mask, value)						  \
	do {								  \
		ret = wcd9xxx_soc_update_bits_push(codec,		  \
						   &taiko->reg_save_restore, \
						   reg, mask, value, 0);  \
		if (ret < 0)						  \
			return ret;					  \
	} while (0)

	switch (stage) {

	case PRE_MEAS:
		INIT_LIST_HEAD(&taiko->reg_save_restore);
		wcd9xxx_prepare_static_pa(mbhc, &taiko->reg_save_restore);
		wcd9xxx_enable_static_pa(mbhc, true);

		/*
		 * save old value of registers and write the new value to
		 * restore old value back, WCD9XXX_A_CDC_PA_RAMP_B{1,2,3,4}_CTL
		 * registers don't need to be restored as those are solely used
		 * by impedance detection.
		 */
		/* Phase 1 */
		/* Reset the PA Ramp */
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B1_CTL, 0x1C);
		/*
		 * Connect the PA Ramp to PA chain and release reset with
		 * keep it connected.
		 */
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B1_CTL, 0x1F);
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B1_CTL, 0x03);
		/*
		 * Program the PA Ramp to FS_48K, L shift 1 and sample
		 * num to 24
		 */
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B3_CTL,
			      0x3 << 4 | 0x6);
		/* 0x56 for 10mv.  0xC0 is for 50mv */
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B4_CTL, 0xC0);
		/* Enable MBHC MUX, Set MUX current to 37.5uA and ADC7 */
		__wr(WCD9XXX_A_MBHC_SCALING_MUX_1, 0xFF, 0xC0);
		__wr(WCD9XXX_A_MBHC_SCALING_MUX_2, 0xFF, 0xF0);
		__wr(WCD9XXX_A_TX_7_MBHC_TEST_CTL, 0xFF, 0x78);
		__wr(WCD9XXX_A_TX_7_MBHC_EN, 0xFF, 0x8C);
		/* Change NSA and NAVG */
		__wr(WCD9XXX_A_CDC_MBHC_TIMER_B4_CTL, 0x4 << 4, 0x4 << 4);
		__wr(WCD9XXX_A_CDC_MBHC_TIMER_B5_CTL, 0xFF, 0x10);
		/* Reset MBHC and set it up for STA */
		__wr(WCD9XXX_A_CDC_MBHC_CLK_CTL, 0xFF, 0x0A);
		snd_soc_write(codec, WCD9XXX_A_CDC_MBHC_EN_CTL, 0x2);
		__wr(WCD9XXX_A_CDC_MBHC_CLK_CTL, 0xFF, 0x02);

		/* Set HPH_MBHC for zdet */
		__wr(WCD9XXX_A_MBHC_HPH, 0xB3, 0x80);
		break;
	case POST_MEAS:
		/* Phase 2 */
		/* Start the PA ramp on HPH L and R */
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B2_CTL, 0x05);
		/* Ramp generator takes ~17ms */
		usleep_range(ZDET_RAMP_WAIT_US,
				ZDET_RAMP_WAIT_US + WCD9XXX_USLEEP_RANGE_MARGIN_US);

		/* Disable Ical */
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B2_CTL, 0x00);
		/* Ramp generator takes ~17ms */
		usleep_range(ZDET_RAMP_WAIT_US,
				ZDET_RAMP_WAIT_US + WCD9XXX_USLEEP_RANGE_MARGIN_US);
		//update_hph_pa_gain();
		break;
	case PA_DISABLE:
		/* Ramp HPH L & R back to Zero */
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B2_CTL, 0x0A);
		/* Ramp generator takes ~17ms */
		usleep_range(ZDET_RAMP_WAIT_US,
				ZDET_RAMP_WAIT_US + WCD9XXX_USLEEP_RANGE_MARGIN_US);
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B2_CTL, 0x00);

		/* Clean up starts */
		/* Turn off PA ramp generator */
		snd_soc_write(codec, WCD9XXX_A_CDC_PA_RAMP_B1_CTL, 0x0);
		if (!mbhc->hph_pa_dac_state &&
		    (!(test_bit(MBHC_EVENT_PA_HPHL, &mbhc->event_state) ||
		       test_bit(MBHC_EVENT_PA_HPHR, &mbhc->event_state))))
			wcd9xxx_enable_static_pa(mbhc, false);
		wcd9xxx_restore_registers(codec, &taiko->reg_save_restore);
		break;
	default:
		dev_dbg(codec->dev, "%s: Case %d not supported\n",
			__func__, stage);
		break;
	}
#undef __wr

	return ret;
}

static void taiko_compute_impedance(s16 *l, s16 *r, uint32_t *zl, uint32_t *zr)
{

	int64_t rl, rr = 0; /* milliohm */
	const int alphal = 364; /* 0.005555 * 65536 = 364.05 */
	const int alphar = 364; /* 0.005555 * 65536 = 364.05 */
	const int beta = 3855; /* 0.011765 * 5 * 65536 = 3855.15 */
	const int rref = 11333; /* not scaled up */
	const int shift = 16;

	rl = (int)(l[0] - l[1]) * 1000 / (l[0] - l[2]);
	rl = rl * rref * alphal;
	rl = rl >> shift;
	rl = rl * beta;
	rl = rl >> shift;
	*zl = rl;

	rr = (int)(r[0] - r[1]) * 1000 / (r[0] - r[2]);
	rr = rr * rref  * alphar;
	rr = rr >> shift;
	rr = rr * beta;
	rr = rr >> shift;
	*zr = rr;
}

static enum wcd9xxx_cdc_type taiko_get_cdc_type(void)
{
	return WCD9XXX_CDC_TYPE_TAIKO;
}

static const struct wcd9xxx_mbhc_cb mbhc_cb = {
	.get_cdc_type = taiko_get_cdc_type,
	.setup_zdet = taiko_setup_zdet,
	.compute_impedance = taiko_compute_impedance,
};

static const struct wcd9xxx_mbhc_intr cdc_intr_ids = {
	.poll_plug_rem = WCD9XXX_IRQ_MBHC_REMOVAL,
	.shortavg_complete = WCD9XXX_IRQ_MBHC_SHORT_TERM,
	.potential_button_press = WCD9XXX_IRQ_MBHC_PRESS,
	.button_release = WCD9XXX_IRQ_MBHC_RELEASE,
	.dce_est_complete = WCD9XXX_IRQ_MBHC_POTENTIAL,
	.insertion = WCD9XXX_IRQ_MBHC_INSERTION,
	.hph_left_ocp = WCD9XXX_IRQ_HPH_PA_OCPL_FAULT,
	.hph_right_ocp = WCD9XXX_IRQ_HPH_PA_OCPR_FAULT,
	.hs_jack_switch = WCD9320_IRQ_MBHC_JACK_SWITCH,
};

static int taiko_post_reset_cb(struct wcd9xxx *wcd9xxx)
{
	int ret = 0;
	struct snd_soc_codec *codec;
	struct taiko_priv *taiko;
	int rco_clk_rate;
	int count;

	codec = (struct snd_soc_codec *)(wcd9xxx->ssr_priv);
	if (!codec)
		return -ENOMEM;
	taiko = snd_soc_codec_get_drvdata(codec);
	if (!taiko)
		return -ENOMEM;
	snd_soc_card_change_online_state(codec->card, 1);

	mutex_lock(&codec->mutex);

    if (codec->reg_def_copy) {
        pr_debug("%s: Update ASOC cache", __func__);
        kfree(codec->reg_cache);
        codec->reg_cache = kmemdup(codec->reg_def_copy,
                                            codec->reg_size, GFP_KERNEL);
        if (!codec->reg_cache) {
            mutex_unlock(&codec->mutex);
            pr_debug("%s: Cache update failed!\n", __func__);
            return -ENOMEM;
        }
    }

	taiko_update_reg_defaults(codec);
	if (wcd9xxx->mclk_rate == TAIKO_MCLK_CLK_12P288MHZ)
		snd_soc_update_bits(codec, TAIKO_A_CHIP_CTL, 0x06, 0x0);
	else if (wcd9xxx->mclk_rate == TAIKO_MCLK_CLK_9P6MHZ)
		snd_soc_update_bits(codec, TAIKO_A_CHIP_CTL, 0x06, 0x2);
	taiko_codec_init_reg(codec);

	if (spkr_drv_wrnd == 1)
		snd_soc_update_bits(codec, TAIKO_A_SPKR_DRV_EN, 0x80, 0x80);

	codec->cache_sync = true;
	snd_soc_cache_sync(codec);
	codec->cache_sync = false;

	ret = taiko_handle_pdata(taiko);
	if (IS_ERR_VALUE(ret))
		pr_debug("%s: bad pdata\n", __func__);

	taiko_init_slim_slave_cfg(codec);
	taiko_slim_interface_init_reg(codec);

	wcd9xxx_resmgr_post_ssr(&taiko->resmgr);

	if (taiko->mbhc_started) {
		wcd9xxx_mbhc_deinit(&taiko->mbhc);
		taiko->mbhc_started = false;

		rco_clk_rate = TAIKO_MCLK_CLK_9P6MHZ;

		ret = wcd9xxx_mbhc_init(&taiko->mbhc, &taiko->resmgr, codec,
					taiko_enable_mbhc_micbias,
					&mbhc_cb, &cdc_intr_ids,
					rco_clk_rate, false);
		if (ret)
			pr_debug("%s: mbhc init failed %d\n", __func__, ret);
		else
			taiko_hs_detect(codec, taiko->mbhc.mbhc_cfg);
	}

	if (taiko->machine_codec_event_cb)
		taiko->machine_codec_event_cb(codec,
				      WCD9XXX_CODEC_EVENT_CODEC_UP);

	taiko_cleanup_irqs(taiko);
	ret = taiko_setup_irqs(taiko);
	if (ret)
		pr_debug("%s: Failed to setup irq: %d\n", __func__, ret);

	for (count = 0; count < NUM_CODEC_DAIS; count++)
		taiko->dai[count].bus_down_in_recovery = true;

	mutex_unlock(&codec->mutex);
    update_bias();
	update_control_regs();
	return ret;
}

void *taiko_get_afe_config(struct snd_soc_codec *codec,
			   enum afe_config_type config_type)
{
	struct taiko_priv *priv = snd_soc_codec_get_drvdata(codec);
	struct wcd9xxx *taiko_core = dev_get_drvdata(codec->dev->parent);

	switch (config_type) {
	case AFE_SLIMBUS_SLAVE_CONFIG:
		return &priv->slimbus_slave_cfg;
	case AFE_CDC_REGISTERS_CONFIG:
		return &taiko_audio_reg_cfg;
	case AFE_SLIMBUS_SLAVE_PORT_CONFIG:
		return &taiko_slimbus_slave_port_cfg;
	case AFE_AANC_VERSION:
		return &taiko_cdc_aanc_version;
	case AFE_CLIP_BANK_SEL:
		return &clip_bank_sel;
	case AFE_CDC_CLIP_REGISTERS_CONFIG:
		return &taiko_clip_reg_cfg;
	default:
		pr_debug("%s: Unknown config_type 0x%x\n", __func__, config_type);
		return NULL;
	}
}

static struct wcd9xxx_reg_address taiko_reg_address = {
	.micb_4_mbhc = TAIKO_A_MICB_4_MBHC,
	.micb_4_int_rbias = TAIKO_A_MICB_4_INT_RBIAS,
	.micb_4_ctl = TAIKO_A_MICB_4_CTL,
};

static int wcd9xxx_ssr_register(struct wcd9xxx *control,
				int (*device_down_cb)(struct wcd9xxx *wcd9xxx),
				int (*device_up_cb)(struct wcd9xxx *wcd9xxx),
				void *priv)
{
	control->dev_down = device_down_cb;
	control->post_reset = device_up_cb;
	control->ssr_priv = priv;
	return 0;
}

static const struct snd_soc_dapm_widget taiko_1_dapm_widgets[] = {
	SND_SOC_DAPM_ADC_E("ADC1", NULL, TAIKO_A_TX_1_2_EN, 7, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_PRE_PMU |
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC2", NULL, TAIKO_A_TX_1_2_EN, 3, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_PRE_PMU |
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC3", NULL, TAIKO_A_TX_3_4_EN, 7, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			   SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC4", NULL, TAIKO_A_TX_3_4_EN, 3, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			   SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC5", NULL, TAIKO_A_TX_5_6_EN, 7, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_ADC_E("ADC6", NULL, TAIKO_A_TX_5_6_EN, 3, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_POST_PMU),
};

static const struct snd_soc_dapm_widget taiko_2_dapm_widgets[] = {
	SND_SOC_DAPM_ADC_E("ADC1", NULL, TAIKO_A_CDC_TX_1_GAIN, 7, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_PRE_PMU |
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC2", NULL, TAIKO_A_CDC_TX_2_GAIN, 7, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_PRE_PMU |
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC3", NULL, TAIKO_A_CDC_TX_3_GAIN, 7, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			   SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC4", NULL, TAIKO_A_CDC_TX_4_GAIN, 7, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			   SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC5", NULL, TAIKO_A_CDC_TX_5_GAIN, 7, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_ADC_E("ADC6", NULL, TAIKO_A_CDC_TX_6_GAIN, 7, 0,
			   taiko_codec_enable_adc,
			   SND_SOC_DAPM_POST_PMU),
};

static struct regulator *taiko_codec_find_regulator(struct snd_soc_codec *codec,
						    const char *name)
{
	int i;
	struct wcd9xxx *core = dev_get_drvdata(codec->dev->parent);

	for (i = 0; i < core->num_of_supplies; i++) {
		if (core->supplies[i].supply &&
		    !strcmp(core->supplies[i].supply, name))
			return core->supplies[i].consumer;
	}

	return NULL;
}

static int show_sound_value(unsigned int inputval)
{
	int tempval;

	tempval = (int)regread(inputval);

	if ((tempval > 171) && (tempval < 256))
		tempval -= 256;

	return tempval;
}

/*
COMP1_B1_CTL 0x370
COMP1_B2_CTL 0x371
COMP1_B3_CTL 0x372
COMP1_B4_CTL 0x373
COMP1_B5_CTL 0x374
COMP1_B6_CTL 0x375
COMP1_SHUT_DOWN_STATUS 0x376
*/

static ssize_t headphone_dac_enabled_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
	u8 hphl_reg_val = 0;
	u8 hphr_reg_val = 0;
	hphl_reg_val = regread(WCD9XXX_A_RX_HPH_L_DAC_CTL);
	hphr_reg_val = regread(WCD9XXX_A_RX_HPH_R_DAC_CTL);
	return sprintf(buf, "Left:%s Right:%s\n", (hphl_reg_val & 0xC0) ? "On" : "Off",
                                              (hphr_reg_val & 0xC0) ? "On" : "Off");
}

/*
enum {
	SEC_JACK_NO_DEVICE				= 0,
	SEC_HEADSET_4POLE				= 1,
	SEC_HEADSET_3POLE				= 2,
};
*/
#ifdef CONFIG_SAMSUNG_JACK_SYSFS
static ssize_t secjack_state_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    if (secjack_state == 0)
    	return sprintf(buf, "%s\n", "Unplugged");
    else if (secjack_state > 0)
    	return sprintf(buf, "%s\n", "Headphones Inserted");
    else
        return sprintf(buf, "%s\n", "ERROR");
}
#endif
static ssize_t compander1_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s%d\n%s%d\n%s%d\n%s%d\n%s%d\n%s%d\n%s%d\n",
						"B1_CTL:", regread(TAIKO_A_CDC_COMP1_B1_CTL),
						"B2_CTL:", regread(TAIKO_A_CDC_COMP1_B2_CTL),
						"B3_CTL:", regread(TAIKO_A_CDC_COMP1_B3_CTL),
						"B4_CTL:", regread(TAIKO_A_CDC_COMP1_B4_CTL),
						"B5_CTL:", regread(TAIKO_A_CDC_COMP1_B5_CTL),
						"B6_CTL:", regread(TAIKO_A_CDC_COMP1_B6_CTL),
						"CNP_DIS_STATUS:", regread(TAIKO_A_CDC_COMP1_SHUT_DOWN_STATUS));
}

static ssize_t compander_enabled_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    unsigned int comp_disabled;

    comp_disabled = regread(TAIKO_A_CDC_COMP1_SHUT_DOWN_STATUS);
    if (comp_disabled > 0)
    	return sprintf(buf, "%s\n", "Compander:Disabled");
    else if (!comp_disabled)
    	return sprintf(buf, "%s\n", "Compander:Enabled");
    else
        return sprintf(buf, "%s\n", "Compander:ERROR");
}

static ssize_t hph_status_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "PA Left Status:%s\nPA Right Status:%s\n",
                   (regread(TAIKO_A_RX_HPH_L_STATUS) == PA_STAT_ON ? "On" : "Off"),
                   (regread(TAIKO_A_RX_HPH_R_STATUS) == PA_STAT_ON ? "On" : "Off"));
}

static ssize_t headphone_audio_active_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
    unsigned int headphone_audio_active = 0;

    if (hpwidget_any())
        headphone_audio_active = 1;

    return sprintf(buf, "%d\n", headphone_audio_active);
}

static ssize_t class_h_control_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "Class H Control:%d\n",
                   regread(TAIKO_A_CDC_CONN_CLSH_CTL));
}

static ssize_t chopper_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "Chopper:%d\n", regread(TAIKO_A_RX_HPH_CHOP_CTL));
}

static ssize_t autochopper_raw_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "Autochopper:%d\n", regread(TAIKO_A_RX_HPH_AUTO_CHOP));
}

#ifdef CONFIG_WAVEGEN_OVERRIDE
static ssize_t wavegen_override_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", wavegen_override);
}

static ssize_t wavegen_override_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

	wavegen_override = uval;
    if (hpwidget()) {
        update_wavegen();
    }
	return count;
}
#endif

static ssize_t autochopper_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hph_autochopper);
}

static ssize_t autochopper_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

	hph_autochopper = uval;
	write_autochopper(hph_autochopper);
	return count;
}

static ssize_t chopper_bypass_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", chopper_bypass);
}

static ssize_t chopper_bypass_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

	chopper_bypass = uval;
	write_chopper();
	return count;
}

/*
		snd_soc_update_bits(codec, TABLA_A_CDC_RX1_B6_CTL,
				    0x02, gain_offset.half_db_gain);
TABLA_A_CDC_RX1_B6_CTL

static ssize_t hph_halfdb_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
}
*/
static ssize_t headphone_gain_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int leftval, rightval, templeft, tempright;

	leftval = show_sound_value(TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL);
	if (leftval == -84) {
		templeft = hphl_cached_gain;

		if ((templeft > 171) && (templeft < 256))
			templeft -= 256;
	} else {
		templeft = leftval;
	}

	rightval = show_sound_value(TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL);
	if (rightval == -84) {
		tempright = hphr_cached_gain;

		if ((tempright > 171) && (tempright < 256))
			tempright -= 256;
	} else {
		tempright = rightval;
	}

	return sprintf(buf, "%d %d\n", templeft, tempright);
}

static ssize_t headphone_gain_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{

	int leftinput, rightinput, dualinput;

	if (sscanf(buf, "%d %d", &leftinput, &rightinput) == 2) {
		leftinput = clamp_val(leftinput, -84, 40);
		rightinput = clamp_val(rightinput, -84, 40);
		if (leftinput < 0)
			hphl_cached_gain = (leftinput + 256);
		else
			hphl_cached_gain = leftinput;
		if (rightinput < 0)
			hphr_cached_gain = (rightinput + 256);
		else
			hphr_cached_gain = rightinput;
	} else if (sscanf(buf, "%d", &dualinput) == 1) {
			dualinput = clamp_val(dualinput, -84, 40);

		if (dualinput < 0) {
			hphl_cached_gain = (dualinput + 256);
			hphr_cached_gain = (dualinput + 256);
		} else {
			hphl_cached_gain = dualinput;
			hphr_cached_gain = dualinput;
		}
	} else {
		return -EINVAL;
	}

	update_headphone_gain();
    
	return count;
}

static ssize_t headphone_mute_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", headphone_mute);
}

static ssize_t headphone_mute_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int hpm;

	sscanf(buf, "%d", &hpm);

	hpm = clamp_val(hpm, 0, 1);
	headphone_mute = hpm;
	update_headphone_gain();

	return count;
}

/*
	SOC_SINGLE_S8_TLV("IIR1 INP1 Volume", TAIKO_A_CDC_IIR1_GAIN_B1_CTL, -84,
		40, digital_gain),
	SOC_SINGLE_S8_TLV("IIR2 INP1 Volume", TAIKO_A_CDC_IIR2_GAIN_B1_CTL, -84,
		40, digital_gain),
*/

#if 0
static ssize_t iir1_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int iirval, tempval;

	iirval = show_sound_value(TAIKO_A_CDC_IIR1_GAIN_B1_CTL);
	if (iirval == -84) {
		tempval = iir1_cached_gain;

		if ((tempval > 171) && (tempval < 256))
			tempval -= 256;
	} else {
		tempval = iirval;
	}

	return sprintf(buf, "%d\n", tempval);
}

static ssize_t iir1_gain_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int iirinput;

	sscanf(buf, "%d", &iirinput);

	iirinput = clamp_val(iirinput, -84, 40);

	if (iirinput < 0)
		iir1_cached_gain = (iirinput + 256);
	else
		iir1_cached_gain = iirinput;

	update_iir_gain();

	return count;
}
#endif
#if 0
static ssize_t iir1_inp2_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int iirval, tempval;

	iirval = show_sound_value(TAIKO_A_CDC_IIR1_GAIN_B2_CTL);
	if (iirval == -84) {
		tempval = iir1_inp2_cached_gain;

		if ((tempval > 171) && (tempval < 256))
			tempval -= 256;
	} else {
		tempval = iirval;
	}

	return sprintf(buf, "%d\n", tempval);
}

static ssize_t iir1_inp2_gain_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int iirinput;

	sscanf(buf, "%d", &iirinput);

	iirinput = clamp_val(iirinput, -84, 40);

	if (iirinput < 0)
		iir1_inp2_cached_gain = (iirinput + 256);
	else
		iir1_inp2_cached_gain = iirinput;

	update_iir_gain();

	return count;
}

static ssize_t iir2_inp2_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int iirval, tempval;

	iirval = show_sound_value(TAIKO_A_CDC_IIR2_GAIN_B2_CTL);
	if (iirval == -84) {
		tempval = iir2_inp2_cached_gain;

		if ((tempval > 171) && (tempval < 256))
			tempval -= 256;
	} else {
		tempval = iirval;
	}

	return sprintf(buf, "%d\n", tempval);
}

static ssize_t iir2_inp2_gain_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int iirinput;

	sscanf(buf, "%d", &iirinput);

	iirinput = clamp_val(iirinput, -84, 40);

	if (iirinput < 0)
		iir2_inp2_cached_gain = (iirinput + 256);
	else
		iir2_inp2_cached_gain = iirinput;

	update_iir_gain();

	return count;
}

static ssize_t iir2_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int iirval, tempval;

	iirval = show_sound_value(TAIKO_A_CDC_IIR2_GAIN_B1_CTL);
	if (iirval == -84) {
		tempval = iir2_cached_gain;

		if ((tempval > 171) && (tempval < 256))
			tempval -= 256;
	} else {
		tempval = iirval;
	}

	return sprintf(buf, "%d\n", tempval);
}

static ssize_t iir2_gain_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int iirinput;

	sscanf(buf, "%d", &iirinput);

	iirinput = clamp_val(iirinput, -84, 40);

	if (iirinput < 0)
		iir2_cached_gain = (iirinput + 256);
	else
		iir2_cached_gain = iirinput;

	update_iir_gain();

	return count;
}
#endif //0

static ssize_t hph_pa_enabled_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", hph_pa_enabled);
}

static ssize_t hph_pa_enabled_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

    if (hpwidget_any())
        return count;

	hph_pa_enabled = uval;
	return count;
}

static ssize_t hph_poweramp_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int leftval, rightval;

   	leftval = read_hph_poweramp_gain(WCD9XXX_A_RX_HPH_L_GAIN);
    rightval = read_hph_poweramp_gain(WCD9XXX_A_RX_HPH_R_GAIN);

	return sprintf(buf, "%d %d\n", leftval, rightval);
}

static ssize_t hph_poweramp_gain_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{

	int leftinput;
	int rightinput;
	int dualinput;

	if (sscanf(buf, "%d %d", &leftinput, &rightinput) == 2) {
		if (leftinput < 0)
			leftinput = 0;
		if (leftinput > 20)
			leftinput = 20;
		if (rightinput < 0)
			rightinput = 0;
		if (rightinput > 20)
			rightinput = 20;
		hphl_pa_cached_gain = leftinput;
		hphr_pa_cached_gain = rightinput;
	} else if (sscanf(buf, "%d", &dualinput) == 1) {
		if (dualinput < 0)
			dualinput = 0;
		if (dualinput > 20)
			dualinput = 20;
		hphl_pa_cached_gain = dualinput;
		hphr_pa_cached_gain = dualinput;
	}

	if (hpwidget())
		write_hph_poweramp_regs();

	return count;
}

static ssize_t speaker_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int spkval, tempval;

	spkval = show_sound_value(TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL);
	if (spkval == -84) {
		tempval = speaker_cached_gain;

		if ((tempval > 171) && (tempval < 256))
			tempval -= 256;
	} else {
		tempval = spkval;
	}

	return sprintf(buf, "%d\n", tempval);
}

static ssize_t speaker_gain_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int spkinput;

	sscanf(buf, "%d", &spkinput);

	spkinput = clamp_val(spkinput, -84, 40);

	if (spkinput < 0)
		speaker_cached_gain = (spkinput + 256);
	else
		speaker_cached_gain = spkinput;

	update_speaker_gain();

	return count;
}

static ssize_t speaker_mute_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", speaker_mute);
}

static ssize_t speaker_mute_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int spkm;

	sscanf(buf, "%d", &spkm);

	spkm = clamp_val(spkm, 0, 1);
	speaker_mute = spkm;
	update_speaker_gain();

	return count;
}

static ssize_t uhqa_mode_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", uhqa_mode);
}

static ssize_t uhqa_mode_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

	uhqa_mode = uval;
    write_chopper();
	return count;
}

static ssize_t high_perf_mode_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", high_perf_mode);
}

static ssize_t high_perf_mode_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

    if (hpwidget_any())
        return count;

	high_perf_mode = uval;
	return count;
}

static ssize_t interpolator_boost_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", interpolator_boost);
}

static ssize_t interpolator_boost_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

    if (hpwidget_any())
        return count;

	interpolator_boost = uval;
	return count;
}

static ssize_t compander_gain_lock_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", compander_gain_lock);
}

static ssize_t compander_gain_lock_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

    if (hpwidget_any())
        return count;

	compander_gain_lock = uval;
	return count;
}

static ssize_t compander_gain_boost_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", compander_gain_boost);
}

static ssize_t compander_gain_boost_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

    if (hpwidget_any())
        return count;

	compander_gain_boost = uval;
	return count;
}

static ssize_t headphone_hdc_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", headphone_hdc);
}

static ssize_t headphone_hdc_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

	headphone_hdc = uval;
    write_hdc_dual(headphone_hdc);

	return count;
}

static ssize_t speaker_hdc_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", speaker_hdc);
}

static ssize_t speaker_hdc_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

	speaker_hdc = uval;
    write_speaker_hdc(speaker_hdc);

	return count;
}

/*		TAIKO_A_CDC_RX1_B4_CTL
		TAIKO_A_CDC_RX2_B4_CTL
		hphl_hpf_cutoff
		hphr_hpf_cutoff
*/

static ssize_t headphone_left_hpf_cutoff_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int leftval;

	leftval = read_hpf_cutoff(TAIKO_A_CDC_RX1_B4_CTL);
	return sprintf(buf, "%d\n", leftval);
}

static ssize_t headphone_left_hpf_cutoff_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

    sscanf(buf, "%d", &input);

	if (input < 0)
		input = 0;
	if (input > 2)
		input = 2;

	hphl_hpf_cutoff = input;
	write_hpf_cutoff(TAIKO_A_CDC_RX1_B4_CTL);

	return count;
}

static ssize_t headphone_right_hpf_cutoff_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int rightval;

	rightval = read_hpf_cutoff(TAIKO_A_CDC_RX2_B4_CTL);

	return sprintf(buf, "%d\n", rightval);
}

static ssize_t headphone_right_hpf_cutoff_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

    sscanf(buf, "%d", &input);
	if (input < 0)
		input = 0;
	if (input > 2)
		input = 2;

	hphr_hpf_cutoff = input;
	write_hpf_cutoff(TAIKO_A_CDC_RX2_B4_CTL);

	return count;
}

/*
		TAIKO_A_CDC_RX7_B4_CTL
		speaker_hpf_cutoff
*/

static ssize_t speaker_hpf_cutoff_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", read_hpf_cutoff(TAIKO_A_CDC_RX7_B4_CTL));
}

static ssize_t speaker_hpf_cutoff_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 2)
		uval = 2;

	speaker_hpf_cutoff = uval;

	write_hpf_cutoff(TAIKO_A_CDC_RX7_B4_CTL);

	return count;
}

/*
	__________________________Bypass Switch_______________________

	SOC_SINGLE(xname, reg, shift, max, invert)
	SOC_SINGLE("RX1 HPF Switch", TAIKO_A_CDC_RX1_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX2 HPF Switch", TAIKO_A_CDC_RX2_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX7 HPF Switch", TAIKO_A_CDC_RX7_B5_CTL, 2, 1, 0),

	#define SOC_SINGLE(xname, reg, shift, max, invert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw,\
	.put = snd_soc_put_volsw, \
	.private_value =  SOC_SINGLE_VALUE(reg, shift, max, invert) }

	static u8 hphl_hpf_bypass;
	static u8 hphr_hpf_bypass;
	static u8 speaker_hpf_bypass;
	______________________________________________________________
*/

static ssize_t headphone_left_hpf_bypass_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int leftval;

	leftval = read_hpf_bypass(TAIKO_A_CDC_RX1_B5_CTL);

	return sprintf(buf, "%d\n", leftval);
}

static ssize_t headphone_left_hpf_bypass_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

    sscanf(buf, "%d", &input);
	if (input < 0)
		input = 0;
	if (input > 1)
		input = 1;
	hphl_hpf_bypass = input;
	write_hpf_bypass(TAIKO_A_CDC_RX1_B5_CTL);

	return count;
}

static ssize_t headphone_right_hpf_bypass_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int rightval;

	rightval = read_hpf_bypass(TAIKO_A_CDC_RX2_B5_CTL);

	return sprintf(buf, "%d\n", rightval);
}

static ssize_t headphone_right_hpf_bypass_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

    sscanf(buf, "%d", &input);
	if (input < 0)
		input = 0;
	if (input > 1)
		input = 1;
	hphr_hpf_bypass = input;
	write_hpf_bypass(TAIKO_A_CDC_RX2_B5_CTL);

	return count;
}

static ssize_t speaker_hpf_bypass_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", read_hpf_bypass(TAIKO_A_CDC_RX7_B5_CTL));
}

static ssize_t speaker_hpf_bypass_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

	speaker_hpf_bypass = uval;

	write_hpf_bypass(TAIKO_A_CDC_RX7_B5_CTL);

	return count;
}

static ssize_t peak_det_timeout_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sc_peak_det_timeout);
}

static ssize_t peak_det_timeout_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 15)
		uval = 15;

    if (hpwidget_any())
        return count;

	sc_peak_det_timeout = (u32)uval;
    update_interpolator();

	return count;
}

static ssize_t rms_meter_div_fact_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sc_rms_meter_div_fact);
}

static ssize_t rms_meter_div_fact_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 15)
		uval = 15;

    if (hpwidget_any())
        return count;

	sc_rms_meter_div_fact = (u32)uval;
    update_interpolator();

	return count;
}

static ssize_t rms_meter_resamp_fact_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%u\n", sc_rms_meter_resamp_fact);
}

static ssize_t rms_meter_resamp_fact_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 255)
		uval = 255;

    if (hpwidget_any())
        return count;

   	sc_rms_meter_resamp_fact = (u32)uval;
    update_interpolator();

	return count;
}

#ifdef CONFIG_WCD_BIAS_ACCESS
static ssize_t hph_pa_bias_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", regread(TAIKO_A_RX_HPH_BIAS_PA));
}

static ssize_t hph_pa_bias_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 85)
		uval = 85;
	if (uval > 170)
		uval = 170;
#ifdef CONFIG_SAMSUNG_JACK_SYSFS
    if (sec_jacked() || hpwidget_any() || \
        spkwidget_active())
#else
    if (hpwidget_any() || spkwidget_active())
#endif
        return count;

	hph_pa_bias = uval;

    update_bias();

	return count;
}

static ssize_t cnp_bias_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", regread(TAIKO_A_RX_HPH_BIAS_CNP));
}

static ssize_t cnp_bias_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 85)
		uval = 85;
	if (uval > 170)
		uval = 170;
#ifdef CONFIG_SAMSUNG_JACK_SYSFS
    if (sec_jacked() || hpwidget_any() || \
        spkwidget_active())
#else
    if (hpwidget_any() || spkwidget_active())
#endif
        return count;

	cnp_bias = uval;

    update_bias();

	return count;
}
#endif
static ssize_t anc_delay_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", anc_delay);
}

static ssize_t anc_delay_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < 0)
		uval = 0;
	if (uval > 1)
		uval = 1;

	anc_delay = uval;
	return count;
}
# if 0
static ssize_t mx_hw_eq_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", mx_hw_eq);
}

static ssize_t mx_hw_eq_store(struct kobject *kobj,
			   struct kobj_attribute *attr, const char *buf, size_t count)
{
	int uval;

	sscanf(buf, "%d", &uval);

	if (uval < HWEQ_OFF)
		uval = HWEQ_OFF;
	if (uval > HWEQ_ON)
		uval = HWEQ_ON;

    if (hpwidget_any() ||
        spkwidget_active())
        return count;

	mx_hw_eq = uval;

	return count;
}
#endif
static struct kobj_attribute headphone_dac_enabled_attribute =
	__ATTR(headphone_dac_enabled, 0444,
		headphone_dac_enabled_show,
		NULL);
#ifdef CONFIG_SAMSUNG_JACK_SYSFS
static struct kobj_attribute secjack_state_attribute =
	__ATTR(secjack_state, 0444,
		secjack_state_show,
		NULL);
#endif
static struct kobj_attribute compander1_attribute =
	__ATTR(compander1, 0444,
		compander1_show,
		NULL);

static struct kobj_attribute compander_enabled_attribute =
	__ATTR(compander_enabled, 0444,
		compander_enabled_show,
		NULL);

static struct kobj_attribute hph_status_attribute =
	__ATTR(hph_status, 0444,
		hph_status_show,
		NULL);

static struct kobj_attribute headphone_audio_active_attribute =
	__ATTR(headphone_audio_active, 0444,
		headphone_audio_active_show,
		NULL);

static struct kobj_attribute class_h_control_attribute =
	__ATTR(class_h_control, 0444,
		class_h_control_show,
		NULL);

static struct kobj_attribute chopper_attribute =
	__ATTR(chopper, 0644,
		chopper_show,
		NULL);

static struct kobj_attribute autochopper_raw_attribute =
	__ATTR(autochopper_raw, 0644,
		autochopper_raw_show,
		NULL);
#ifdef CONFIG_WAVEGEN_OVERRIDE
static struct kobj_attribute wavegen_override_attribute =
	__ATTR(wavegen_override, 0644,
		wavegen_override_show,
		wavegen_override_store);
#endif
static struct kobj_attribute autochopper_attribute =
	__ATTR(autochopper, 0644,
		autochopper_show,
		autochopper_store);

static struct kobj_attribute chopper_bypass_attribute =
	__ATTR(chopper_bypass, 0644,
		chopper_bypass_show,
		chopper_bypass_store);

static struct kobj_attribute headphone_gain_attribute =
	__ATTR(headphone_gain, 0644,
		headphone_gain_show,
		headphone_gain_store);

static struct kobj_attribute headphone_mute_attribute =
	__ATTR(headphone_mute, 0644,
		headphone_mute_show,
		headphone_mute_store);

#if 0
static struct kobj_attribute crossfeed_gain_attribute =
	__ATTR(crossfeed_gain, 0644,
		crossfeed_gain_show,
		crossfeed_gain_store);
#endif

static struct kobj_attribute hph_poweramp_gain_attribute =
	__ATTR(hph_poweramp_gain, 0644,
		hph_poweramp_gain_show,
		hph_poweramp_gain_store);

static struct kobj_attribute speaker_gain_attribute =
	__ATTR(speaker_gain, 0644,
		speaker_gain_show,
		speaker_gain_store);

static struct kobj_attribute speaker_mute_attribute =
	__ATTR(speaker_mute, 0644,
		speaker_mute_show,
		speaker_mute_store);

#if 0
static struct kobj_attribute iir1_gain_attribute =
	__ATTR(iir1_gain, 0644,
		iir1_gain_show,
		iir1_gain_store);

static struct kobj_attribute iir2_gain_attribute =
	__ATTR(iir2_gain, 0644,
		iir2_gain_show,
		iir2_gain_store);

static struct kobj_attribute iir1_inp2_gain_attribute =
	__ATTR(iir1_inp2_gain, 0644,
		iir1_inp2_gain_show,
		iir1_inp2_gain_store);

static struct kobj_attribute iir2_inp2_gain_attribute =
	__ATTR(iir2_inp2_gain, 0644,
		iir2_inp2_gain_show,
		iir2_inp2_gain_store);
#endif //0

static struct kobj_attribute uhqa_mode_attribute =
	__ATTR(uhqa_mode, 0644,
		uhqa_mode_show,
		uhqa_mode_store);

static struct kobj_attribute high_perf_mode_attribute =
	__ATTR(high_perf_mode, 0644,
		high_perf_mode_show,
		high_perf_mode_store);

static struct kobj_attribute interpolator_boost_attribute =
	__ATTR(interpolator_boost, 0644,
		interpolator_boost_show,
		interpolator_boost_store);

static struct kobj_attribute hph_pa_enabled_attribute =
	__ATTR(hph_pa_enabled, 0644,
		hph_pa_enabled_show,
		hph_pa_enabled_store);

static struct kobj_attribute compander_gain_lock_attribute =
	__ATTR(compander_gain_lock, 0644,
		compander_gain_lock_show,
		compander_gain_lock_store);

static struct kobj_attribute compander_gain_boost_attribute =
	__ATTR(compander_gain_boost, 0644,
		compander_gain_boost_show,
		compander_gain_boost_store);

static struct kobj_attribute headphone_hdc_attribute =
	__ATTR(headphone_hdc, 0644,
		headphone_hdc_show,
		headphone_hdc_store);

static struct kobj_attribute speaker_hdc_attribute =
	__ATTR(speaker_hdc, 0644,
		speaker_hdc_show,
		speaker_hdc_store);

static struct kobj_attribute anc_delay_attribute =
	__ATTR(anc_delay, 0644,
		anc_delay_show,
		anc_delay_store);

static struct kobj_attribute headphone_left_hpf_cutoff_attribute =
	__ATTR(headphone_left_hpf_cutoff, 0644,
		headphone_left_hpf_cutoff_show,
		headphone_left_hpf_cutoff_store);

static struct kobj_attribute headphone_right_hpf_cutoff_attribute =
	__ATTR(headphone_right_hpf_cutoff, 0644,
		headphone_right_hpf_cutoff_show,
		headphone_right_hpf_cutoff_store);

static struct kobj_attribute speaker_hpf_cutoff_attribute =
	__ATTR(speaker_hpf_cutoff, 0644,
		speaker_hpf_cutoff_show,
		speaker_hpf_cutoff_store);

static struct kobj_attribute headphone_left_hpf_bypass_attribute =
	__ATTR(headphone_left_hpf_bypass, 0644,
		headphone_left_hpf_bypass_show,
		headphone_left_hpf_bypass_store);

static struct kobj_attribute headphone_right_hpf_bypass_attribute =
	__ATTR(headphone_right_hpf_bypass, 0644,
		headphone_right_hpf_bypass_show,
		headphone_right_hpf_bypass_store);

static struct kobj_attribute speaker_hpf_bypass_attribute =
	__ATTR(speaker_hpf_bypass, 0644,
		speaker_hpf_bypass_show,
		speaker_hpf_bypass_store);

static struct kobj_attribute peak_det_timeout_attribute =
	__ATTR(peak_det_timeout, 0644,
		peak_det_timeout_show,
		peak_det_timeout_store);

static struct kobj_attribute rms_meter_div_fact_attribute =
	__ATTR(rms_meter_div_fact, 0644,
		rms_meter_div_fact_show,
		rms_meter_div_fact_store);

static struct kobj_attribute rms_meter_resamp_fact_attribute =
	__ATTR(rms_meter_resamp_fact, 0644,
		rms_meter_resamp_fact_show,
		rms_meter_resamp_fact_store);

#ifdef CONFIG_WCD_BIAS_ACCESS
static struct kobj_attribute hph_pa_bias_attribute =
	__ATTR(hph_pa_bias, 0644,
		hph_pa_bias_show,
		hph_pa_bias_store);

static struct kobj_attribute cnp_bias_attribute =
	__ATTR(cnp_bias, 0644,
		cnp_bias_show,
		cnp_bias_store);
#endif
#if 0
static struct kobj_attribute mx_hw_eq_attribute =
	__ATTR(mx_hw_eq, 0644,
		mx_hw_eq_show,
		mx_hw_eq_store);
#endif
static struct attribute *sound_control_attrs[] = {
        &headphone_dac_enabled_attribute.attr,
#ifdef CONFIG_SAMSUNG_JACK_SYSFS
        &secjack_state_attribute.attr,
#endif
		&compander1_attribute.attr,
		&compander_enabled_attribute.attr,
		&hph_status_attribute.attr,
		&headphone_audio_active_attribute.attr,
        &class_h_control_attribute.attr,
		&chopper_attribute.attr,
		&autochopper_raw_attribute.attr,
#ifdef CONFIG_WAVEGEN_OVERRIDE
        &wavegen_override_attribute.attr,
#endif
		&autochopper_attribute.attr,
		&chopper_bypass_attribute.attr,
		&headphone_gain_attribute.attr,
		&headphone_mute_attribute.attr,
		&hph_poweramp_gain_attribute.attr,
		&speaker_gain_attribute.attr,
		&speaker_mute_attribute.attr,
#if 0
		&iir1_gain_attribute.attr,
		&iir2_gain_attribute.attr,
#endif
		&uhqa_mode_attribute.attr,
		&high_perf_mode_attribute.attr,
		&interpolator_boost_attribute.attr,
		&hph_pa_enabled_attribute.attr,
		&compander_gain_lock_attribute.attr,
		&compander_gain_boost_attribute.attr,
		&headphone_hdc_attribute.attr,
		&speaker_hdc_attribute.attr,
		&anc_delay_attribute.attr,
		&headphone_left_hpf_cutoff_attribute.attr,
		&headphone_right_hpf_cutoff_attribute.attr,
		&speaker_hpf_cutoff_attribute.attr,
		&headphone_left_hpf_bypass_attribute.attr,
		&headphone_right_hpf_bypass_attribute.attr,
		&speaker_hpf_bypass_attribute.attr,
		&peak_det_timeout_attribute.attr,
		&rms_meter_div_fact_attribute.attr,
		&rms_meter_resamp_fact_attribute.attr,
#ifdef CONFIG_WCD_BIAS_ACCESS
		&hph_pa_bias_attribute.attr,
		&cnp_bias_attribute.attr,
#endif
#if 0
		&mx_hw_eq_attribute.attr,
#endif
		NULL,
};

static struct attribute_group sound_control_attr_group = {
		.attrs = sound_control_attrs,
};

static struct kobject *sound_control_kobj;

static int taiko_codec_probe(struct snd_soc_codec *codec)
{
	struct wcd9xxx *control;
	struct taiko_priv *taiko;
	struct wcd9xxx_pdata *pdata;
	struct wcd9xxx *wcd9xxx;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	int ret = 0;
	int i, rco_clk_rate;
	void *ptr = NULL;
	struct wcd9xxx *core = dev_get_drvdata(codec->dev->parent);
	struct wcd9xxx_core_resource *core_res;

	codec->control_data = dev_get_drvdata(codec->dev->parent);
	control = codec->control_data;

	wcd9xxx_ssr_register(control, taiko_device_down,
			     taiko_post_reset_cb, (void *)codec);

	dev_dbg(codec->dev, "%s()\n", __func__);

	taiko = kzalloc(sizeof(struct taiko_priv), GFP_KERNEL);
	if (!taiko) {
		dev_dbg(codec->dev, "Failed to allocate private data\n");
		return -ENOMEM;
	}
	for (i = 0 ; i < NUM_DECIMATORS; i++) {
		tx_hpf_work[i].taiko = taiko;
		tx_hpf_work[i].decimator = i + 1;
		INIT_DELAYED_WORK(&tx_hpf_work[i].dwork,
			tx_hpf_corner_freq_callback);
	}

	snd_soc_codec_set_drvdata(codec, taiko);

	/* codec resmgr module init */
	wcd9xxx = codec->control_data;
	core_res = &wcd9xxx->core_res;
	pdata = dev_get_platdata(codec->dev->parent);
	ret = wcd9xxx_resmgr_init(&taiko->resmgr, codec, core_res, pdata,
				  &pdata->micbias, &taiko_reg_address,
				  WCD9XXX_CDC_TYPE_TAIKO);
	if (ret) {
		pr_debug("%s: wcd9xxx init failed %d\n", __func__, ret);
		goto err_init;
	}

	taiko->clsh_d.buck_mv = taiko_codec_get_buck_mv(codec);
	/* Taiko does not support dynamic switching of vdd_cp */
	taiko->clsh_d.is_dynamic_vdd_cp = false;
	wcd9xxx_clsh_init(&taiko->clsh_d, &taiko->resmgr);

	rco_clk_rate = TAIKO_MCLK_CLK_9P6MHZ;
#if !defined(CONFIG_SAMSUNG_JACK) && !defined(CONFIG_MUIC_DET_JACK)
	/* init and start mbhc */
	ret = wcd9xxx_mbhc_init(&taiko->mbhc, &taiko->resmgr, codec,
				taiko_enable_mbhc_micbias,
				&mbhc_cb, &cdc_intr_ids,
				rco_clk_rate, false);
	if (ret) {
		pr_debug("%s: mbhc init failed %d\n", __func__, ret);
		goto err_init;
	}
#endif

	taiko->codec = codec;
	taiko->comp_enabled[0] = 0;
	taiko->comp_fs[0] = COMPANDER_FS_48KHZ;
	taiko->comp_enabled[1] = 0;
	taiko->comp_fs[1] = COMPANDER_FS_48KHZ;
	taiko->comp_enabled[2] = 0;
	taiko->comp_fs[2] = COMPANDER_FS_48KHZ;
	taiko->intf_type = wcd9xxx_get_intf_type();
	taiko->aux_pga_cnt = 0;
	taiko->aux_l_gain = 0x1F;
	taiko->aux_r_gain = 0x1F;
	taiko->ldo_h_users = 0;
	taiko->micb_2_users = 0;
	taiko_update_reg_defaults(codec);
	pr_debug("%s: MCLK Rate = %x\n", __func__, wcd9xxx->mclk_rate);
	if (wcd9xxx->mclk_rate == TAIKO_MCLK_CLK_12P288MHZ)
		snd_soc_update_bits(codec, TAIKO_A_CHIP_CTL, 0x06, 0x0);
	else if (wcd9xxx->mclk_rate == TAIKO_MCLK_CLK_9P6MHZ)
		snd_soc_update_bits(codec, TAIKO_A_CHIP_CTL, 0x06, 0x2);
	taiko_codec_init_reg(codec);
	ret = taiko_handle_pdata(taiko);
	if (IS_ERR_VALUE(ret)) {
		pr_debug("%s: bad pdata\n", __func__);
		goto err_pdata;
	}

	taiko->spkdrv_reg = taiko_codec_find_regulator(codec,
						       WCD9XXX_VDD_SPKDRV_NAME);

	if (spkr_drv_wrnd > 0) {
		WCD9XXX_BG_CLK_LOCK(&taiko->resmgr);
		wcd9xxx_resmgr_get_bandgap(&taiko->resmgr,
					   WCD9XXX_BANDGAP_AUDIO_MODE);
		WCD9XXX_BG_CLK_UNLOCK(&taiko->resmgr);
	}

#if defined(CONFIG_SND_SOC_ESXXX)
	remote_add_codec_controls(codec);
#elif defined(CONFIG_SND_SOC_ES325)
	es325_remote_add_codec_controls(codec);
#endif

	ptr = kmalloc((sizeof(taiko_rx_chs) +
		       sizeof(taiko_tx_chs)), GFP_KERNEL);
	if (!ptr) {
		pr_debug("%s: no mem for slim chan ctl data\n", __func__);
		ret = -ENOMEM;
		goto err_nomem_slimch;
	}

	if (taiko->intf_type == WCD9XXX_INTERFACE_TYPE_I2C) {
		snd_soc_dapm_new_controls(dapm, taiko_dapm_i2s_widgets,
			ARRAY_SIZE(taiko_dapm_i2s_widgets));
		snd_soc_dapm_add_routes(dapm, audio_i2s_map,
			ARRAY_SIZE(audio_i2s_map));
			snd_soc_dapm_add_routes(dapm, audio_i2s_map_2_0,
						ARRAY_SIZE(audio_i2s_map_2_0));
		for (i = 0; i < ARRAY_SIZE(taiko_i2s_dai); i++)
			INIT_LIST_HEAD(&taiko->dai[i].wcd9xxx_ch_list);
	} else if (taiko->intf_type == WCD9XXX_INTERFACE_TYPE_SLIMBUS) {
		for (i = 0; i < NUM_CODEC_DAIS; i++) {
			INIT_LIST_HEAD(&taiko->dai[i].wcd9xxx_ch_list);
			init_waitqueue_head(&taiko->dai[i].dai_wait);
		}
		taiko_slimbus_slave_port_cfg.slave_dev_intfdev_la =
		    control->slim_slave->laddr;
		taiko_slimbus_slave_port_cfg.slave_dev_pgd_la =
		    control->slim->laddr;
		taiko_slimbus_slave_port_cfg.slave_port_mapping[0] =
		    TAIKO_MAD_SLIMBUS_TX_PORT;

		taiko_init_slim_slave_cfg(codec);
	}

	snd_soc_dapm_new_controls(dapm, taiko_2_dapm_widgets,
				  ARRAY_SIZE(taiko_2_dapm_widgets));
	snd_soc_add_codec_controls(codec,
		taiko_2_x_analog_gain_controls,
		ARRAY_SIZE(taiko_2_x_analog_gain_controls));

	snd_soc_add_codec_controls(codec, impedance_detect_controls,
				   ARRAY_SIZE(impedance_detect_controls));

	control->num_rx_port = TAIKO_RX_MAX;
	control->rx_chs = ptr;
	memcpy(control->rx_chs, taiko_rx_chs, sizeof(taiko_rx_chs));
	control->num_tx_port = TAIKO_TX_MAX;
	control->tx_chs = ptr + sizeof(taiko_rx_chs);
	memcpy(control->tx_chs, taiko_tx_chs, sizeof(taiko_tx_chs));

	snd_soc_dapm_sync(dapm);

	ret = taiko_setup_irqs(taiko);
	if (ret) {
		pr_debug("%s: taiko irq setup failed %d\n", __func__, ret);
		goto err_irq;
	}

	atomic_set(&kp_taiko_priv, (unsigned long)taiko);
	mutex_lock(&dapm->codec->mutex);
	snd_soc_dapm_disable_pin(dapm, "ANC HPHL");
	snd_soc_dapm_disable_pin(dapm, "ANC HPHR");
	snd_soc_dapm_disable_pin(dapm, "ANC HEADPHONE");
	snd_soc_dapm_disable_pin(dapm, "ANC EAR PA");
	snd_soc_dapm_disable_pin(dapm, "ANC EAR");
	mutex_unlock(&dapm->codec->mutex);
	snd_soc_dapm_sync(dapm);

	codec->ignore_pmdown_time = 1;
	pr_info("Sound Control: Taiko Sound Engine Probe\n");
   	sound_control_codec_ptr = codec->control_data;
   	direct_codec = codec;

	if (codec->name)
		pr_info("Sound Control: Probing Codec %s\n", codec->name);
	sound_control_kobj = kobject_create_and_add("sound_control", kernel_kobj);
	if (sound_control_kobj) {
		ret = sysfs_create_group(sound_control_kobj, &sound_control_attr_group);
        if (ret) {
			kobject_put(sound_control_kobj);
			pr_warn("%s sysfs file create failed!\n", __func__);
		}
	} else {
		pr_warn("%s kobject create failed!\n", __func__);
	}

    wake_lock_init(&spk_playback_wake_lock, WAKE_LOCK_SUSPEND, "mx_spk_playback");
    wake_lock_init(&hph_playback_wake_lock, WAKE_LOCK_SUSPEND, "mx_hph_playback");
    update_bias();
	update_control_regs();
	return ret;

err_irq:
	taiko_cleanup_irqs(taiko);
err_pdata:
	kfree(ptr);
err_nomem_slimch:
	kfree(taiko);
err_init:
	return ret;
}
static int taiko_codec_remove(struct snd_soc_codec *codec)
{
	struct taiko_priv *taiko = snd_soc_codec_get_drvdata(codec);

	WCD9XXX_BG_CLK_LOCK(&taiko->resmgr);
	atomic_set(&kp_taiko_priv, 0);

	if (spkr_drv_wrnd > 0)
		wcd9xxx_resmgr_put_bandgap(&taiko->resmgr,
					   WCD9XXX_BANDGAP_AUDIO_MODE);
	WCD9XXX_BG_CLK_UNLOCK(&taiko->resmgr);

	taiko_cleanup_irqs(taiko);
#if !defined(CONFIG_SAMSUNG_JACK) && !defined(CONFIG_MUIC_DET_JACK)
	/* cleanup MBHC */
	wcd9xxx_mbhc_deinit(&taiko->mbhc);
#endif
	/* cleanup resmgr */
	wcd9xxx_resmgr_deinit(&taiko->resmgr);

	taiko->spkdrv_reg = NULL;

	kfree(taiko);
    wake_lock_destroy(&spk_playback_wake_lock);
    wake_lock_destroy(&hph_playback_wake_lock);
	return 0;
}
static struct snd_soc_codec_driver soc_codec_dev_taiko = {
	.probe	= taiko_codec_probe,
	.remove	= taiko_codec_remove,

	.read = taiko_read,
	.write = taiko_write,

	.readable_register = taiko_readable,
	.volatile_register = taiko_volatile,

	.reg_cache_size = TAIKO_CACHE_SIZE,
	.reg_cache_default = taiko_reset_reg_defaults,
	.reg_word_size = 1,

	.controls = taiko_snd_controls,
	.num_controls = ARRAY_SIZE(taiko_snd_controls),
	.dapm_widgets = taiko_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(taiko_dapm_widgets),
	.dapm_routes = audio_map,
	.num_dapm_routes = ARRAY_SIZE(audio_map),
};

#ifdef CONFIG_PM
static int taiko_suspend(struct device *dev)
{
	dev_dbg(dev, "%s: system suspend\n", __func__);
	return 0;
}

static int taiko_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct taiko_priv *taiko = platform_get_drvdata(pdev);

	if (!taiko) {
		dev_dbg(dev, "%s: taiko private data is NULL\n", __func__);
		return -EINVAL;
	}
	dev_dbg(dev, "%s: system resume\n", __func__);
	/* Notify */
	wcd9xxx_resmgr_notifier_call(&taiko->resmgr, WCD9XXX_EVENT_POST_RESUME);
	return 0;
}

static const struct dev_pm_ops taiko_pm_ops = {
	.suspend	= taiko_suspend,
	.resume		= taiko_resume,
};
#endif

static int __devinit taiko_probe(struct platform_device *pdev)
{
	int ret = 0;
	if (wcd9xxx_get_intf_type() == WCD9XXX_INTERFACE_TYPE_SLIMBUS)
		ret = snd_soc_register_codec(&pdev->dev, &soc_codec_dev_taiko,
			taiko_dai, ARRAY_SIZE(taiko_dai));
	else if (wcd9xxx_get_intf_type() == WCD9XXX_INTERFACE_TYPE_I2C)
		ret = snd_soc_register_codec(&pdev->dev, &soc_codec_dev_taiko,
			taiko_i2s_dai, ARRAY_SIZE(taiko_i2s_dai));
	return ret;
}
static int __devexit taiko_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}
static struct platform_driver taiko_codec_driver = {
	.probe = taiko_probe,
	.remove = taiko_remove,
	.driver = {
		.name = "taiko_codec",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &taiko_pm_ops,
#endif
	},
};

static int __init taiko_codec_init(void)
{
	return platform_driver_register(&taiko_codec_driver);
}

static void __exit taiko_codec_exit(void)
{
	platform_driver_unregister(&taiko_codec_driver);
}

module_init(taiko_codec_init);
module_exit(taiko_codec_exit);

MODULE_DESCRIPTION("Taiko codec driver");
MODULE_LICENSE("GPL v2");
