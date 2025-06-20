#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <poll.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <endian.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <linux/ion.h>
#include <msm_ion.h>
//#include <media/msm_vidc.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#define DBG_TAG "  main"

#include "common.h"

#include "defs.h"
#include "ts.h"
#include "packet.h"
#include "video.h"
#include "list.h"



#define VIDEO_DEVICE "/dev/video32"
#define ION_DEVICE "/dev/ion"
#define ROTATOR_DEVICE "/dev/video2"
#define WIDTH 1928
#define HEIGHT 1208
#define STREAM_BUFFER_SIZE (1024 * 1024)
#define OUTPUT_BUFFER_COUNT 4
#define CAPTURE_BUFFER_COUNT 4

#define EXTRADATA_IDX(__num_planes) ((__num_planes) ? (__num_planes) - 1 : 0)
#define TIMESTAMP_NONE	((uint64_t)-1)


#define av_err(errnum, fmt, ...) \
	LOGE(fmt ": %s", ##__VA_ARGS__, av_err2str(errnum))


void stream_close(struct instance *i) {
	i->stream = NULL;
	// if (i->bsf)
	// 	av_bsf_free(&i->bsf);
	if (i->avctx)
		avformat_close_input(&i->avctx);
}

int handle_signal(struct instance *i)
{
	struct signalfd_siginfo siginfo;
	sigset_t sigmask;

	if (read(i->sigfd, &siginfo, sizeof (siginfo)) < 0) {
		perror("signalfd/read");
		return -1;
	}

	sigemptyset(&sigmask);
	sigaddset(&sigmask, siginfo.ssi_signo);
	sigprocmask(SIG_UNBLOCK, &sigmask, NULL);

	i->finish = 1;

	return 0;
}


int stream_open(struct instance *i) {
	//const AVBitStreamFilter *filter;
	AVCodecParameters *codecpar;
	AVRational framerate;
	//int codec;
	int ret;

	//av_register_all();
	avformat_network_init();

	ret = avformat_open_input(&i->avctx, i->url, NULL, NULL);
	if (ret < 0) {
		av_err(ret, "failed to open %s", i->url);
		goto fail;
	}

	ret = avformat_find_stream_info(i->avctx, NULL);
	if (ret < 0) {
		av_err(ret, "failed to get streams info");
		goto fail;
	}

	av_dump_format(i->avctx, -1, i->url, 0);

	ret = av_find_best_stream(i->avctx, AVMEDIA_TYPE_VIDEO, -1, -1,
				  NULL, 0);
	if (ret < 0) {
		av_err(ret, "stream does not seem to contain video");
		goto fail;
	}

	i->stream = i->avctx->streams[ret];
	codecpar = i->stream->codecpar;

	i->width = codecpar->width ?: 1928;
	i->height = codecpar->height ?: 1208;
	i->need_header = 1;

	framerate = av_guess_frame_rate(i->avctx, i->stream, NULL);
	i->fps_n = framerate.num;
	i->fps_d = framerate.den;
	// filter = av_bsf_get_by_name("hevc_mp4toannexb");
	i->fourcc = V4L2_PIX_FMT_HEVC;

	// ret = av_bsf_alloc(filter, &i->bsf);
	// if (ret < 0) {
	// 		av_err(ret, "cannot allocate bistream filter");
	// 		goto fail;
	// }

	// avcodec_parameters_copy(i->bsf->par_in, codecpar);
	// i->bsf->time_base_in = i->stream->time_base;

	// ret = av_bsf_init(i->bsf);
	// if (ret < 0) {
	// 		av_err(ret, "failed to initialize bitstream filter");
	// 		goto fail;
	// }


	return 0;

fail:
	stream_close(i);
	return -1;
}

int video_dequeue_buf(struct instance *i, struct v4l2_buffer *buf)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, VIDIOC_DQBUF, buf);
	if (ret < 0) {
		// err("failed to dequeue buffer on %s queue: %m",
		//     buf_type_to_string(buf->type));
		return -errno;
	}

	switch (buf->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		vid->cap_buf_flag[buf->index] = 0;
		break;
	}

	return 0;
}

int dequeue_output(struct instance *i, int *n)
{

	struct v4l2_buffer buf;
	struct v4l2_plane planes[OUT_PLANES];
	int ret;

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.m.planes = planes;
	buf.length = OUT_PLANES;

	ret = video_dequeue_buf(i, &buf);
	if (ret < 0)
		return ret;

	*n = buf.index;
	//static int frame_cnt = 0;
	//LOG("Dequeued %d output frames (Contains compressed frame)", frame_cnt);
	//frame_cnt++;

	return 0;
}


int restart_capture(struct instance *i) {
	struct video *vid = &i->video;
	int n;

	/*
	 * Destroy window buffers that are not in use by the
	 * wayland compositor; buffers in use will be destroyed
	 * when the release callback is called
	 */
	/* Stop capture and release buffers */
	if (vid->cap_buf_cnt > 0 && video_stop_capture(i))
		return -1;

	/* Setup capture queue with new parameters */
	if (video_setup_capture(i, CAPTURE_BUFFER_COUNT, i->width, i->height))
		return -1;

	/* Start streaming */
	if (video_stream(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			 VIDIOC_STREAMON))
		return -1;

	/* Queue all capture buffers */
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		if (video_queue_buf_cap(i, n))
			return -1;
	}

	return 0;
}


int handle_video_event(struct instance *i) {
	struct v4l2_event event;

	if (video_dequeue_event(i, &event))
		return -1;

	switch (event.type) {
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT: {
		unsigned int *ptr = (unsigned int *)event.u.data;
		unsigned int height = ptr[0];
		unsigned int width = ptr[1];

		LOG("Port Reconfig received insufficient, new size %ux%u",
		     width, height);

		i->depth = ptr[2];

		if (ptr[3] == MSM_VIDC_PIC_STRUCT_MAYBE_INTERLACED) {
			i->interlaced = 1;
		} else {
			i->interlaced = 0;
		}

		//unsigned int cspace = ptr[4];

		i->width = width;
		i->height = height;
		i->reconfigure_pending = 1;
		LOG("See dmesg msm_vidc for more info");
		/* flush capture queue, we will reconfigure it when flush
		 * done event is received */
		video_flush(i, V4L2_QCOM_CMD_FLUSH_CAPTURE);
		break;
	}
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT:
		LOGD("Setting changed sufficient");
		break;
	case V4L2_EVENT_MSM_VIDC_FLUSH_DONE: {
		unsigned int *ptr = (unsigned int *)event.u.data;
		unsigned int flags = ptr[0];

		if (flags & V4L2_QCOM_CMD_FLUSH_CAPTURE)
			LOGD("Flush Done received on CAPTURE queue");
		if (flags & V4L2_QCOM_CMD_FLUSH_OUTPUT)
			LOGD("Flush Done received on OUTPUT queue");

		if (i->reconfigure_pending) {
			LOGD("Reconfiguring output");
			restart_capture(i);
			i->reconfigure_pending = 0;
		}
		break;
	}
	case V4L2_EVENT_MSM_VIDC_SYS_ERROR:
		LOGD("SYS Error received");
		break;
	case V4L2_EVENT_MSM_VIDC_HW_OVERLOAD:
		LOGD("HW Overload received");
		break;
	case V4L2_EVENT_MSM_VIDC_HW_UNSUPPORTED:
		LOGD("HW Unsupported received");
		break;
	case V4L2_EVENT_MSM_VIDC_RELEASE_BUFFER_REFERENCE:
		LOGD("Release buffer reference");
		break;
	case V4L2_EVENT_MSM_VIDC_RELEASE_UNQUEUED_BUFFER:
		LOGD("Release unqueued buffer");
		break;
	default:
		LOGE("unknown event type occurred %x", event.type);
		break;
	}

	return 0;
}

int handle_video_capture(struct instance *i) {
	struct video *vid = &i->video;
	struct timeval tv;
	uint32_t flags;
	uint64_t pts;
	unsigned int bytesused;
	struct msm_vidc_extradata_header *extradata;
	bool busy;
	int ret, n;

	/* capture buffer is ready */

	ret = video_dequeue_capture(i, &n, &bytesused, &flags, &tv, &extradata);
	if (ret < 0) {
		LOGE("dequeue capture buffer fail");
		return ret;
	}

	if (flags & V4L2_QCOM_BUF_TIMESTAMP_INVALID)
		pts = TIMESTAMP_NONE;
	else
		pts = ((uint64_t)tv.tv_sec) * 1000000 + tv.tv_usec;

	busy = false;

	if (bytesused > 0) {
		struct ts_entry *l, *min = NULL;
		int pending = 0;

		vid->total_captured++;

		//pthread_mutex_lock(&i->lock);

		/* PTS are expected to be monotonically increasing,
		 * so when unknown use the lowest pending DTS */
		list_for_each_entry(struct ts_entry, l, &vid->pending_ts_list, link) {
			if (l->dts == TIMESTAMP_NONE)
				continue;
			if (min == NULL || min->dts > l->dts)
				min = l;
			pending++;
		}

		if (min) {
			LOGD("pending %d min pts %" PRIi64
			    " dts %" PRIi64
			    " duration %" PRIi64, pending,
			    min->pts, min->dts, min->duration);
		}

		if (pts == TIMESTAMP_NONE) {
			LOGD("no pts on frame");
			if (min && vid->pts_dts_delta != TIMESTAMP_NONE) {
				LOGD("reuse dts %" PRIu64
				    " delta %" PRIu64,
				    min->dts, vid->pts_dts_delta);
				pts = min->dts + vid->pts_dts_delta;
			}
		}

		if (pts == TIMESTAMP_NONE) {
			if (min && vid->cap_last_pts != TIMESTAMP_NONE)
				pts = vid->cap_last_pts + min->duration;
			else
				pts = 0;

			LOGD("guessing pts %" PRIu64, pts);
		}

		vid->cap_last_pts = pts;

		if (min != NULL) {
			pts -= min->base;
			ts_remove(min);
		}

		if (bytesused > 0 && vid->cap_buf_addr[n]) {

		// 	//LOG("Saving Frame %d, size %d", n, bytesused);
		// 	// Convert UBWC to linear NV12 using SDE rotator
		// 	unsigned char *linear_data = NULL;
		// 	//size_t linear_size = 0;
		// 	unsigned long ion_fd = (unsigned long)vid->cap_buf_fd[n];

		// 	//ret = convert_ubwc_to_linear(ion_fd, i->width, i->height, &linear_data, &linear_size);
		// 	// calculate frames per second
		static unsigned long frame_count = 0;
		static struct timeval last_time = {0, 0};
		frame_count++;
		struct timeval current_time;
		gettimeofday(&current_time, NULL);
			if (last_time.tv_sec == 0 && last_time.tv_usec == 0) {
				last_time = current_time;
			} else {
				double elapsed = (current_time.tv_sec - last_time.tv_sec) +
				                 (current_time.tv_usec - last_time.tv_usec) / 1000000.0;
				if (elapsed > 0) {
					double fps = frame_count / elapsed;
					LOG("Frames: %lu, FPS: %.2f", frame_count, fps);
				}
			}

		}

		//pthread_mutex_unlock(&i->lock);

		i->prerolled = 1;

	}

	if (!busy && !i->reconfigure_pending) {
		video_queue_buf_cap(i, n);
	}

	if (flags & V4L2_QCOM_BUF_FLAG_EOS) {
		LOG("End of stream");
		//finish(i);
	}

	return 0;
}

int handle_output(struct instance *i) {
	struct video *vid = &i->video;
	int ret, n;

	ret = dequeue_output(i, &n);
	if (ret < 0) {
		LOGE("dequeue output buffer fail");
		return ret;
	}

	vid->out_buf_flag[n] = 0;

	return 0;
}

int parse_frame(struct instance *i, AVPacket *pkt)
{
	int ret;

	ret = av_read_frame(i->avctx, pkt);
	if (ret < 0)
		return ret;

	if (pkt->stream_index != i->stream->index) {
		av_packet_unref(pkt);
		return AVERROR(EAGAIN);
	}

	if (i->bsf) {
		ret = av_bsf_send_packet(i->bsf, pkt);
		if (ret < 0)
			return ret;

		i->bsf_data_pending = 1;
	}


	if (i->bsf) {
		ret = av_bsf_receive_packet(i->bsf, pkt);
		if (ret == AVERROR(EAGAIN))
			i->bsf_data_pending = 0;

		if (ret < 0)
			return ret;
	}
	// for (int i = 0; i < pkt->size && i < 16; ++i)
    // 	print(2,"%02X ", pkt->data[i]);
	// print(2,"\n");
	return 0;
}

int get_buffer_unlocked(struct instance *i) {
	struct video *vid = &i->video;

	for (int n = 0; n < vid->out_buf_cnt; n++) {
		if (!vid->out_buf_flag[n])
			return n;
	}

	return -1;
}



void main_loop(struct instance *i) {

	struct video *vid = &i->video;
	//struct wl_display *wl_display = NULL;
	struct pollfd pfd[EV_COUNT];
	int ev[EV_COUNT];
	short revents;
	int nfds = 0;
	int ret;

	LOGD("main thread started");

	for (int n = 0; n < EV_COUNT; n++)
		ev[n] = -1;

	memset(pfd, 0, sizeof (pfd));

	pfd[nfds].fd = vid->fd;
	pfd[nfds].events = POLLOUT | POLLWRNORM | POLLPRI;
	ev[EV_VIDEO] = nfds++;

	if (i->sigfd != -1) {
		pfd[nfds].fd = i->sigfd;
		pfd[nfds].events = POLLIN;
		ev[EV_SIGNAL] = nfds++;
	}

	AVPacket pkt;
	int parse_ret;
	av_init_packet(&pkt);
	pfd[ev[EV_VIDEO]].events |= POLLIN | POLLRDNORM;


	//const int target_fps = 20;
	//const long frame_period_us = 1000000 / target_fps;
	//struct timeval last = {0};

	while (!i->finish) {
		// struct timeval now;
		// gettimeofday(&now, NULL);
		// if (last.tv_sec != 0) {
		// 	long elapsed_us = (now.tv_sec - last.tv_sec) * 1000000L + (now.tv_usec - last.tv_usec);
		// 	if (elapsed_us < frame_period_us){
		// 		usleep(frame_period_us - elapsed_us);
		// 	}
		// }
		// gettimeofday(&last, NULL);

		ret = poll(pfd, nfds, 18);
		if (ret <= 0) { // poll did not return any events so we will try to parse a frame

			// parse_ret = parse_frame(i, &pkt);
			parse_ret = av_read_frame(i->avctx, &pkt);
			if (parse_ret == AVERROR(EAGAIN)) {
				continue;
			}

			int buf = get_buffer_unlocked(i);
			if (buf < 0) {
				av_packet_unref(&pkt);
				LOGE("No output buffer available");
				continue;
			}
			if (parse_ret < 0) {
				if (parse_ret == AVERROR_EOF) {
					LOGD("Queue end of stream");
				} else {
					av_err(parse_ret, "Parsing failed");
				}
				LOG("Sending EOS for buffer %d", buf);
				send_eos(i, buf);
				av_packet_unref(&pkt);
				break;
			}

			if (send_pkt(i, buf, &pkt) < 0) {
				LOGE("Failed to send packet");
				av_packet_unref(&pkt);
				break;
			}
			av_packet_unref(&pkt);
			continue;
		}

		for (int idx = 0; idx < nfds; idx++) {
			revents = pfd[idx].revents;
			if (!revents)
				continue;

			if (idx == ev[EV_VIDEO]) {
				if (revents & (POLLIN | POLLRDNORM))
					handle_video_capture(i);
				if (revents & (POLLOUT | POLLWRNORM))
					handle_output(i);
				if (revents & POLLPRI)
					handle_video_event(i);

			} else if (idx == ev[EV_DISPLAY]) {
				if (revents & POLLOUT)
					pfd[ev[EV_DISPLAY]].events &= ~POLLOUT;

			} else if (idx == ev[EV_SIGNAL]) {
				handle_signal(i);
				break;
			}
		}
	}

	LOGD("main thread finished");
}


int main(int argc, char **argv) {
	LOG("Starting video decoder");
  struct instance inst = {0};
	memset(&inst, 0, sizeof(inst));
	inst.sigfd = -1;
	INIT_LIST_HEAD(&inst.video.pending_ts_list);
	INIT_LIST_HEAD(&inst.fb_list);
	inst.video.pts_dts_delta = TIMESTAMP_NONE;
	inst.video.cap_last_pts = TIMESTAMP_NONE;
	inst.video.extradata_index = -1;
	inst.video.extradata_size = 0;
	inst.video.extradata_ion_fd = -1;
	inst.url = argv[1];
	inst.video.name = (char *)VIDEO_DEVICE;

	if (stream_open(&inst)) {
			LOGE("Failed to open stream\n");
			return EXIT_FAILURE;
	}

	inst.video.fd = open(VIDEO_DEVICE, O_RDWR, 0);
	if (inst.video.fd < 0) {
		LOGE("Failed to open video decoder: %s", inst.video.name);
		return EXIT_FAILURE;
	}
	struct v4l2_capability cap;
	memzero(cap);
	if (ioctl(inst.video.fd, VIDIOC_QUERYCAP, &cap) < 0) {
		LOGE("Failed to verify capabilities: %m");
		return -1;
	}

	const int n_events = sizeof(event_type) / sizeof(event_type[0]);
	for (int i = 0; i < n_events; i++) {
		if (video_subscribe_event(&inst, event_type[i])) {
			LOGE("Failed to subscribe to event %d\n", event_type[i]);
			return EXIT_FAILURE;
		}
	}

	if (video_setup_output(&inst, inst.fourcc, STREAM_BUFFER_SIZE, OUTPUT_BUFFER_COUNT)) {
		LOGE("Failed to setup video output\n");
		return EXIT_FAILURE;
	}

 	if (video_set_control(&inst)) {
		LOGE("Failed to set video control\n");
		return EXIT_FAILURE;
	}

	if (video_stream(&inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,VIDIOC_STREAMON)) {
		LOGE("Failed to start video output stream\n");
		return EXIT_FAILURE;
	}

	if (restart_capture(&inst)) {
		LOGE("Failed to restart capture\n");
		return EXIT_FAILURE;
	}

	sigset_t sigmask;
	int fd;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);

	fd = signalfd(-1, &sigmask, SFD_CLOEXEC);
	if (fd < 0) {
		perror("signalfd");
		return EXIT_FAILURE;
	}

	sigprocmask(SIG_BLOCK, &sigmask, NULL);
	inst.sigfd = fd;

	LOG("Video stream started successfully\n");

	main_loop(&inst);

	return 0;

}

