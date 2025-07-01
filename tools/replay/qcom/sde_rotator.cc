#include "sde_rotator.h"
#include "third_party/linux/include/msm_media_info.h"
#include "common/swaglog.h"
#include <cstdio>
#include <linux/ion.h>
#include <msm_ion.h>

#ifndef V4L2_PIX_FMT_NV12_UBWC
#define V4L2_PIX_FMT_NV12_UBWC v4l2_fourcc('Q', '1', '2', '8')
#endif

static void checked_ioctl(int fd, unsigned long request, void *argp) {
  int ret = util::safe_ioctl(fd, request, argp);
  if (ret != 0) {
    LOGE("checked_ioctl failed with error %d (%d %lx %p)", errno, fd, request, argp);
    assert(0);
  }
}

static void request_buffers(int fd, v4l2_buf_type buf_type, unsigned int count) {
  struct v4l2_requestbuffers reqbuf = {
    .count = count,
    .type = buf_type,
    .memory = V4L2_MEMORY_USERPTR
  };
  checked_ioctl(fd, VIDIOC_REQBUFS, &reqbuf);
}


SdeRotator::SdeRotator() {}

bool SdeRotator::init(const char *dev) {
  LOGD("Initializing sde_rot device %s", dev);
  fd = open(dev, O_RDWR, 0);
  if (fd < 0) {
    LOGE("failed to open rotator device");
    return false;
  }
  fmt_cap = {}, fmt_out = {}, cap_desc = {};
  pfd = { .fd = fd, .events = POLLIN | POLLRDNORM, .revents = 0 };
  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  checked_ioctl(fd, VIDIOC_QUERYCAP, &cap); // check if this needed.
  return true;
}


SdeRotator::~SdeRotator() {
  cleanup();
}

/**
 * @brief Configures the SdeRotator operation for the specified frame dimensions.
 *
 * This function sets up the video output and capture formats, allocates and manages
 * the necessary buffers, and starts streaming on both output and capture devices.
 * It ensures that any previously allocated ION buffer is freed and unmapped if the
 * size has changed, and allocates a new buffer for the current configuration.
 * The function also queries and caches the capture buffer information after allocation.
 *
 * @param width  The width of the video frame to configure.
 * @param height The height of the video frame to configure.
 * @return int Returns 0 on successful configuration, or asserts on failure.
 */
int SdeRotator::configUBWCtoNV12(int width, int height) {
  // stop streaming if already started
  enum v4l2_buf_type t;
  t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  checked_ioctl(fd, VIDIOC_STREAMOFF, &t);
  t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  checked_ioctl(fd, VIDIOC_STREAMOFF, &t);
  LOG("Configuring rotator for width=%d height=%d", width, height);
  queued = false;
  fmt_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt_out.fmt.pix.width       = width;
  fmt_out.fmt.pix.height      = height;
  fmt_out.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12_UBWC;
  fmt_out.fmt.pix.field       = V4L2_FIELD_NONE;
  checked_ioctl(fd, VIDIOC_S_FMT, &fmt_out);

  fmt_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt_cap.fmt.pix.width       = width;
  fmt_cap.fmt.pix.height      = height;
  fmt_cap.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
  fmt_cap.fmt.pix.field       = V4L2_FIELD_NONE;
  checked_ioctl(fd, VIDIOC_S_FMT, &fmt_cap);

  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1);
  if (cap_buf.fd >= 0) {
    munmap(cap_buf.addr, cap_buf.len);
    cap_buf.free();
  }
  cap_buf = VisionBuf();
  cap_buf.allocate_no_cache(fmt_cap.fmt.pix.sizeimage);
  cap_buf.addr = mmap(nullptr,
                      fmt_cap.fmt.pix.sizeimage,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED,
                      cap_buf.fd,
                      0);
  assert(cap_buf.addr != MAP_FAILED);
  cap_buf.init_yuv(fmt_cap.fmt.pix.width, fmt_cap.fmt.pix.height,
                    fmt_cap.fmt.pix.bytesperline, fmt_cap.fmt.pix.bytesperline * fmt_cap.fmt.pix.height);

  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1);

  // Query and cache capture buffer info (only needed after (re)alloc)
  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap_desc.memory = V4L2_MEMORY_USERPTR;
  cap_desc.index  = 0;
  checked_ioctl(fd, VIDIOC_QUERYBUF, &cap_desc);

  // Only streamon after (re)configuration
  t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  checked_ioctl(fd, VIDIOC_STREAMON, &t);
  t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  checked_ioctl(fd, VIDIOC_STREAMON, &t);

  return 0;
}

int SdeRotator::configUBWCtoNV12WithOutputBuf(int width, int height, VisionBuf *output_buf) {
  // stop streaming if already started
  enum v4l2_buf_type t;
  t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  checked_ioctl(fd, VIDIOC_STREAMOFF, &t);
  t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  checked_ioctl(fd, VIDIOC_STREAMOFF, &t);
  LOG("Configuring rotator for input=%dx%d output=%dx%d with external output buffer",
      width, height, width, height);
  queued = false;

  fmt_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt_out.fmt.pix.width       = width;           // Input: decoder size (1952x1216)
  fmt_out.fmt.pix.height      = height;
  fmt_out.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12_UBWC;
  fmt_out.fmt.pix.field       = V4L2_FIELD_NONE;
  checked_ioctl(fd, VIDIOC_S_FMT, &fmt_out);

  struct v4l2_cropcap cropcap;

  memset(&cropcap, 0, sizeof (cropcap));
  cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  // Set crop rectangle to extract the valid region
  struct v4l2_crop crop;
  crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;  // Crop the input
  crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  crop.c.left = (width - output_buf->width) / 2;    // (1952-1928)/2 = 12
  crop.c.top = (height - output_buf->height) / 2;   // (1216-1208)/2 = 4
  crop.c.width = output_buf->width;                  // 1928
  crop.c.height = output_buf->height;                // 1208
  checked_ioctl(fd, VIDIOC_S_CROP, &crop);

  fmt_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt_cap.fmt.pix.width       = output_buf->width;   // Use output buffer width (1928)
  fmt_cap.fmt.pix.height      = output_buf->height;  // Use user buffer height (1208)
  fmt_cap.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
  fmt_cap.fmt.pix.field       = V4L2_FIELD_NONE;
  checked_ioctl(fd, VIDIOC_S_FMT, &fmt_cap);

  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1);

  // Don't allocate our own cap_buf, use the provided output_buf instead
  // Free any existing cap_buf since we won't use it
  // if (cap_buf.fd >= 0) {
  //   munmap(cap_buf.addr, cap_buf.len);
  //   cap_buf.free();
  // }
  // cap_buf = VisionBuf(); // Reset to empty state

  // Store reference to the external buffer
  external_output_buf = output_buf;

  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1);

  // Query and cache capture buffer info (only needed after (re)alloc)
  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap_desc.memory = V4L2_MEMORY_USERPTR;
  cap_desc.index  = 0;
  checked_ioctl(fd, VIDIOC_QUERYBUF, &cap_desc);

  // Only streamon after (re)configuration
  t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  checked_ioctl(fd, VIDIOC_STREAMON, &t);
  t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  checked_ioctl(fd, VIDIOC_STREAMON, &t);

  return 0;
}

int SdeRotator::putFrameWithOutputBuf(VisionBuf *ubwc, VisionBuf *output_buf)
{
  /* re-configure if size changed or different output buffer */
  if (
      external_output_buf != output_buf) {
      configUBWCtoNV12WithOutputBuf(ubwc->width, ubwc->height, output_buf);
  }

  /* OUTPUT (UBWC) */
  struct v4l2_buffer out = {};
  out.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  out.memory    = V4L2_MEMORY_USERPTR;
  out.index     = 0;
  out.m.userptr = static_cast<unsigned long>(ubwc->fd);
  out.length    = fmt_out.fmt.pix.sizeimage;
  checked_ioctl(fd, VIDIOC_QBUF, &out);

  /* CAPTURE (linear NV12) – use external output buffer */
  struct v4l2_buffer cap = cap_desc;
  cap.m.userptr = static_cast<unsigned long>(output_buf->fd);
  checked_ioctl(fd, VIDIOC_QBUF, &cap);

  queued = true;
  return 0;
}

VisionBuf* SdeRotator::getFrameExternal(int timeout_ms /* =100 */)
{
  if (!queued || !external_output_buf)
      return nullptr;

  if (poll(&pfd, 1, timeout_ms) <= 0)
      return nullptr;

  /* dequeue CAPTURE */
  struct v4l2_buffer cap = {};
  cap.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap.memory = V4L2_MEMORY_USERPTR;
  checked_ioctl(fd, VIDIOC_DQBUF, &cap);

  /* dequeue OUTPUT (frees the slot) */
  struct v4l2_buffer out = {};
  out.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  out.memory = V4L2_MEMORY_USERPTR;
  checked_ioctl(fd, VIDIOC_DQBUF, &out);

  queued = false;
  return external_output_buf; // Return the user-provided buffer
}

void SdeRotator::convertStride(VisionBuf *rotated_buf, VisionBuf *user_buf) {
  LOGD("=== STRIDE CONVERSION DEBUG ===");
  LOGD("SRC: w=%zu h=%zu stride=%zu", rotated_buf->width, rotated_buf->height, rotated_buf->stride);
  LOGD("DST: w=%zu h=%zu stride=%zu", user_buf->width, user_buf->height, user_buf->stride);
  LOGD("SRC Y=%p UV=%p (offset=%zu)", rotated_buf->y, rotated_buf->uv,
       (uint8_t*)rotated_buf->uv - (uint8_t*)rotated_buf->y);
  LOGD("DST Y=%p UV=%p (offset=%zu)", user_buf->y, user_buf->uv,
       (uint8_t*)user_buf->uv - (uint8_t*)user_buf->y);

  // Copy Y plane row by row
  for (int y = 0; y < user_buf->height; y++) {
    uint8_t *src_row = (uint8_t*)rotated_buf->y + y * rotated_buf->stride;
    uint8_t *dst_row = (uint8_t*)user_buf->y + y * user_buf->stride;
    memcpy(dst_row, src_row, user_buf->width);  // Copy only actual width
  }

  // Copy UV plane row by row (NV12: height/2)
  for (int y = 0; y < user_buf->height / 2; y++) {
    uint8_t *src_row = (uint8_t*)rotated_buf->uv + y * rotated_buf->stride;
    uint8_t *dst_row = (uint8_t*)user_buf->uv + y * user_buf->stride;
    memcpy(dst_row, src_row, user_buf->width);  // Copy only actual width
  }
  LOGD("=== STRIDE CONVERSION COMPLETE ===");
}


int SdeRotator::cleanup() {
  int err = 0;
  if (fd >= 0) {
    err = close(fd);
    fd = -1;
  }
  cap_buf.free();
  cap_buf.~VisionBuf();
  new (&cap_buf) VisionBuf();
  queued = false;
  return err;
}

int SdeRotator::putFrame(VisionBuf *ubwc)
{
  /* re-configure if size changed – same as before */
  if (ubwc->width != cap_buf.width || ubwc->height != cap_buf.height)
      configUBWCtoNV12(ubwc->width, ubwc->height);

  /* OUTPUT (UBWC) */
  struct v4l2_buffer out = {};
  out.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  out.memory    = V4L2_MEMORY_USERPTR;
  out.index     = 0;
  out.m.userptr = static_cast<unsigned long>(ubwc->fd);
  out.length    = fmt_out.fmt.pix.sizeimage;
  checked_ioctl(fd, VIDIOC_QBUF, &out);

  /* CAPTURE (linear NV12) – use previously cached cap_desc */
  struct v4l2_buffer cap = cap_desc;
  cap.m.userptr = static_cast<unsigned long>(cap_buf.fd);
  checked_ioctl(fd, VIDIOC_QBUF, &cap);

  queued = true;
  return 0;
}


VisionBuf* SdeRotator::getFrame(int timeout_ms /* =100 */)
{
  if (!queued)                       // nothing in flight
      return nullptr;

  if (poll(&pfd, 1, timeout_ms) <= 0)   // timeout or error
      return nullptr;

  /* dequeue CAPTURE */
  struct v4l2_buffer cap = {};
  cap.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap.memory = V4L2_MEMORY_USERPTR;
  checked_ioctl(fd, VIDIOC_DQBUF, &cap);

  /* dequeue OUTPUT (frees the slot) */
  struct v4l2_buffer out = {};
  out.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  out.memory = V4L2_MEMORY_USERPTR;
  checked_ioctl(fd, VIDIOC_DQBUF, &out);

  queued = false;                    // ready for next putFrame()
  return &cap_buf;                   // << NO COPY, already mapped
}

int SdeRotator::saveFrame(const char* filename, bool append) {
  if (!cap_buf.addr || cap_buf.len == 0) {
    LOGE("No frame data available to save");
    return -1;
  }

  // Print detailed buffer information for debugging
  LOGD("=== Buffer Debug Info ===");
  LOGD("cap_buf.len: %zu bytes", cap_buf.len);
  LOGD("fmt_cap.fmt.pix.sizeimage: %d bytes", fmt_cap.fmt.pix.sizeimage);
  LOGD("fmt_cap.fmt.pix.width: %d", fmt_cap.fmt.pix.width);
  LOGD("fmt_cap.fmt.pix.height: %d", fmt_cap.fmt.pix.height);
  LOGD("fmt_cap.fmt.pix.bytesperline: %d", fmt_cap.fmt.pix.bytesperline);

  // Open in append mode if requested, otherwise overwrite
  const char* mode = append ? "ab" : "wb";
  FILE* fp = fopen(filename, mode);
  if (!fp) {
    LOGE("Failed to open file %s for writing: %s", filename, strerror(errno));
    return -1;
  }

  // Use the V4L2 format sizeimage instead of cap_buf.len
  size_t bytes_to_write = fmt_cap.fmt.pix.sizeimage;
  LOGD("About to write %zu bytes to file (%s mode)", bytes_to_write, append ? "append" : "overwrite");

  size_t bytes_written = fwrite(cap_buf.addr, 1, bytes_to_write, fp);
  LOGD("fwrite() returned %zu bytes written", bytes_written);

  int fflush_result = fflush(fp);
  LOGD("fflush() returned %d", fflush_result);

  int fclose_result = fclose(fp);
  LOGD("fclose() returned %d", fclose_result);

  // Check actual file size after writing
  struct stat st;
  if (stat(filename, &st) == 0) {
    LOGD("Actual file size on disk: %ld bytes", st.st_size);
  } else {
    LOGE("Failed to stat file: %s", strerror(errno));
  }

  if (bytes_written != bytes_to_write) {
    LOGE("Failed to write complete frame data. Expected %zu bytes, wrote %zu bytes",
         bytes_to_write, bytes_written);
    return -1;
  }

  LOGD("Successfully %s frame to %s (%zu bytes)",
       append ? "appended" : "saved", filename, bytes_written);

  if (!append) {
    // Only show ffmpeg command for single frame
    LOGD("Use ffmpeg: ffmpeg -y -f rawvideo -pix_fmt nv12 -s %dx%d -i %s -frames:v 1 output.png",
         fmt_cap.fmt.pix.width, fmt_cap.fmt.pix.height, filename);
  } else {
    // For multiple frames, calculate frame count
    long frame_count = st.st_size / bytes_to_write;
    LOGD("File now contains %ld frames", frame_count);
    LOGD("Use ffmpeg for video: ffmpeg -y -f rawvideo -pix_fmt nv12 -s %dx%d -r 25 -i %s output.mp4",
         fmt_cap.fmt.pix.width, fmt_cap.fmt.pix.height, filename);
  }

  return 0;
}

static int alloc_ion_buffer(size_t size, uint32_t flags)
{
	struct ion_allocation_data ion_alloc = { 0 };
	struct ion_fd_data ion_fd_data = { 0 };
	struct ion_handle_data ion_handle_data = { 0 };
	static int ion_fd = -1;
	int ret;

	if (ion_fd < 0) {
		ion_fd = open("/dev/ion", O_RDONLY);
		if (ion_fd < 0) {
			LOGE("Cannot open ion device: %m");
			return -1;
		}
	}

	ion_alloc.handle = -1;
	ion_alloc.len = size;
	ion_alloc.align = 4096;
	ion_alloc.flags = flags;
	ion_alloc.heap_id_mask = 0;

  ion_alloc.heap_id_mask = ION_HEAP(ION_IOMMU_HEAP_ID);

	if (ioctl(ion_fd, ION_IOC_ALLOC, &ion_alloc) < 0) {
		LOGE("Failed to allocate ion buffer: %m");
		return -1;
	}

	LOGD("Allocated %zd bytes ION buffer %d",
	    ion_alloc.len, ion_alloc.handle);

	ion_fd_data.handle = ion_alloc.handle;
	ion_fd_data.fd = -1;

	if (ioctl(ion_fd, ION_IOC_MAP, &ion_fd_data) < 0) {
		LOGE("Failed to map ion buffer: %m");
		ret = -1;
	} else {
		ret = ion_fd_data.fd;
	}

	ion_handle_data.handle = ion_alloc.handle;
	if (ioctl(ion_fd, ION_IOC_FREE, &ion_handle_data) < 0)
		LOGE("Failed to free ion buffer: %m");

	return ret;
}


int SdeRotator::convert_ubwc_to_linear(int out_buf_fd,
                          int width, int height,
                          unsigned char **linear_data, size_t *linear_size)
{
    static int rotator_fd = -1;
    static int cap_ion_fd = -1;
    static void *linear_ptr = NULL;
    static size_t mapped_size = 0;
    static int last_width = 0, last_height = 0;

    static struct v4l2_buffer cached_cap_buf = {0};
    int ret = -1;

    // Open SDE rotator device only once
    if (rotator_fd < 0) {
        rotator_fd = open("/dev/video2", O_RDWR);
        if (rotator_fd < 0) {
            LOGE("Failed to open rotator device");
            return -1;
        }
        struct v4l2_capability cap = {};

        if (ioctl(rotator_fd, VIDIOC_QUERYCAP, &cap) < 0) {
            LOGE("Failed to verify capabilities: %m");
            close(rotator_fd);
            rotator_fd = -1;
            return -1;
        }
        LOGD("caps (/dev/video2): driver=\"%s\" bus_info=\"%s\" card=\"%s\" "
            "version=%u.%u.%u",  cap.driver, cap.bus_info, cap.card,
            (cap.version >> 16) & 0xff,
            (cap.version >> 8) & 0xff,
            cap.version & 0xff);
    }

    // Only reconfigure formats and buffers if width/height changed
    if (width != last_width || height != last_height) {
        fmt_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        fmt_out.fmt.pix.width       = width;
        fmt_out.fmt.pix.height      = height;
        fmt_out.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12_UBWC;
        fmt_out.fmt.pix.field       = V4L2_FIELD_NONE;
        ioctl(rotator_fd, VIDIOC_S_FMT, &fmt_out);

        fmt_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt_cap.fmt.pix.width       = width;
        fmt_cap.fmt.pix.height      = height;
        fmt_cap.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt_cap.fmt.pix.field       = V4L2_FIELD_NONE;
        ioctl(rotator_fd, VIDIOC_S_FMT, &fmt_cap);

        struct v4l2_requestbuffers req = {0};
        req.count  = 1;
        req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        req.memory = V4L2_MEMORY_USERPTR;
        ioctl(rotator_fd, VIDIOC_REQBUFS, &req);

        // Free previous ION buffer and unmap if size changed
        if (cap_ion_fd >= 0) {
            if (linear_ptr && mapped_size) {
                munmap(linear_ptr, mapped_size);
                linear_ptr = NULL;
                mapped_size = 0;
            }
            close(cap_ion_fd);
            cap_ion_fd = -1;
        }
        cap_ion_fd = alloc_ion_buffer(fmt_cap.fmt.pix.sizeimage, 0);
        if (cap_ion_fd < 0) {
            LOGE("Failed to allocate ION buffer for capture");
            return -1;
        }
        last_width = width;
        last_height = height;
        // last_sizeimage = fmt_cap.fmt.pix.sizeimage;

        struct v4l2_requestbuffers req_cap = {0};
        req_cap.count  = 1;
        req_cap.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req_cap.memory = V4L2_MEMORY_USERPTR;
        ioctl(rotator_fd, VIDIOC_REQBUFS, &req_cap);

        // Query and cache capture buffer info (only needed after (re)alloc)
        memset(&cached_cap_buf, 0, sizeof(cached_cap_buf));
        cached_cap_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        cached_cap_buf.memory = V4L2_MEMORY_USERPTR;
        cached_cap_buf.index  = 0;
        ioctl(rotator_fd, VIDIOC_QUERYBUF, &cached_cap_buf);

        // Only streamon after (re)configuration
        enum v4l2_buf_type t;
        t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(rotator_fd, VIDIOC_STREAMON, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(rotator_fd, VIDIOC_STREAMON, &t);
        // streamon_done = 1;
    }

    // Queue output buffer
    struct v4l2_buffer buf = {0};
    int sizeimage = fmt_out.fmt.pix.sizeimage;
    buf.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory    = V4L2_MEMORY_USERPTR;
    buf.index     = 0;
    buf.m.userptr = (unsigned long)out_buf_fd;
    buf.length    = sizeimage;
    ioctl(rotator_fd, VIDIOC_QBUF, &buf);

    // Queue capture buffer (use cached info)
    struct v4l2_buffer cap_buf2 = cached_cap_buf;
    cap_buf2.m.userptr = (unsigned long)cap_ion_fd;
    ioctl(rotator_fd, VIDIOC_QBUF, &cap_buf2);

    struct pollfd pfd2 = {
        .fd = rotator_fd,
        .events = POLLIN | POLLRDNORM,
    };
    ret = poll(&pfd2, 1, 2000);

    if (ret <= 0) {
        LOGE("poll() timed out or error");
        return -1;
    }

    struct v4l2_buffer dq = {
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_USERPTR
    };
    if (ioctl(rotator_fd, VIDIOC_DQBUF, &dq) < 0) {
        LOGE("CAP DQBUF");
        return -1;
    }

    struct v4l2_buffer dqout = {
        .type   = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    ioctl(rotator_fd, VIDIOC_DQBUF, &dqout);

    // Only mmap if not already mapped
    if (!linear_ptr || mapped_size != dq.length) {
        if (linear_ptr && mapped_size)
            munmap(linear_ptr, mapped_size);
        linear_ptr = mmap(NULL, dq.length,
                        PROT_READ|PROT_WRITE,
                        MAP_SHARED,
                        cap_ion_fd, 0);
        if (linear_ptr == MAP_FAILED) {
            LOGE("mmap CAP");
            linear_ptr = NULL;
            mapped_size = 0;
            return -1;
        }
        mapped_size = dq.length;
    }

    *linear_data = (unsigned char *)linear_ptr;
    *linear_size = dq.length;


    return 0;
}

