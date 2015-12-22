/* 
 * BSD 3 Clause - See LICENCE file for details.
 *
 * Copyright (c) 2015, Intel Corporation
 * All rights reserved.
 *
 */

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <reef/reef.h>
#include <reef/lock.h>
#include <reef/list.h>
#include <reef/stream.h>
#include <reef/alloc.h>
#include <reef/work.h>
#include <reef/audio/component.h>

/* this should ramp from 0dB to mute in 64ms.
 * i.e 2^16 -> 0 in 32 * 2048 steps each lasting 2ms
 */
#define VOL_RAMP_MS	2
#define VOL_RAMP_STEP	2048

/*
 * Simple volume control
 *
 * Gain amplitude value is between 0 (mute) ... 2^16 (0dB) ... 2^24 (~+48dB)
 *
 * Currently we use 16 bit data for copies to/from DAIs and HOST PCM buffers,
 * 32 bit data is used in all other cases for overhead.
 * TODO: Add 24 bit (4 byte aligned) support using HiFi2 EP SIMD.
 */

/* volume component private data */
struct comp_data {
	uint32_t volume[STREAM_MAX_CHANNELS];	/* current volume */
	uint32_t tvolume[STREAM_MAX_CHANNELS];	/* target volume */
	uint32_t mvolume[STREAM_MAX_CHANNELS];	/* mute volume */
	void (*scale_vol)(struct comp_dev *dev, struct comp_buffer *sink,
		struct comp_buffer *source);
	struct work volwork;
};

struct comp_func_map {
	uint16_t source;	/* source format */
	uint16_t sink;		/* sink format */
	void (*func)(struct comp_dev *dev, struct comp_buffer *sink,
		struct comp_buffer *source);
};

/* copy and scale volume from 16 bit source buffer to 32 bit dest buffer */
static void vol_s16_to_s32(struct comp_dev *dev, struct comp_buffer *sink,
	struct comp_buffer *source)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int16_t *src = (int16_t*) source->r_ptr;
	int32_t *dest = (int32_t*) sink->w_ptr;
	int i, j;

	/* buffer sizes are always divisable by period frames */
	for (i = 0; i < dev->params.pcm.channels; i++) {
		for (j = 0; j < dev->params.pcm.period_frames; j++) {
			int32_t val = (int32_t)*src;
			*dest = (val * cd->volume[i]) >> 16;
			dest++;
			src++;
		}
	}

	source->r_ptr = (uint8_t*)src;
	sink->w_ptr = (uint8_t*)dest;
}

/* copy and scale volume from 24 bit source buffer to 16 bit dest buffer */
static void vol_s32_to_s16(struct comp_dev *dev, struct comp_buffer *sink,
	struct comp_buffer *source)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int32_t *src = (int32_t*) source->r_ptr;
	int16_t *dest = (int16_t*) sink->w_ptr;
	int i, j;

	/* buffer sizes are always divisable by period frames */
	for (i = 0; i < dev->params.pcm.channels; i++) {
		for (j = 0; j < dev->params.pcm.period_frames; j++) {
			/* TODO: clamp when converting to int16_t */
			*dest = (int16_t)((*src * cd->volume[i]) >> 16);
			dest++;
			src++;
		}
	}

	source->r_ptr = (uint8_t*)src;
	sink->w_ptr = (uint8_t*)dest;
}

/* copy and scale volume from 24 bit source buffer to 24 bit dest buffer */
static void vol_s32_to_s32(struct comp_dev *dev, struct comp_buffer *sink,
	struct comp_buffer *source)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int32_t *src = (int32_t*) source->r_ptr;
	int32_t *dest = (int32_t*) sink->w_ptr;
	int i, j;

	/* buffer sizes are always divisable by period frames */
	for (i = 0; i < dev->params.pcm.channels; i++) {
		for (j = 0; j < dev->params.pcm.period_frames; j++) {
			*dest = (*src * cd->volume[i]) >> 16;
			dest++;
			src++;
		}
	}

	source->r_ptr = (uint8_t*)src;
	sink->w_ptr = (uint8_t*)dest;
}

/* copy and scale volume from 16 bit source buffer to 16 bit dest buffer */
static void vol_s16_to_s16(struct comp_dev *dev, struct comp_buffer *sink,
	struct comp_buffer *source)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int16_t *src = (int16_t*) source->r_ptr;
	int16_t *dest = (int16_t*) sink->w_ptr;
	int i, j;

	/* buffer sizes are always divisable by period frames */
	for (i = 0; i < dev->params.pcm.channels; i++) {
		for (j = 0; j < dev->params.pcm.period_frames; j++) {
			int32_t val = (int32_t)*src;
			/* TODO: clamp when converting to int16_t */
			*dest = (int16_t)((val * cd->volume[i]) >> 16);
			dest++;
			src++;
		}
	}

	source->r_ptr = (uint8_t*)src;
	sink->w_ptr = (uint8_t*)dest;
}

/* map of source and sink buffer formats to volume function */
static const struct comp_func_map func_map[] = {
	{STREAM_FORMAT_S16_LE, STREAM_FORMAT_S16_LE, vol_s16_to_s16},
	{STREAM_FORMAT_S16_LE, STREAM_FORMAT_S32_LE, vol_s16_to_s32},
	{STREAM_FORMAT_S32_LE, STREAM_FORMAT_S16_LE, vol_s32_to_s16},
	{STREAM_FORMAT_S32_LE, STREAM_FORMAT_S32_LE, vol_s32_to_s32},
};

/* this ramps volume changes over time */
static uint32_t vol_work(void *data)
{
	struct comp_dev *dev = (struct comp_dev *)data;
	struct comp_data *cd = comp_get_drvdata(dev);
	int i, again = 0;

	/* inc/dec each volume if it's not at target */ 
	for (i = 0; i < dev->params.pcm.channels; i++) {

		/* skip if target reached */
		if (cd->volume[i] == cd->tvolume[i])
			continue;

		if (cd->volume[i] < cd->tvolume[i]) {
			/* ramp up */
			cd->volume[i] += VOL_RAMP_STEP;

			/* ramp completed ? */
			if (cd->volume[i] > cd->tvolume[i])
				cd->volume[i] = cd->tvolume[i];
			else
				again = 1;
		} else {
			/* ramp down */
			cd->volume[i] -= VOL_RAMP_STEP;

			/* ramp completed ? */
			if (cd->volume[i] < cd->tvolume[i])
				cd->volume[i] = cd->tvolume[i];
			else
				again = 1;
		}
	}

	/* do we need to continue ramping */
	if (again)
		return VOL_RAMP_MS;
	else
		return 0;
}

static struct comp_dev *volume_new(struct comp_desc *desc)
{
	struct comp_dev *dev;
	struct comp_data *cd;

	dev = rmalloc(RZONE_MODULE, RMOD_SYS, sizeof(*dev));
	if (dev == NULL)
		return NULL;

	cd = rmalloc(RZONE_MODULE, RMOD_SYS, sizeof(*cd));
	if (cd == NULL) {
		rfree(RZONE_MODULE, RMOD_SYS, dev);
		return NULL;
	}

	comp_set_drvdata(dev, cd);
	work_init(&cd->volwork, vol_work, dev);
	dev->id = desc->id;
	return dev;
}

static void volume_free(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	rfree(RZONE_MODULE, RMOD_SYS, cd);
	rfree(RZONE_MODULE, RMOD_SYS, dev);
}

/* set component audio stream paramters */
static int volume_params(struct comp_dev *dev, struct stream_params *params)
{
	/* just copy params atm */
	dev->params = *params;
	return 0;
}

static int volume_prepare(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *sink, *source;
	struct comp_dev *sink_dev, *source_dev;
	int i;

	/* volume components will only ever have 1 source and 1 sink buffer */
	source = list_entry(&dev->bsource_list, struct comp_buffer, sink_list);
	sink = list_entry(&dev->bsink_list, struct comp_buffer, source_list);
	sink_dev = sink->sink;
	source_dev = source->source;

	/* map the volume function for source and sink buffers */
	for (i = 0; i < ARRAY_SIZE(func_map); i++) {
		if (source_dev->params.pcm.format != func_map[i].source)
			continue;
		if (sink_dev->params.pcm.format != func_map[i].sink)
			continue;

		cd->scale_vol = func_map[i].func;
		return 0;
	}

	return -EINVAL;
}

static inline void volume_set_chan(struct comp_dev *dev, int chan, uint16_t vol)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	cd->tvolume[chan] = vol;
}

static inline void volume_set_chan_mute(struct comp_dev *dev, int chan)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	cd->mvolume[chan] = cd->volume[chan];
	cd->tvolume[chan] = 0;
}

static inline void volume_set_chan_unmute(struct comp_dev *dev, int chan)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	cd->tvolume[chan] = cd->mvolume[chan];
}

/* used to pass standard and bespoke commands (with data) to component */
static int volume_cmd(struct comp_dev *dev, int cmd, void *data)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_volume *cv = (struct comp_volume*)data;
	int i;

	switch (cmd) {
	case COMP_CMD_VOLUME:
		for (i = 0; i < dev->params.pcm.channels; i++)
			volume_set_chan(dev, i, cv->volume[i]);
		work_schedule_default(&cd->volwork, VOL_RAMP_MS);
		break;
	case COMP_CMD_MUTE:
		for (i = 0; i < dev->params.pcm.channels; i++) {
			if (cv->volume[i])
				volume_set_chan_mute(dev, i);
		}
		work_schedule_default(&cd->volwork, VOL_RAMP_MS);
		break;
	case COMP_CMD_UNMUTE:
		for (i = 0; i < dev->params.pcm.channels; i++) {
			if (cv->volume[i])
				volume_set_chan_unmute(dev, i);
		}
		work_schedule_default(&cd->volwork, VOL_RAMP_MS);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* copy and process stream data from source to sink buffers */
static int volume_copy(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *sink, *source;

	/* volume components will only ever have 1 source and 1 sink buffer */
	source = list_entry(&dev->bsource_list, struct comp_buffer, sink_list);
	sink = list_entry(&dev->bsink_list, struct comp_buffer, source_list);

	/* copy and scale volume */
	cd->scale_vol(dev, source, sink);

	/* update buffer pointers for overflow */
	if (source->r_ptr >= source->addr + source->size)
		source->r_ptr = source->addr;
	if (sink->w_ptr >= source->addr + sink->size)
		sink->w_ptr = sink->addr;

	/* number of frames sent downstream */
	return dev->params.pcm.period_frames;
}

struct comp_driver comp_volume = {
	.uuid	= COMP_UUID(COMP_VENDOR_GENERIC, COMP_TYPE_VOLUME),
	.ops	= {
		.new		= volume_new,
		.free		= volume_free,
		.params		= volume_params,
		.cmd		= volume_cmd,
		.copy		= volume_copy,
		.prepare	= volume_prepare,
	},
	.caps	= {
		.source = {
			.formats	= STREAM_FORMAT_S16_LE | STREAM_FORMAT_S32_LE,
			.min_rate	= 8000,
			.max_rate	= 192000,
			.min_channels	= 1,
			.max_channels	= STREAM_MAX_CHANNELS,
		},
		.sink = {
			.formats	= STREAM_FORMAT_S16_LE | STREAM_FORMAT_S32_LE,
			.min_rate	= 8000,
			.max_rate	= 192000,
			.min_channels	= 1,
			.max_channels	= STREAM_MAX_CHANNELS,
		},
	},
};

void sys_comp_volume_init(void)
{
	comp_register(&comp_volume);
}
