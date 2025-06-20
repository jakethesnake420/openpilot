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

bool MsmVidc::init(const char* dev) {
  LOGD("Initializing msm_vidc device %s", dev);
  fd = open(dev, O_RDWR, 0);
  if (fd < 0) {
    LOGE("failed to open video device");
    return false;
  }
  struct v4l2_capability cap = {0};
  checked_ioctl(fd, VIDIOC_QUERYCAP, &cap);
  subscribeEvents();

  assert(this->width && this->height && this->codec); // TODO: set width, height, codec
  v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  setPlaneFormat(out_type, V4L2_PIX_FMT_HEVC); // Also allocates the output buffer.
  setFPS(20);
  request_buffers(fd, out_type, OUTPUT_BUFFER_COUNT);
  setControls();
  checked_ioctl(fd, VIDIOC_STREAMON, &out_type);
  restartCapture();


  // setup polling
  sigset_t sigmask;
  sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);
  sigfd = signalfd(-1, &sigmask, SFD_CLOEXEC);
  if (sigfd < 0) {
    LOGE("failed to create signalfd");
    return false;
  }
  sigprocmask(SIG_BLOCK, &sigmask, nullptr);

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
  for (uint32_t e : event_type) {
    struct v4l2_event_subscription sub = { .type = e};
    checked_ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
  }
  return true;
}

bool MsmVidc::setPlaneFormat(enum v4l2_buf_type type, uint32_t fourcc) {
  struct v4l2_format fmt = {.type = type};
  struct v4l2_pix_format_mplane *pix = &fmt.fmt.pix_mp;
  *pix = { .width = (__u32)this->width, .height = (__u32)this->height, .pixelformat = fourcc };
  checked_ioctl(fd, VIDIOC_S_FMT, &fmt);

  LOG("This is not the same as the working implementation, need to check if this is correct");
  if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
    this->out_buf_size = pix->plane_fmt[0].sizeimage;
    int ion_size = this->out_buf_size * OUTPUT_BUFFER_COUNT; // Output (input) buffers are ION buffer.
    this->out_buf.allocate(ion_size); // We need to manage the cache and free the buffer later.
  } else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    request_buffers(this->fd, type, CAPTURE_BUFFER_COUNT);
    checked_ioctl(fd, VIDIOC_G_FMT, &fmt);
    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
      this->cap_bufs[i].allocate(pix->plane_fmt[0].sizeimage); // Note that this is mmapped rw which is different from the working implementation.
      this->cap_bufs[i].init_yuv(pix->width, pix->height, pix->plane_fmt[0].bytesperline, 0);
    }
    this->cap_buf_format = pix->pixelformat;
    assert(pix->pixelformat == v4l2_fourcc('Q', '1', '2', '8')); // should be NV12 UBWC
    assert(CAPTURE_BUFFER_COUNT == fmt.fmt.pix_mp.num_planes); // These may not be the same, but they should be
    //this->cap_planes_count = 1;
    // alloc extra buffer for extradata becasue we know the size from pix structure right now
    this->ext_buf.allocate(pix->plane_fmt[1].sizeimage * CAPTURE_BUFFER_COUNT);
    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
      size_t offset = i * pix->plane_fmt[1].sizeimage;
      this->ext_buf_off[i] = offset;
      this->ext_buf_addr[i] = (char *)this->ext_buf.addr + offset;
    }
  }
  return true;
}

bool MsmVidc::setFPS(uint32_t fps) {
  struct v4l2_streamparm streamparam = {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .parm.output.timeperframe = {1, fps}
  };
  checked_ioctl(fd, VIDIOC_S_PARM, &streamparam);
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
  return true;
}

bool MsmVidc::restartCapture() {
  // stop capture
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  checked_ioctl(this->fd, VIDIOC_STREAMOFF, &type);
  struct v4l2_requestbuffers reqbuf = {.memory = V4L2_MEMORY_USERPTR, .type = type};
  checked_ioctl(this->fd, VIDIOC_REQBUFS, &reqbuf);
  for (size_t i = 0; i < CAPTURE_BUFFER_COUNT; ++i) {
    this->cap_bufs[i].free();
    this->cap_buf_flag[i] = false; // mark as not queued
    cap_bufs[i].~VisionBuf();
    new (&cap_bufs[i]) VisionBuf();
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

  planes[0].m.userptr = (unsigned long)this->cap_bufs[i].addr; // no security
  planes[0].length = this->cap_bufs[i].len;
  planes[0].reserved[0] = this->cap_bufs[i].fd; // ION fd
  planes[0].reserved[1] = 0; // reserved
  planes[0].bytesused = this->cap_bufs[i].len;
  planes[0].data_offset = 0;
  checked_ioctl(this->fd, VIDIOC_QBUF, &buf);
  this->cap_buf_flag[i] = true; // mark as queued
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
  return true;
}