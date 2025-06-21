#include <sys/signalfd.h>

//#include <sys/ioctl.h>
#include <assert.h>
#include "decoder.h"
#include "common/swaglog.h"
#include "common/util.h"



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
    LOGE("failed to open video device");
    return false;
  }
  struct v4l2_capability cap = {0};
  checked_ioctl(fd, VIDIOC_QUERYCAP, &cap);
  subscribeEvents();

  v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

  setPlaneFormat(out_type, V4L2_PIX_FMT_HEVC); // Also allocates the output buffer.
  setFPS(20);
  request_buffers(fd, out_type, OUTPUT_BUFFER_COUNT);
  setControls();
  checked_ioctl(fd, VIDIOC_STREAMON, &out_type);
  restartCapture();
  setupPolling();
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
    assert(2 == fmt.fmt.pix_mp.num_planes); // These may not be the same, but they should be
    LOGW(" this->cap_planes_count = 2, need to check what going on here");
    this->cap_planes_count = 2;
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
  struct v4l2_plane planes[2] = {0};

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
  struct v4l2_plane planes[1] = {0};

  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory = V4L2_MEMORY_USERPTR;
  buf.index = i;
  buf.m.planes = planes;
  buf.length = 1;
  // decoded frame plane
  LOGW("Check if out_buf_off[i] is correct here");
  planes[0].m.userptr = (unsigned long)this->out_buf_off[i]; // check this
  planes[0].length = this->out_buf_size;
  planes[0].reserved[0] = this->out_buf.fd; // ION fd
  planes[0].reserved[1] = 0;
  planes[0].bytesused = size;
  planes[0].data_offset = 0;

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
	//uint64_t pts, dts, duration, start_time = TIMESTAMP_NONE;
	int flags;
	size_t size = 0;
	uint8_t *data;
	//AVRational vid_timebase;
	//AVRational v4l_timebase = { 1, 1000000 };
	data = (uint8_t *)this->out_buf_addr[buf_index];
  // Would send sequence header here if needed

  memcpy(data + size, pkt->data, pkt->size);
  size += pkt->size;
  flags = 0;
  // time base is unknown here
  //vid_timebase = {1, 30}; // TODO: get from stream
  flags |= V4L2_QCOM_BUF_TIMESTAMP_INVALID;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  // if ((pkt->flags & AV_PKT_FLAG_KEY) &&
  //     pts != TIMESTAMP_NONE && dts != TIMESTAMP_NONE) {
  //   this->pts_dts_delta = pts - dts;
  // }
  queueOutputBuffer(buf_index, size, flags, tv);

  //tsInsert(pkt->pts, pkt->dts, pkt->duration, pkt->start_time);
  return true;
}

void MsmVidc::tsInsert(uint64_t pts, uint64_t dts, uint64_t duration, uint64_t start_time) {
  // For tracking timestamps of frames.
  pending_ts_list.push_back(pts);
  if (pts != TIMESTAMP_NONE) {
    cap_last_pts = pts;
  }
  pts_dts_delta = pts - dts; // TODO: check if this is correct
  LOGD("Inserted timestamp: pts=%" PRIu64 ", dts=%" PRIu64 ", duration=%" PRIu64 ", start_time=%" PRIu64, pts, dts, duration, start_time);
}

int MsmVidc::getBufferUnlocked() {
  for (int i = 0; i < this->out_buf_cnt; i++) {
    if (!cap_buf_flag[i]) {
      return i;
    }
  }
  return -1;
}

bool MsmVidc::decodeFrame(AVPacket *pkt, VisionBuf *buf) {
  assert(initialized && pkt != nullptr && buf != nullptr);
  int buf_index = getBufferUnlocked();
  assert(buf_index >= 0 && buf_index < out_buf_cnt);
  sendPacket(buf_index, pkt);

  // Wait for the output buffer to be filled
  while (!out_buf_flag[buf_index]) {
    // Polling or waiting logic here
  }

  // Copy the output buffer to the provided VisionBuf
  memcpy(buf->addr, out_buf_addr[buf_index], out_buf_size);
  buf->len = out_buf_size;

  out_buf_flag[buf_index] = false; // Mark as not queued anymore
  return buf->addr;
}