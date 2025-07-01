#include <sys/signalfd.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <deque>

//#include <sys/ioctl.h>
#include <assert.h>
#include "decoder.h"
#include "common/swaglog.h"
#include "common/util.h"
#include <cstdio>


static void checked_ioctl(int fd, unsigned long request, void *argp) {
  int ret = util::safe_ioctl(fd, request, argp);
  if (ret != 0) {
    LOGE("checked_ioctl failed with error %d (%d %lx %p)", errno, fd, request, argp);
    assert(0);
  }
}

static void request_buffers(int fd, v4l2_buf_type buf_type, unsigned int count) {
  struct v4l2_requestbuffers reqbuf = {
    .type = buf_type,
    .memory = V4L2_MEMORY_USERPTR,
    .count = count
  };
  checked_ioctl(fd, VIDIOC_REQBUFS, &reqbuf);
}


MsmVidc::MsmVidc() {}

// asserts on failure
bool MsmVidc::init(const char* dev,
                   size_t width, size_t height,
                   uint64_t codec) {
  LOGD("Initializing msm_vidc device %s", dev);
  this->w = width;
  this->h = height;
  this->c = codec;
  this->fd = open(dev, O_RDWR, 0);
  if (fd < 0) {
    LOGE("failed to open video device %s", dev);
    return false;
  }
  struct v4l2_capability cap = {0};
  checked_ioctl(fd, VIDIOC_QUERYCAP, &cap);
  subscribeEvents();

  v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

  setPlaneFormat(out_type, V4L2_PIX_FMT_HEVC); // Also allocates the output buffer.
  setFPS(FPS);
  request_buffers(fd, out_type, OUTPUT_BUFFER_COUNT);
  setControls();
  checked_ioctl(fd, VIDIOC_STREAMON, &out_type);
  restartCapture();
  setupPolling();
  rotator.init();
  rotator.configUBWCtoNV12(width, height);
  initialized = true;

  return true;
}

MsmVidc::~MsmVidc() {
  if (fd > 0) {
    close(fd);
  }
  if (sigfd > 0) {
    close(sigfd);
  }
}

bool MsmVidc::subscribeEvents() {
  for (uint32_t event : subscriptions) {
    struct v4l2_event_subscription sub = { .type = event};
    checked_ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
  }
  return true;
}

bool MsmVidc::setPlaneFormat(enum v4l2_buf_type type, uint32_t fourcc) {
  struct v4l2_format fmt = {.type = type};
  struct v4l2_pix_format_mplane *pix = &fmt.fmt.pix_mp;
  *pix = { .width = (__u32)this->w, .height = (__u32)this->h, .pixelformat = fourcc };
  checked_ioctl(fd, VIDIOC_S_FMT, &fmt);

  LOG("This is not the same as the working implementation, need to check if this is correct");
  if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
    this->out_buf_size = pix->plane_fmt[0].sizeimage;
    // out_buf_cnt = OUTPUT_BUFFER_COUNT;
    int ion_size = this->out_buf_size * OUTPUT_BUFFER_COUNT; // Output (input) buffers are ION buffer.
    this->out_buf.allocate_no_cache(ion_size); // mmap rw
    for (int i = 0; i < OUTPUT_BUFFER_COUNT; i++) {
      this->out_buf_off[i] = i * this->out_buf_size;
      this->out_buf_addr[i] = (char *)this->out_buf.addr + this->out_buf_off[i];
      this->out_buf_flag[i] = false;
    }
    LOGD("Set output buffer size to %d, count %d, addr %p", this->out_buf_size, OUTPUT_BUFFER_COUNT, this->out_buf.addr);
  } else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    request_buffers(this->fd, type, CAPTURE_BUFFER_COUNT);
    checked_ioctl(fd, VIDIOC_G_FMT, &fmt);
    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
      this->cap_bufs[i].allocate_no_cache(pix->plane_fmt[0].sizeimage); // Note that this is mmapped rw which is different from the working implementation.
      this->cap_bufs[i].init_yuv(pix->width, pix->height, pix->plane_fmt[0].bytesperline, 0);
    }
    this->cap_buf_format = pix->pixelformat;
    assert(pix->pixelformat == v4l2_fourcc('Q', '1', '2', '8')); // should be NV12 UBWC
    assert(CAP_PLANES == fmt.fmt.pix_mp.num_planes); // These may not be the same, but they should be
    LOGW(" this->cap_planes_count = 2, need to check what going on here");
    //this->cap_planes_count = CAP_PLANES; // 2 planes for NV12 UBWC, 1 plane for NV12
    // alloc extra buffer for extradata becasue we know the size from pix structure right now
    this->ext_buf.allocate_no_cache(pix->plane_fmt[1].sizeimage * CAPTURE_BUFFER_COUNT);
    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
      size_t offset = i * pix->plane_fmt[1].sizeimage;
      this->ext_buf_off[i] = offset;
      this->ext_buf_addr[i] = (char *)this->ext_buf.addr + offset;
    }
    LOGD("Set capture buffer size to %d, count %d, addr %p, extradata size %d",
      pix->plane_fmt[0].sizeimage, CAPTURE_BUFFER_COUNT, this->cap_bufs[0].addr, pix->plane_fmt[1].sizeimage);
  }
  return true;
}

bool MsmVidc::setFPS(uint32_t fps) {
  struct v4l2_streamparm streamparam = {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .parm.output.timeperframe = {1, fps}
  };
  checked_ioctl(fd, VIDIOC_S_PARM, &streamparam);
  LOGD("Set FPS to %d, timeperframe %d/%d", fps, streamparam.parm.output.timeperframe.numerator, streamparam.parm.output.timeperframe.denominator);
  return true;
}


bool MsmVidc::setControls() {
  struct v4l2_control control = {0};
	control.id = V4L2_CID_MPEG_VIDC_VIDEO_OPERATING_RATE;
	control.value = INT_MAX;
  checked_ioctl(fd, VIDIOC_S_CTRL, &control);
	control.id = V4L2_CID_MPEG_VIDC_VIDEO_CONCEAL_COLOR_8BIT;
	control.value = 0x000000ff;
  checked_ioctl(fd, VIDIOC_S_CTRL, &control);
	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_INTERLACE_VIDEO;
  checked_ioctl(fd, VIDIOC_S_CTRL, &control);
	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_OUTPUT_CROP;
  checked_ioctl(fd, VIDIOC_S_CTRL, &control);
	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_ASPECT_RATIO;
  checked_ioctl(fd, VIDIOC_S_CTRL, &control);
	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_FRAME_RATE;
  checked_ioctl(fd, VIDIOC_S_CTRL, &control);
  #if 0
  control.id = V4L2_CID_MPEG_VIDC_VIDEO_PICTYPE_DEC_MODE;
	control.value = V4L2_MPEG_VIDC_VIDEO_PICTYPE_DECODE_ON;
  checked_ioctl(fd, VIDIOC_S_CTRL, &control);
	control.id = V4L2_CID_MPEG_VIDC_VIDEO_EXTRADATA;
	control.value = V4L2_MPEG_VIDC_EXTRADATA_DISPLAY_COLOUR_SEI;
  checked_ioctl(fd, VIDIOC_S_CTRL, &control);
  #endif
  LOGD("Set controls: operating rate %d, conceal color 0x%08x, extradata interlace %d, output crop %d, aspect ratio %d, frame rate %d",
      control.value, control.value, control.value, control.value, control.value, control.value);
  return true;
}

bool MsmVidc::restartCapture() {
  // stop capture if already initialized
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (this->initialized) {
    LOGD("Stopping capture and releasing buffers");
    checked_ioctl(this->fd, VIDIOC_STREAMOFF, &type);
    struct v4l2_requestbuffers reqbuf = {.memory = V4L2_MEMORY_USERPTR, .type = type};
    checked_ioctl(this->fd, VIDIOC_REQBUFS, &reqbuf);
    for (size_t i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
      this->cap_bufs[i].free();
      this->cap_buf_flag[i] = false; // mark as not queued
      cap_bufs[i].~VisionBuf();
      new (&cap_bufs[i]) VisionBuf();
    }
  }

  // setup capture
  setDBP(V4L2_MPEG_VIDC_VIDEO_DPB_COLOR_FMT_NONE);
  setPlaneFormat(type, v4l2_fourcc('Q', '1', '2', '8'));
  //start capture
  checked_ioctl(this->fd, VIDIOC_STREAMON, &type);
  for (size_t i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
    queueCaptureBuffer(i);
  }

  return true;
}

bool MsmVidc::queueCaptureBuffer(int i) {

  struct v4l2_buffer buf = {0};
  struct v4l2_plane planes[CAP_PLANES] = {0};

  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory = V4L2_MEMORY_USERPTR;
  buf.index = i;
  buf.m.planes = planes;
  buf.length = 2;
  // decoded frame plane
  planes[0].m.userptr = (unsigned long)this->cap_bufs[i].addr; // no security
  planes[0].length = this->cap_bufs[i].len;
  planes[0].reserved[0] = this->cap_bufs[i].fd; // ION fd
  planes[0].reserved[1] = 0; // reserved
  planes[0].bytesused = this->cap_bufs[i].len;
  planes[0].data_offset = 0;
  // extradata plane
  planes[1].m.userptr = (unsigned long)this->ext_buf_addr[i];
  planes[1].length = this->ext_buf.len;
  planes[1].reserved[0] = this->ext_buf.fd; // ION fd
  planes[1].reserved[1] = this->ext_buf_off[i]; // offset in the buffer
  planes[1].bytesused = 0; // why is this 0?
  planes[1].data_offset = 0;
  checked_ioctl(this->fd, VIDIOC_QBUF, &buf);
  this->cap_buf_flag[i] = true; // mark as queued
  //LOGD("Queued capture buffer %d with size %zu, extradata size %zu, offset %zu", i, planes[0].length, planes[1].length, planes[1].reserved[1]);
  return true;
}

bool MsmVidc::queueOutputBuffer(int i, size_t size, uint32_t flags, struct timeval tv) {
  struct v4l2_buffer buf = {0};
  struct v4l2_plane planes[OUT_PLANES] = {0};

  buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  buf.memory = V4L2_MEMORY_USERPTR;
  buf.index = i;
  buf.m.planes = planes;
  buf.length = 1;
  // decoded frame plane
  planes[0].m.userptr = (unsigned long)this->out_buf_off[i]; // check this
  planes[0].length = this->out_buf_size;
  planes[0].reserved[0] = this->out_buf.fd; // ION fd
  planes[0].reserved[1] = 0;
  planes[0].bytesused = size;
  planes[0].data_offset = 0;
  assert((this->out_buf_off[i] & 0xfff) == 0);          // must be 4 KiB aligned
  assert(this->out_buf_size % 4096 == 0);               // ditto for size

  buf.flags = flags;
  buf.timestamp = tv;
  checked_ioctl(this->fd, VIDIOC_QBUF, &buf);
  this->out_buf_flag[i] = true; // mark as queued
  LOGD("Queued output buffer %d with size %zu, flags %08x, timestamp %ld.%06lu", i, size, flags, tv.tv_sec, tv.tv_usec);
  return true;
}

bool MsmVidc::setDBP(v4l2_mpeg_vidc_video_dpb_color_format format) {
  struct v4l2_ext_control control[2] = {0};
  struct v4l2_ext_controls controls = {0};

  control[0].id = V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_MODE;
  control[0].value = V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_PRIMARY;

  control[1].id = V4L2_CID_MPEG_VIDC_VIDEO_DPB_COLOR_FORMAT;
  control[1].value = format;

  controls.count = 2;
  controls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
  controls.controls = control;

  checked_ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls);
  LOGD("Set DBP format to %d", format);
  return true;
}

bool MsmVidc::setupPolling() {
  // setup polling
  sigset_t sigmask;
  sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);
  this->sigfd = signalfd(-1, &sigmask, SFD_CLOEXEC);
  assert(sigfd > 0);
  sigprocmask(SIG_BLOCK, &sigmask, nullptr);
  nfds = 0;
  assert(this->fd > 0);
  pfd[nfds].fd = this->fd;
  pfd[nfds].events = POLLOUT | POLLWRNORM | POLLPRI;
  ev[EV_VIDEO] = nfds++;
  pfd[nfds].fd = this->sigfd;
  pfd[nfds].events = POLLIN;
  ev[EV_SIGNAL] = nfds++;
  pfd[ev[EV_VIDEO]].events |= POLLIN | POLLRDNORM;
  LOGD("Setup polling with %d fds", nfds);
  for (int i = 0; i < nfds; i++) {
    LOGD("Poll fd %d, events %d", pfd[i].fd, pfd[i].events);
  }
  return true;
}

bool MsmVidc::sendEOS() {
  LOGE("sendEOS not implemented yet");
  assert(0);
}

bool MsmVidc::sendPacket(int buf_index, AVPacket *pkt) {
  struct timeval tv;
	uint64_t pts, dts, duration, start_time = TIMESTAMP_NONE;
	int flags = 0;
	size_t size = 0;
	uint8_t *data;
	AVRational vid_timebase = this->avctx->streams[0]->time_base;
  AVRational v4l_timebase = { 1, 1000000 };

  assert(buf_index >= 0 && buf_index < out_buf_cnt);
  assert(pkt != nullptr && pkt->data != nullptr && pkt->size > 0);

  pts = pkt->pts;
  dts = pkt->dts;
  duration = pkt->duration;
  start_time = 0;
  if (this->avctx->start_time != AV_NOPTS_VALUE) {
    start_time = av_rescale_q(this->avctx->start_time,
					  vid_timebase, v4l_timebase);
  }
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
		this->pts_dts_delta = pts - dts;

  // if (pts == TIMESTAMP_NONE || dts == TIMESTAMP_NONE)
  //   LOGE("Invalid PTS or DTS in packet");

  // if (pts < dts)
  //   LOGE("PTS is less than DTS, this is not allowed");

  // if (pkt->size > this->out_buf_size)
  //   LOGE("Packet size %d is larger than output buffer size %d", pkt->size, this->out_buf_size);


  // Prepare output buffer
  memset(this->out_buf_addr[buf_index], 0, this->out_buf_size);
	//AVRational v4l_timebase = { 1, 1000000 };
	data = (uint8_t *)this->out_buf_addr[buf_index];
  // Would send sequence header here if needed
  if (pkt->pts )

  memcpy(data + size, pkt->data, pkt->size);
  size += pkt->size;
  // time base is unknown here
  //vid_timebase = {1, 30}; // TODO: get from stream
  flags |= V4L2_QCOM_BUF_TIMESTAMP_INVALID;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  if ((pkt->flags & AV_PKT_FLAG_KEY) &&
       pts != TIMESTAMP_NONE && dts != TIMESTAMP_NONE) {
     this->pts_dts_delta = pts - dts;
  }
  queueOutputBuffer(buf_index, size, flags, tv);
  //LOGW("ts needs work");
  tsInsert(pts, dts, duration, start_time);
  return true;
}

void MsmVidc::tsInsert(uint64_t pts, uint64_t dts, uint64_t duration, uint64_t start_time) {
  // For tracking timestamps of frames.
  struct ts_entry entry = {.pts = pts, .dts = dts, .duration = duration, .base = start_time};
  pending_ts_list.push_back(entry);
  LOGD("Inserted timestamp: pts=%" PRIu64 ", dts=%" PRIu64 ", duration=%" PRIu64 ", start_time=%" PRIu64, pts, dts, duration, start_time);
}

int MsmVidc::getBufferUnlocked() {
  for (int i = 0; i < this->out_buf_cnt; i++) {
    if (!out_buf_flag[i]) {
      return i;
    }
  }
  return -1;
}

VisionBuf* MsmVidc::decodeFrame(AVPacket *pkt, VisionBuf *buf) {
  //LOGW("TODO: make setter for avctx");
  assert(initialized && (pkt != nullptr) && (buf != nullptr));

  this->frame_ready = false;
  this->current_output_buf = buf;
  while (!this->frame_ready) {
    int p = poll(pfd, nfds, 12);
    if (p <= 0) { // try to send a packet since there is nothing going on
      int buf_index = getBufferUnlocked();
      LOGD("getBufferUnlocked returned %d", buf_index);
      if (buf_index < 0) {
        continue;
      }
      assert(buf_index >= 0 && buf_index < out_buf_cnt);
      sendPacket(buf_index, pkt);
      // not unref'ing the packet here, as it is not owned by this class
    } else { // We haVe events to handle
      for (int idx = 0; idx < nfds; idx++) {
        short revents = pfd[idx].revents;
        if (!revents) {
          continue; // no events for this fd
        }
        if (idx == ev[EV_VIDEO]) {
          if (revents & (POLLIN | POLLRDNORM)) {
            VisionBuf *result = handleCapture();
            if (result == this->current_output_buf) {
              this->frame_ready = true;
            }
          }
          if (revents & (POLLOUT | POLLWRNORM))
            handleOutput();
          if (revents & POLLPRI)
            handleEvent();

        } else if (idx == ev[EV_SIGNAL]) {
          handleSignal();
          break;
        } else {
          LOGE("Unexpected event on fd %d", pfd[idx].fd);
          continue; // unexpected event, skip
        }
      }
    }
  }
  return buf;
}

int MsmVidc::handleSignal() {
	struct signalfd_siginfo siginfo;
	sigset_t sigmask;
  if (read(this->sigfd, &siginfo, sizeof (siginfo)) < 0) {
		LOGE("signalfd/read");
		return -1;
	}
  sigemptyset(&sigmask);
	sigaddset(&sigmask, siginfo.ssi_signo);
	sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
  // clean up
  LOGD("Received signal %d, cleaning up", siginfo.ssi_signo);
  if (fd > 0) {
    close(fd);
  }
  if (sigfd > 0) {
    close(sigfd);
  }
  return 0;
}

VisionBuf* MsmVidc::handleCapture() {
  //unsigned int bytesused = 0;
  struct msm_vidc_extradata_header *extradata = nullptr;
  struct v4l2_buffer buf = {0};
  struct v4l2_plane planes[CAP_PLANES] = {0};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory = V4L2_MEMORY_USERPTR;
  buf.m.planes = planes;
  buf.length = CAP_PLANES;
  checked_ioctl(this->fd, VIDIOC_DQBUF, &buf);

  //bytesused = buf.m.planes[0].bytesused;
  int n = buf.index;
  uint32_t flags = buf.flags;
  void *extradata_addr = nullptr;
  if (this->ext_buf_addr[n]) {
    extradata_addr = this->ext_buf_addr[n];
    // validate extradata
    //LOGW("Implement extradata validation here");
    extradata = (struct msm_vidc_extradata_header *)extradata_addr;
    if (extradata->type == MSM_VIDC_EXTRADATA_FRAME_RATE) {
      LOGD("Frame rate extradata");
    } else if (extradata->type == MSM_VIDC_EXTRADATA_ASPECT_RATIO) {
      LOGD("Aspect ratio extradata");
    } else if (extradata->type == MSM_VIDC_EXTRADATA_OUTPUT_CROP) {
      LOGD("Output crop extradata");
    } else {
      LOGW("Unknown extradata type %d", extradata->type);
    }

  }

  uint64_t pts = flags & V4L2_QCOM_BUF_TIMESTAMP_INVALID ? \
    TIMESTAMP_NONE : buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;

  if (buf.m.planes[0].bytesused) {
    LOGD("Dequeued capture buffer");
    static size_t cap_cnt = 0;
    cap_cnt++;
    if (cap_cnt % 240 == 0) {
      LOGD("Dequeued %zu capture buffers", cap_cnt);
    }

    // Find the ts_entry with the minimum dts that is not TIMESTAMP_NONE
    struct ts_entry *min = nullptr;
    int pending = 0;
    for (auto &entry : this->pending_ts_list) {
      if (entry.dts == TIMESTAMP_NONE)
        continue;
      if (min == nullptr || min->dts > entry.dts)
        min = &entry;
      pending++;
    }
    LOGD("Pending timestamps count %d", pending);

    if (min != nullptr) {
      // LOGD("Dequeued capture buffer %d, pts %lu, dts %lu, duration %lu, start_time %lu, pending ts count %d",
      //      n, pts, min->dts, min->duration, min->start_time, pending);
    }
    if (pts != TIMESTAMP_NONE) {
      LOGD("no pts on frame");
      if (min != nullptr && this->pts_dts_delta != TIMESTAMP_NONE) {
        LOGD("reuse dts %lu delta %lu", min->dts, this->pts_dts_delta);
        pts = min->dts + this->pts_dts_delta;
      }
    }

    if (pts == TIMESTAMP_NONE) {
      if (min != nullptr && this->cap_last_pts != TIMESTAMP_NONE) {
        pts = this->cap_last_pts + min->duration;
      } else {
        pts = 0;
      }
      LOGD("Using fallback pts %lu", pts);
    }
    this->cap_last_pts = pts;
    if (min != nullptr) {
      pts -= min->base;
      pending_ts_list.remove_if([min](const ts_entry& entry) { return &entry == min; });
    }

    static std::deque<double> frame_times;
    static const size_t WINDOW_SIZE = 30; // Rolling window of 30 frames
    static unsigned long frame_count = 0;

    frame_count++;
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    double current_timestamp = current_time.tv_sec + current_time.tv_usec / 1000000.0;

    frame_times.push_back(current_timestamp);

    // Keep only the last WINDOW_SIZE frame times
    if (frame_times.size() > WINDOW_SIZE) {
      frame_times.pop_front();
    }
    // Calculate rolling average FPS
    if (frame_times.size() >= 2) {
      double time_span = frame_times.back() - frame_times.front();
      if (time_span > 0) {
        double fps = (frame_times.size() - 1) / time_span;
        LOG("Frames: %lu, Rolling FPS (last %zu frames): %.2f", frame_count, frame_times.size(), fps);
      }
    }

    //rotator.saveFrame("capture_frame.yuv", true);
    // unsigned char *linear_data = NULL;
		// size_t linear_size = 0;
    // rotator.convert_ubwc_to_linear(this->cap_bufs[n].fd, this->w, this->h, &linear_data, &linear_size);
    // // save the frame to a file for debugging
    // FILE *f = fopen("/data/openpilot/capture_frame.yuv", "wb");
    // if (f != NULL) {
    //   fwrite(linear_data, 1, linear_size, f);
    //   fclose(f);
    //   LOGD("Saved capture frame to /data/openpilot/capture_frame.yuv");
    // } else {
    //   LOGE("Failed to open /data/openpilot/capture_frame.yuv for writing");
    // }
    if (!this->reconfigure_pending) {
      rotator.putFrame(&cap_bufs[n]);
      VisionBuf *rotated = rotator.getFrame(100);
      //rotated->stride = 1928; // set stride to width
      queueCaptureBuffer(n);
      if (rotated) {
        rotator.convertStride(rotated, this->current_output_buf);
        return this->current_output_buf;
      }


      // Return the user buffer only if rotation was successful
      if (rotated == this->current_output_buf) {
        return this->current_output_buf;
      }
    }

  } else {
    LOGE("Dequeued empty capture buffer %d", n);
  }

  return nullptr;
}

bool MsmVidc::handleOutput() {
  struct v4l2_buffer buf = {0};
	struct v4l2_plane planes[OUT_PLANES];

	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.m.planes = planes;
	buf.length = OUT_PLANES;

  checked_ioctl(this->fd, VIDIOC_DQBUF, &buf);
  this->out_buf_flag[buf.index] = false; // mark as not queued
  return true;
}

bool MsmVidc::handleEvent() {
  // dequeue event
  struct v4l2_event event = {0};
  checked_ioctl(this->fd, VIDIOC_DQEVENT, &event);
  switch (event.type) {
    case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT: {
      unsigned int *ptr = (unsigned int *)event.u.data;
      unsigned int height = ptr[0];
      unsigned int width = ptr[1];

      assert(ptr[2] == 0); // depth
      assert(ptr[3] == MSM_VIDC_PIC_STRUCT_PROGRESSIVE); // progressive
      // don't care about the rest of the data
      this->w = width;
      this->h = height;
      LOG("Port Reconfig received insufficient, new size %ux%u, flushing capture bufs...",
		     width, height);
      struct v4l2_decoder_cmd dec;
      dec.flags = V4L2_QCOM_CMD_FLUSH_CAPTURE;
      dec.cmd = V4L2_QCOM_CMD_FLUSH;
      checked_ioctl(this->fd, VIDIOC_DECODER_CMD, &dec); // Now we need to wait for the flush done event
      this->reconfigure_pending = true; // reconfigure the capture queue when flush done event is received
      LOGD("Waiting for flush done event to reconfigure capture queue");
      break;
    }

    case V4L2_EVENT_MSM_VIDC_FLUSH_DONE: {
      unsigned int *ptr = (unsigned int *)event.u.data;
      unsigned int flags = ptr[0];
		  if (flags & V4L2_QCOM_CMD_FLUSH_CAPTURE) {
			  LOGD("Flush Done received on CAPTURE queue");
        if (this->reconfigure_pending) {
          LOGD("Reconfiguring capture queue after flush done");
          this->restartCapture(); // reconfigure the capture queue
          this->reconfigure_pending = false;
        }
      }
      break;
    }

    case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT:
    case V4L2_EVENT_MSM_VIDC_SYS_ERROR:
    case V4L2_EVENT_MSM_VIDC_HW_OVERLOAD:
    case V4L2_EVENT_MSM_VIDC_HW_UNSUPPORTED:
    case V4L2_EVENT_MSM_VIDC_RELEASE_BUFFER_REFERENCE:
    case V4L2_EVENT_MSM_VIDC_RELEASE_UNQUEUED_BUFFER:
      LOGD("Received event type %d", event.type);
      break;
    default:
      LOGW("Received unknown event type %d", event.type);
      break;
  }
  return true;
}