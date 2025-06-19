#pragma once
// Stub for packet.h to allow build to proceed. Fill in as needed.
#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>
#include "video.h"

struct instance;
struct video;
struct AVPacket;
struct AVCodecParameters;
struct AVRational;

int vc1_write_bdu(uint8_t *dst, int dst_size,
		  uint8_t *bdu, int bdu_size,
		  uint8_t type);

// Forward declarations for missing functions
int write_sequence_header(struct instance *i, uint8_t *data, int size);
// int video_queue_buf_out(struct instance *i, int buf_index, int size, int flags, struct timeval tv);
// int video_dequeue_buf(struct instance *i, void *buf);

int vc1_find_sc(const uint8_t *data, int size)
{
	for (int i = 0; i < size - 4; i++) {
		if (data[i + 0] == 0x00 &&
		    data[i + 1] == 0x00 &&
		    data[i + 2] == 0x01)
			return i;
	}

	return -1;
}

int send_pkt(struct instance *i, int buf_index, AVPacket *pkt)
{
	struct video *vid = &i->video;
	struct timeval tv;
	uint64_t pts, dts, duration, start_time;
	int flags;
	int size;
	uint8_t *data;
	const char *hex = "";
	AVRational vid_timebase;
	AVRational v4l_timebase = { 1, 1000000 };
	AVCodecParameters *codecpar = i->stream->codecpar;

	data = (uint8_t *)vid->out_buf_addr[buf_index];
	size = 0;

	if (i->need_header) {
		int n = write_sequence_header(i, data, vid->out_buf_size);
		if (n > 0)
			size += n;

		switch (codecpar->codec_id) {
		case AV_CODEC_ID_WMV3:
		case AV_CODEC_ID_VC1:
			if (vc1_find_sc(pkt->data, MIN(10, pkt->size)) < 0)
				i->insert_sc = 1;
			break;
		default:
			break;
		}

		i->need_header = 0;
	}

	if ((codecpar->codec_id == AV_CODEC_ID_WMV3 ||
	     codecpar->codec_id == AV_CODEC_ID_VC1) &&
	    i->insert_sc) {
		size += vc1_write_bdu(data + size, vid->out_buf_size - size,
				      pkt->data, pkt->size, 0x0d);
	} else {
		memcpy(data + size, pkt->data, pkt->size);
		size += pkt->size;
	}

	flags = 0;

	vid_timebase = i->stream->time_base;

	start_time = 0;
	if (i->stream->start_time != AV_NOPTS_VALUE)
		start_time = av_rescale_q(i->stream->start_time,
					  vid_timebase, v4l_timebase);

	pts = TIMESTAMP_NONE;
	if (pkt->pts != AV_NOPTS_VALUE)
		pts = av_rescale_q(pkt->pts, vid_timebase, v4l_timebase);

	dts = TIMESTAMP_NONE;
	if (pkt->dts != AV_NOPTS_VALUE)
		dts = av_rescale_q(pkt->dts, vid_timebase, v4l_timebase);

	duration = TIMESTAMP_NONE;
	if (pkt->duration) {
		duration = av_rescale_q(pkt->duration,
					vid_timebase, v4l_timebase);
	}

	// if (debug_level > 3)
	// 	hex = dump_pkt(data, size);
	// else
	// 	hex = "";

	dbg("input size=%d pts=%" PRIi64 " dts=%" PRIi64 " duration=%" PRIu64
	     " start_time=%" PRIi64 "%s", size, pts, dts, duration,
	     start_time, hex);

	if (pts != TIMESTAMP_NONE) {
		tv.tv_sec = pts / 1000000;
		tv.tv_usec = pts % 1000000;
	} else {
		flags |= V4L2_QCOM_BUF_TIMESTAMP_INVALID;
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}

	if ((pkt->flags & AV_PKT_FLAG_KEY) &&
	    pts != TIMESTAMP_NONE && dts != TIMESTAMP_NONE)
		vid->pts_dts_delta = pts - dts;

	if (video_queue_buf_out(i, buf_index, size, flags, tv) < 0)
		return -1;


	pthread_mutex_lock(&i->lock);

	ts_insert(vid, pts, dts, duration, start_time);
	pthread_mutex_unlock(&i->lock);

	vid->out_buf_flag[buf_index] = 1;

	return 0;
}

int send_eos(struct instance *i, int buf_index) {
	struct video *vid = &i->video;
	struct timeval tv;

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	info("sending eos");
	if (video_queue_buf_out(i, buf_index, 0,
				V4L2_QCOM_BUF_FLAG_EOS |
				V4L2_QCOM_BUF_TIMESTAMP_INVALID, tv) < 0)
		return -1;

	vid->out_buf_flag[buf_index] = 1;

	return 0;
}


int rbdu_escape(uint8_t *dst, int dst_size, const uint8_t *src, int src_size)
{
	uint8_t *dstp = dst;
	const uint8_t *srcp = src;
	const uint8_t *end = src + src_size;
	int count = 0;

	while (srcp < end) {
		if (count == 2 && *srcp <= 0x03) {
			*dstp++ = 0x03;
			count = 0;
		}

		if (*srcp == 0)
			count++;
		else
			count = 0;

		*dstp++ = *srcp++;
	}

	return dstp - dst;
}

/*
 * Transform RBDU (raw bitstream decodable units)
 *  into an EBDU (encapsulated bitstream decodable units)
 */
int vc1_write_bdu(uint8_t *dst, int dst_size,
	      uint8_t *bdu, int bdu_size,
	      uint8_t type)
{
	int len;

	/* add start code */
	dst[0] = 0x00;
	dst[1] = 0x00;
	dst[2] = 0x01;
	dst[3] = type;
	len = 4;

	/* escape start codes */
	len += rbdu_escape(dst + len, dst_size - len, bdu, bdu_size);

	/* add flushing byte at the end of the BDU */
	dst[len++] = 0x80;

	return len;
}

int write_sequence_header_vc1(struct instance *i, uint8_t *data, int size)
{
	AVCodecParameters *codecpar = i->stream->codecpar;
	int n;

	if (codecpar->extradata_size == 0) {
		dbg("no codec data, skip sequence header generation");
		return 0;
	}

	if (codecpar->extradata_size == 4 || codecpar->extradata_size == 5) {
		/* Simple/Main Profile ASF header */
		return vc1_write_bdu(data, size,
				     codecpar->extradata,
				     codecpar->extradata_size,
				     0x0f);
	}

	if (codecpar->extradata_size == 36 && codecpar->extradata[3] == 0xc5) {
		/* Annex L Sequence Layer */
		if (size < codecpar->extradata_size)
			return -1;

		memcpy(data, codecpar->extradata, codecpar->extradata_size);
		return codecpar->extradata_size;
	}

	n = vc1_find_sc(codecpar->extradata, codecpar->extradata_size);
	if (n >= 0) {
		/* BDU in header */
		if (size < codecpar->extradata_size - n)
			return -1;

		memcpy(data, codecpar->extradata + n,
		       codecpar->extradata_size - n);
		return codecpar->extradata_size - n;
	}

	err("cannot parse VC1 codec data");

	return -1;
}

int write_sequence_header(struct instance *i, uint8_t *data, int size)
{
	AVCodecParameters *codecpar = i->stream->codecpar;

	switch (codecpar->codec_id) {
	case AV_CODEC_ID_WMV3:
	case AV_CODEC_ID_VC1:
		return write_sequence_header_vc1(i, data, size);
	default:
		return 0;
	}
}