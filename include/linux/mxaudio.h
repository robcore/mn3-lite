/* Copyright (c) 2021, The Linux Foundation. All rights reserved.
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

#ifndef __MXAUDIO_H__
#define __MXAUDIO_H__

#include <linux/types.h>
#include <linux/mfd/wcd9xxx/core-resource.h>

enum {
	HWEQ_OFF = 0,
	HWEQ_ON = 1,
};

extern unsigned int mx_hw_eq;
extern u8 hphl_cached_gain;
extern u8 hphr_cached_gain;
extern u8 speaker_cached_gain;
#if 0
extern u8 iir1_cached_gain;
extern u8 iir2_cached_gain;
#endif
#ifdef CONFIG_RAMP_VOLUME
extern unsigned int ramp_volume;
#endif

#endif //__MXAUDIO_H__
