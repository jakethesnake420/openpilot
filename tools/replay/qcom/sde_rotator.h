#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <poll.h>
#include "common/util.h"
#include "common/swaglog.h"
#include "msgq/visionipc/visionbuf.h"

#define ROTATOR_DEVICE "/dev/video2"


class SdeRotator {
public:
  SdeRotator();
  ~SdeRotator();
  int config_ubwc_to_nv12_op(int width, int height);
  int put_frame(VisionBuf *input_frame);
  int get_frame(unsigned char **linear_data, size_t *linear_size, int timeout_ms);
  bool queued = false;
  void publish_frame();
  bool init(const char *dev = ROTATOR_DEVICE);

private:
  int fd;
  void *linear_ptr;
  size_t mapped_size;
  struct v4l2_format fmt_cap = {0}, fmt_out = {0};
  struct v4l2_buffer cached_cap_buf = {0};
  VisionBuf vision_buf;
  struct pollfd pfd;

  int cleanup();
};
