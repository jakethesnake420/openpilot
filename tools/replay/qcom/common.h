/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Common stuff header file
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
 * Copyright (c) 2015 Linaro Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modifications:
 * - Added support for openpilot - Ryley McCarroll
 */
#pragma once
#ifndef INCLUDE_COMMON_H
#define INCLUDE_COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <termios.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "common/swaglog.h"

#include "list.h"

extern int debug_level;

#define ARRAY_LENGTH(x) (sizeof (x) / sizeof (*(x)))

// Logging macros using cloudlog/LOG interface (no FILE* argument)
#define print(l, msg, ...) LOG(msg, ##__VA_ARGS__)
#define err(msg, ...) LOG("error: " msg, ##__VA_ARGS__)
#define info(msg, ...) LOG("info: " msg, ##__VA_ARGS__)
#define dbg(msg, ...) LOG("debug: " msg, ##__VA_ARGS__)

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define memzero(x)	memset(&(x), 0, sizeof (x));

/* Maximum number of output buffers */
#define MAX_OUT_BUF		16

/* Maximum number of capture buffers (32 is the limit imposed by MFC */
#define MAX_CAP_BUF		32

/* Number of output planes */
#define OUT_PLANES		1

/* Number of capture planes */
#define CAP_PLANES		2

/* Maximum number of planes used in the application */
#define MAX_PLANES		CAP_PLANES

/* video decoder related parameters */
struct video {
	char *name;
	int fd;

	/* Output queue related */
	int out_buf_cnt;
	int out_buf_size;
	int out_buf_off[MAX_OUT_BUF];
	char *out_buf_addr[MAX_OUT_BUF];
	int out_buf_flag[MAX_OUT_BUF];
	int out_ion_fd;
	int out_ion_size;
	void *out_ion_addr;

	/* Capture queue related */
	int cap_w;
	int cap_h;
	int cap_buf_cnt;
	uint32_t cap_buf_format;
	int cap_planes_count;
	int cap_plane_off[CAP_PLANES];
	int cap_plane_stride[CAP_PLANES];
	int cap_buf_flag[MAX_CAP_BUF];
	int cap_buf_size;
	int cap_buf_fd[MAX_CAP_BUF];
	void *cap_buf_addr[MAX_CAP_BUF];

	/* timestamp list for all pending frames */
	struct list_head pending_ts_list;
	uint64_t cap_last_pts;
	uint64_t pts_dts_delta;

	/* Extradata stuff */
	int extradata_index;
	int extradata_size;
	int extradata_ion_fd;
	void *extradata_ion_addr;
	int extradata_off[MAX_CAP_BUF];
	void *extradata_addr[MAX_CAP_BUF];

	/* Metrics */
	unsigned long total_captured;
};

struct rotator {
	int fd;
	int width;
	int height;
	int fourcc;
	int secure;
	int out_buf_size;
	int out_buf_cnt;
	unsigned long out_buf_fd[MAX_OUT_BUF];
	void *out_buf_addr[MAX_OUT_BUF];
};

struct instance {
	int width;
	int height;
	int fullscreen;
	uint32_t fourcc;
	int fps_n, fps_d;
	int depth;
	int interlaced;
	int decode_order;
	int skip_frames;
	int insert_sc;
	int need_header;
	int secure;
	int continue_data_transfer;
	char *url;

	/* video decoder related parameters */
	struct video	video;
	struct rotator	rotator;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	/* Control */
	int sigfd;
	int paused;
	int prerolled;
	int finish;  /* Flag set when decoding has been completed and all
			threads finish */

	int reconfigure_pending;
	int group;

	struct display *display;
	struct window *window;
	struct list_head fb_list;

	int stdin_valid;
	struct termios stdin_termios;

	AVFormatContext *avctx;
	AVStream *stream;
	AVBSFContext *bsf;
	int bsf_data_pending;
};

#endif /* INCLUDE_COMMON_H */