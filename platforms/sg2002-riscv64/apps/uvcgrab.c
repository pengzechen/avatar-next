#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "avatar_uvc.h"

#define FRAME_BUF_SIZE (512u * 1024u)

static unsigned char g_frame_buf[FRAME_BUF_SIZE];

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *out_path = argc > 1 ? argv[1] : "/tmp/uvc-frame.jpg";

    int fd = open("/dev/video0", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/video0");
        return 1;
    }

    struct avatar_uvc_cap cap;
    if (ioctl(fd, AV_UVC_IOC_QUERYCAP, &cap) == 0) {
        printf("camera: %s vid=0x%04x pid=0x%04x addr=%u\n",
               cap.card, cap.vid, cap.pid, cap.dev_addr);
    }

    struct avatar_uvc_format fmt;
    if (ioctl(fd, AV_UVC_IOC_G_FMT, &fmt) == 0) {
        printf("format: %ux%u MJPEG payload=%u frame_max=%u\n",
               fmt.width, fmt.height, fmt.max_payload_size, fmt.max_frame_size);
    }

    struct avatar_uvc_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.buf = (uint64_t)(uintptr_t)g_frame_buf;
    frame.buf_size = sizeof(g_frame_buf);

    size_t bytes = 0;
    if (ioctl(fd, AV_UVC_IOC_GET_FRAME, &frame) == 0) {
        bytes = frame.bytes_used;
        printf("captured: %u bytes seq=%u transfers=%u data_packets=%u fid=%u\n",
               frame.bytes_used, frame.sequence, frame.transfers,
               frame.data_packets, frame.fid);
    } else {
        ssize_t n = read(fd, g_frame_buf, sizeof(g_frame_buf));
        if (n < 0) {
            perror("capture frame");
            close(fd);
            return 1;
        }
        bytes = (size_t)n;
        printf("captured via read: %zu bytes\n", bytes);
    }

    close(fd);

    if (bytes < 4 || g_frame_buf[0] != 0xff || g_frame_buf[1] != 0xd8 ||
        g_frame_buf[bytes - 2] != 0xff || g_frame_buf[bytes - 1] != 0xd9) {
        fprintf(stderr, "warning: frame does not look like a complete JPEG\n");
    }

    int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        perror("open output");
        return 1;
    }

    if (write_all(out, g_frame_buf, bytes) != 0) {
        perror("write output");
        close(out);
        return 1;
    }

    close(out);
    printf("saved: %s (%zu bytes)\n", out_path, bytes);
    return 0;
}