/*
 *  V4L2 video capture example
 *
 *  This program can be used and distributed without restrictions.
 *
 *      This program is provided with the V4L2 API
 * see http://linuxtv.org/docs.php for more information
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getopt.h> /* getopt_long() */
#include <time.h> /* clock_gettime() */

#include <errno.h>
#include <fcntl.h> /* low-level i/o */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "common.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))

#define SET_QUEUED(buffer) ((buffer).flags |= V4L2_BUF_FLAG_QUEUED)

#define IS_QUEUED(buffer) \
    ((buffer).flags & (V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_DONE))

struct buffer {
    void* start;
    size_t length;
    size_t bytesused;
};

static char* dev_name;
static int fd = -1;
struct buffer* buffers;
static unsigned int n_buffers;
static int frame_count = 70;

static void errno_exit(const char* s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static int xioctl(int fh, unsigned long int request, void* arg)
{
    int r;

    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static int read_frame(void)
{
    char strbuf[1024];
    struct v4l2_buffer buf;
    unsigned int i;

    CLEAR(buf);

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
        switch (errno) {
        case EAGAIN:
            return 0;

        case EIO:
            /* Could ignore EIO, see spec. */

            /* fall through */

        default:
            errno_exit("VIDIOC_DQBUF");
        }
    }

    printf("MMAP\t%s\n",
        snprintf_buffer(strbuf, sizeof(strbuf), &buf));
    fflush(stdout);
    assert(buf.index < n_buffers);

    if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
        errno_exit("VIDIOC_QBUF");

    return 1;
}

static void mainloop(void)
{
    unsigned int count;
    int keep_running = 1;

    count = frame_count;

    while (1) {
        if (count < 1)
            break;
        if (frame_count >= 0) {
            count--;
        }

        for (;;) {
            fd_set fds;
            struct timeval tv;
            int r;

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout. */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(fd + 1, &fds, NULL, NULL, &tv);

            if (-1 == r) {
                if (EINTR == errno)
                    continue;
                errno_exit("select");
            }

            if (0 == r) {
                fprintf(stderr, "select timeout\n");
                exit(EXIT_FAILURE);
            }

            if (read_frame())
                break;
            /* EAGAIN - continue select loop. */
        }
    }
}

static void stop_capturing(void)
{
    enum v4l2_buf_type type;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
        errno_exit("VIDIOC_STREAMOFF");
}

static void start_capturing(void)
{
    unsigned int i;
    enum v4l2_buf_type type;

    for (i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;

        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
        errno_exit("VIDIOC_STREAMON");
}

static void uninit_device(void)
{
    unsigned int i;

    for (i = 0; i < n_buffers; ++i)
        if (-1 == munmap(buffers[i].start, buffers[i].length))
            errno_exit("munmap");
    free(buffers);
}

static void init_mmap(void)
{
    char strbuf[1024];
    const int count = 4;
    struct v4l2_requestbuffers req;

    CLEAR(req);

    req.count = count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            fprintf(stderr,
                "%s does not support "
                "memory mapping\n",
                dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }
    printf("requested %d buffers, got %d\n", count, req.count);

    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory on %s\n", dev_name);
        exit(EXIT_FAILURE);
    }

    buffers = calloc(req.count, sizeof(*buffers));

    if (!buffers) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        struct v4l2_buffer buf;

        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;

        if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
            errno_exit("VIDIOC_QUERYBUF");
        printf("requested buffer %d/%d: %s\n", n_buffers, count,
            snprintf_buffer(strbuf, sizeof(strbuf), &buf));

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = mmap(NULL /* start anywhere */, buf.length,
            PROT_READ | PROT_WRITE /* required */,
            MAP_SHARED /* recommended */, fd, buf.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start)
            errno_exit("mmap");
    }
}

static void init_device(void)
{
    char strbuf[1024];
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    struct v4l2_format fmt;

    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s is no V4L2 device\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_QUERYCAP");
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s is no video capture device\n", dev_name);
        exit(EXIT_FAILURE);
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s does not support streaming i/o\n",
            dev_name);
        exit(EXIT_FAILURE);
    }

    /* Select video input, video standard and tune here. */

    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */

        if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
            switch (errno) {
            case EINVAL:
                /* Cropping not supported. */
                break;
            default:
                /* Errors ignored. */
                break;
            }
        }
    } else {
        /* Errors ignored. */
    }

    CLEAR(fmt);

    /* get the current format */
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
        errno_exit("VIDIOC_G_FMT");
    printf("got format: %s\n",
        snprintf_format(strbuf, sizeof(strbuf), &fmt));

    /* try to set the current format (no-change should always succeed) */
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        const char* s = "VIDIOC_S_FMT";
        fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
        errno_exit("VIDIOC_S_FMT");
    }
    printf("set format: %s\n",
        snprintf_format(strbuf, sizeof(strbuf), &fmt));

    init_mmap();
}

static void close_device(void)
{
    if (-1 == close(fd))
        errno_exit("close");

    fd = -1;
}

static void open_device(void)
{
    struct stat st;

    if (-1 == stat(dev_name, &st)) {
        fprintf(stderr, "Cannot identify '%s': %d, %s\n", dev_name,
            errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "%s is no device\n", dev_name);
        exit(EXIT_FAILURE);
    }

    fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

    if (-1 == fd) {
        fprintf(stderr, "Cannot open '%s': %d, %s\n", dev_name, errno,
            strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static void usage(FILE* fp, int argc, char** argv)
{
    fprintf(fp,
        "Usage: %s [options]\n\n"
        "Version 1.3\n"
        "Options:\n"
        "-d | --device name   Video device name [%s]\n"
        "-h | --help          Print this message\n"
        "-m | --mmap          Use memory mapped buffers [default]\n"
        "-r | --read          Use read() calls\n"
        "-u | --userp         Use application allocated buffers\n"
        "-c | --count         Number of frames to grab [%i] (negative numbers: no limit)\n"
        "",
        argv[0], dev_name, frame_count);
}

static const char short_options[] = "d:hmruofc:";

static const struct option long_options[] = {
    { "device", required_argument, NULL, 'd' },
    { "help", no_argument, NULL, 'h' },
    { "mmap", no_argument, NULL, 'm' },
    { "userp", no_argument, NULL, 'u' },
    { "count", required_argument, NULL, 'c' },
    { 0, 0, 0, 0 }
};

int main(int argc, char** argv)
{
    dev_name = "/dev/video0";

    for (;;) {
        int idx;
        int c;

        c = getopt_long(argc, argv, short_options, long_options, &idx);

        if (-1 == c)
            break;

        switch (c) {
        case 0: /* getopt_long() flag */
            break;

        case 'd':
            dev_name = optarg;
            break;

        case 'h':
            usage(stdout, argc, argv);
            exit(EXIT_SUCCESS);

        case 'c':
            errno = 0;
            frame_count = strtol(optarg, NULL, 0);
            if (errno)
                errno_exit(optarg);
            break;

        default:
            usage(stderr, argc, argv);
            exit(EXIT_FAILURE);
        }
    }

    open_device();
    init_device();
    start_capturing();
    mainloop();
    stop_capturing();
    uninit_device();
    close_device();
    fprintf(stderr, "\n");
    return 0;
}
