#pragma once

#include <sys/signalfd.h>
#include "third_party/linux/include/v4l2-controls.h"
#include <signal.h>

#include "third_party/linux/include/videodev2.h"


const int event_type[] = {
	V4L2_EVENT_MSM_VIDC_FLUSH_DONE,
	V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT,
	V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT,
	V4L2_EVENT_MSM_VIDC_SYS_ERROR,
	V4L2_EVENT_MSM_VIDC_HW_OVERLOAD,
	V4L2_EVENT_MSM_VIDC_HW_UNSUPPORTED,
	V4L2_EVENT_MSM_VIDC_RELEASE_BUFFER_REFERENCE,
	V4L2_EVENT_MSM_VIDC_RELEASE_UNQUEUED_BUFFER,
};

enum {
	EV_VIDEO,
	EV_DISPLAY,
	EV_SIGNAL,
	EV_COUNT
};

// const char *buf_type_to_string(enum v4l2_buf_type type)
// {
// 	switch (type) {
// 	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
// 	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
// 		return "OUTPUT";
// 	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
// 	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
// 		return "CAPTURE";
// 	default:
// 		return "??";
// 	}
// }

// int kbd_init(struct instance *i)
// {
// 	struct termios newt;

// 	if (tcgetattr(STDIN_FILENO, &i->stdin_termios) < 0)
// 		return -1;

// 	newt = i->stdin_termios;
// 	newt.c_lflag &= ~ICANON;
// 	newt.c_lflag &= ~ECHO;

// 	if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) < 0)
// 		return -1;

// 	i->stdin_valid = 1;

// 	return STDIN_FILENO;
// }

// int kbd_handle_key(struct instance *i)
// {
// 	uint8_t key[3];
// 	int ret;

// 	ret = read(STDIN_FILENO, key, 3);
// 	if (ret < 0)
// 		return -1;

// 	if (key[0] == 's') {
// 		info("Frame Step");
// 		i->prerolled = 0;
// 	}

// 	return 0;
// }

// int handle_signal(struct instance *i)
// {
// 	struct signalfd_siginfo siginfo;
// 	sigset_t sigmask;

// 	if (read(i->sigfd, &siginfo, sizeof (siginfo)) < 0) {
// 		perror("signalfd/read");
// 		return -1;
// 	}

// 	sigemptyset(&sigmask);
// 	sigaddset(&sigmask, siginfo.ssi_signo);
// 	sigprocmask(SIG_UNBLOCK, &sigmask, NULL);

// 	i->finish = 1;

// 	return 0;
// }
