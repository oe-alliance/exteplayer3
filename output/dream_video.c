/*
 * DreamNextGen (AMLogic dreamone/dreamtwo) video output via the AML
 * /dev/amstream_vbuf port. Replaces the linuxdvb_mipsel video path
 * for boxes whose /dev/dvb/adapter0/video0 shim doesn't expose
 * AMSTREAM_IOC_PORT_INIT and silently drops every write (amstream
 * Video buffer stays Unalloc / wcnt=0, decoder hangs in CONNECTED).
 *
 * Init sequence mirrors libamcodec esplayer.c + Kodi AMLCodec.cpp:
 *   VFORMAT → VID → SYSINFO → PORT_INIT, then write Annex-B ES
 *   together with TSTAMP for the PTS.
 *
 * See codesnake/libamcodec/examples/esplayer.c for the canonical
 * userspace flow and quarnster/boxeebox-xbmc AMLCodec.cpp for the
 * dec_sysinfo field semantics (format=VIDEO_DEC_FORMAT_*, not
 * VFORMAT_*, and param = EXTERNAL_PTS | SYNC_OUTSIDE).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/uio.h>

#include "common.h"
#include "output.h"
#include "debug.h"
#include "misc.h"
#include "writer.h"
#include "bcm_ioctls.h"

#define cERR_DREAMVIDEO_NO_ERROR    0
#define cERR_DREAMVIDEO_ERROR      -1

#define DV_DBG(fmt, ...) do { fprintf(stderr, "[dream_video] " fmt "\n", ##__VA_ARGS__); } while (0)

/* AML amstream ioctls (linux-amlogic/include/linux/amlogic/amports/amstream.h
 * — note: 'int' in the macro encodes the ioctl number; the kernel copies
 * the struct from userspace itself, so the third arg of _IOW must literally
 * be 'int' here. Using e.g. 'struct aml_dec_sysinfo' would change the size
 * bits and yield a different ioctl number → ENOTTY.) */
#define AMSTREAM_IOC_MAGIC      'S'
#define AMSTREAM_IOC_VB_SIZE    _IOW(AMSTREAM_IOC_MAGIC, 0x01, int)
#define AMSTREAM_IOC_VFORMAT    _IOW(AMSTREAM_IOC_MAGIC, 0x04, int)
#define AMSTREAM_IOC_VID        _IOW(AMSTREAM_IOC_MAGIC, 0x06, int)
#define AMSTREAM_IOC_SYSINFO    _IOW(AMSTREAM_IOC_MAGIC, 0x0a, int)
#define AMSTREAM_IOC_TSTAMP     _IOW(AMSTREAM_IOC_MAGIC, 0x0e, int)
#define AMSTREAM_IOC_PORT_INIT  _IO (AMSTREAM_IOC_MAGIC, 0x11)
#define AMSTREAM_IOC_VPAUSE     _IOW(AMSTREAM_IOC_MAGIC, 0x17, int)

/* vformat_t — passed to AMSTREAM_IOC_VFORMAT */
#define AML_VFORMAT_MPEG12      0
#define AML_VFORMAT_MPEG4       1
#define AML_VFORMAT_H264        2
#define AML_VFORMAT_MJPEG       3
#define AML_VFORMAT_VC1         6
#define AML_VFORMAT_AVS         7
#define AML_VFORMAT_HEVC        11

/* vdec_type_t — packed into dec_sysinfo.format */
#define AML_DEC_FORMAT_MPEG4_5  3
#define AML_DEC_FORMAT_H264     4
#define AML_DEC_FORMAT_HEVC     15

/* dec_sysinfo.param flag bits */
#define AML_EXTERNAL_PTS        1
#define AML_SYNC_OUTSIDE        2

struct aml_dec_sysinfo {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t rate;
    uint32_t extra;
    uint32_t status;
    uint32_t ratio;
    void    *param;
    uint64_t ratio64;
};

static const char VBUF_DEV[]    = "/dev/amstream_vbuf";
static const char CNTL_DEV[]    = "/dev/amvideo";

/* state */
static pthread_mutex_t        dv_mutex = PTHREAD_MUTEX_INITIALIZER;
static int                    dv_fd = -1;          /* /dev/amstream_vbuf — ES writes */
static int                    dv_cntl_fd = -1;     /* /dev/amvideo       — TSTAMP ioctl */
static int                    dv_running = 0;
static int                    dv_inited = 0;
static unsigned long long     dv_current_pts = 0;

/* saved tsync state — restored on close so we leave the system the way
 * the next consumer (Live-TV / GStreamer) found it. */
static int                    dv_tsync_saved = 0;
static int                    dv_tsync_saved_enable = 0;
static int                    dv_tsync_saved_mode = 0;

/* BCM streamtype (from exteplayer3's bcm_ioctls.h) → AML pair. */
static int aml_format_for(int bcm_streamtype, uint32_t *vformat, uint32_t *dec_format)
{
    switch (bcm_streamtype) {
    case STREAMTYPE_MPEG2:        *vformat = AML_VFORMAT_MPEG12; *dec_format = 0; return 0;
    case STREAMTYPE_MPEG1:        *vformat = AML_VFORMAT_MPEG12; *dec_format = 0; return 0;
    case STREAMTYPE_MPEG4_Part2:  *vformat = AML_VFORMAT_MPEG4;  *dec_format = AML_DEC_FORMAT_MPEG4_5; return 0;
    case STREAMTYPE_MPEG4_H264:   *vformat = AML_VFORMAT_H264;   *dec_format = AML_DEC_FORMAT_H264; return 0;
    case STREAMTYPE_MPEG4_H265:   *vformat = AML_VFORMAT_HEVC;   *dec_format = AML_DEC_FORMAT_HEVC; return 0;
    case STREAMTYPE_VC1:          *vformat = AML_VFORMAT_VC1;    *dec_format = 0; return 0;
    case STREAMTYPE_MJPEG:        *vformat = AML_VFORMAT_MJPEG;  *dec_format = 0; return 0;
    default:                      return -1;
    }
}

/* AMlogic display sink blanks frames while disable_video != 0. The
 * value persists across processes (enigma2 / live-TV / Standby leave it
 * at 1 or 2). Re-enable on open. */
static void dv_enable_display(void)
{
    FILE *f = fopen("/sys/class/video/disable_video", "w");
    if (!f) return;
    fputs("0", f);
    fclose(f);
}

/* Kernel tsync drives AV sync between our amstream_vbuf video PTS
 * (via AMSTREAM_IOC_TSTAMP) and dream_audio's pts_audio sysfs write.
 * Force on AND amaster mode — gst-plugin-dreamaudiosink leaves it
 * disabled, dreamvideosink/Live-TV leave it in pcrmaster (=2) which
 * waits for a transport-stream PCR HLS/file playback never delivers
 * (= video stutters). Save & restore the previous state so the next
 * consumer (Live-TV / GStreamer) finds the system it expects. */
static int dv_read_sysfs_int(const char *path, int *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = 0;
    int n = fscanf(f, "%d", &v);
    fclose(f);
    if (n != 1) return -1;
    *out = v;
    return 0;
}

static void dv_write_sysfs_int(const char *path, int v)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d", v);
    fclose(f);
}

static void dv_setup_tsync(void)
{
    if (!dv_tsync_saved) {
        if (dv_read_sysfs_int("/sys/class/tsync/enable", &dv_tsync_saved_enable) == 0 &&
            dv_read_sysfs_int("/sys/class/tsync/mode",   &dv_tsync_saved_mode)   == 0)
            dv_tsync_saved = 1;
    }
    dv_write_sysfs_int("/sys/class/tsync/mode",   1);   /* 1 = amaster */
    dv_write_sysfs_int("/sys/class/tsync/enable", 1);
}

static void dv_restore_tsync(void)
{
    if (!dv_tsync_saved) return;
    dv_write_sysfs_int("/sys/class/tsync/mode",   dv_tsync_saved_mode);
    dv_write_sysfs_int("/sys/class/tsync/enable", dv_tsync_saved_enable);
    dv_tsync_saved = 0;
}

/* ----- Output_t handlers ---------------------------------------------- */

static int DreamVideoOpen(Context_t *context, char *type)
{
    if (strcmp(type, "video") != 0) return cERR_DREAMVIDEO_NO_ERROR;

    pthread_mutex_lock(&dv_mutex);
    if (dv_fd < 0) {
        dv_fd = open(VBUF_DEV, O_WRONLY);
        if (dv_fd < 0) {
            DV_DBG("open %s failed: %s", VBUF_DEV, strerror(errno));
            pthread_mutex_unlock(&dv_mutex);
            return cERR_DREAMVIDEO_ERROR;
        }
        DV_DBG("opened %s (fd=%d)", VBUF_DEV, dv_fd);
        /* PTS check-in (AMSTREAM_IOC_TSTAMP) doesn't go through
         * amstream_vbuf in libamcodec — it goes through the control
         * device /dev/amvideo. Without this fd TSTAMP returns EINVAL
         * and the decoder never learns the first PTS, so first_stamp
         * stays 0xffffffff and the pipeline freezes. */
        dv_cntl_fd = open(CNTL_DEV, O_RDWR);
        if (dv_cntl_fd < 0)
            DV_DBG("open %s failed: %s", CNTL_DEV, strerror(errno));
        else
            DV_DBG("opened %s (fd=%d)", CNTL_DEV, dv_cntl_fd);
        dv_enable_display();
        dv_setup_tsync();
    }
    dv_inited = 0;
    dv_running = 0;
    pthread_mutex_unlock(&dv_mutex);
    return cERR_DREAMVIDEO_NO_ERROR;
}

static int DreamVideoClose(Context_t *context, char *type)
{
    if (strcmp(type, "video") != 0) return cERR_DREAMVIDEO_NO_ERROR;

    pthread_mutex_lock(&dv_mutex);
    if (dv_fd >= 0) {
        close(dv_fd);
        dv_fd = -1;
        if (dv_cntl_fd >= 0) {
            close(dv_cntl_fd);
            dv_cntl_fd = -1;
        }
        dv_restore_tsync();
    }
    dv_inited = 0;
    dv_running = 0;
    dv_current_pts = 0;
    pthread_mutex_unlock(&dv_mutex);
    DV_DBG("closed");
    return cERR_DREAMVIDEO_NO_ERROR;
}

static int DreamVideoPlay(Context_t *context, char *type)
{
    if (strcmp(type, "video") != 0) return cERR_DREAMVIDEO_NO_ERROR;

    pthread_mutex_lock(&dv_mutex);
    if (dv_fd >= 0) {
        char *Encoding = NULL;
        context->manager->video->Command(context, MANAGER_GETENCODING, &Encoding);
        Writer_t *writer = getWriter(Encoding);
        if (writer) {
            uint32_t vformat = 0, dec_format = 0;
            if (aml_format_for(writer->caps->dvbStreamType, &vformat, &dec_format) == 0) {
                if (ioctl(dv_fd, AMSTREAM_IOC_VFORMAT, vformat) < 0)
                    DV_DBG("VFORMAT %u failed: %s", vformat, strerror(errno));
                else
                    DV_DBG("VFORMAT set to %u (%s)", vformat, writer->caps->name);
                /* ES mode: VID=0xffff means no stream-id filtering. */
                ioctl(dv_fd, AMSTREAM_IOC_VID, 0xffff);
            } else {
                DV_DBG("no AML mapping for streamtype %d (encoding %s)",
                       writer->caps->dvbStreamType, Encoding);
            }
        }
        free(Encoding);
    }
    dv_running = 1;
    pthread_mutex_unlock(&dv_mutex);
    return cERR_DREAMVIDEO_NO_ERROR;
}

static int DreamVideoStop(Context_t *context, char *type)
{
    return DreamVideoClose(context, type);
}

static int DreamVideoPts(Context_t *context, unsigned long long *pts)
{
    pthread_mutex_lock(&dv_mutex);
    *pts = dv_current_pts;
    pthread_mutex_unlock(&dv_mutex);
    return cERR_DREAMVIDEO_NO_ERROR;
}

/* ----- Write ---------------------------------------------------------- */

static int dv_lazy_init(Context_t *context, AudioVideoOut_t *out)
{
    if (dv_inited || !out->width || !out->height) return 0;

    char *Encoding = NULL;
    context->manager->video->Command(context, MANAGER_GETENCODING, &Encoding);
    Writer_t *writer = getWriter(Encoding);
    if (!writer) {
        free(Encoding);
        return -1;
    }

    uint32_t vformat = 0, dec_format = 0;
    if (aml_format_for(writer->caps->dvbStreamType, &vformat, &dec_format) < 0) {
        free(Encoding);
        return -1;
    }
    free(Encoding);

    struct aml_dec_sysinfo si;
    memset(&si, 0, sizeof(si));
    si.format = dec_format;
    si.width  = out->width;
    si.height = out->height;
    /* rate is duration-per-frame in 96000-unit ticks, NOT fps.
     * exteplayer3 reports frameRate as e.g. 50000 milli-fps. */
    if (out->frameRate > 0)
        si.rate = (uint32_t)((uint64_t)96000ULL * 1000U / out->frameRate);
    else
        si.rate = 3200;     /* 30 fps fallback */
    si.param  = (void *)(uintptr_t)(AML_EXTERNAL_PTS | AML_SYNC_OUTSIDE);

    if (ioctl(dv_fd, AMSTREAM_IOC_SYSINFO, &si) < 0) {
        DV_DBG("SYSINFO failed: %s", strerror(errno));
        return -1;
    }
    if (ioctl(dv_fd, AMSTREAM_IOC_PORT_INIT) < 0) {
        DV_DBG("PORT_INIT failed: %s", strerror(errno));
        return -1;
    }
    DV_DBG("inited: dec_format=%u %ux%u rate=%u param=0x%x",
           si.format, si.width, si.height, si.rate,
           AML_EXTERNAL_PTS | AML_SYNC_OUTSIDE);
    dv_inited = 1;
    return 0;
}

static int DreamVideoWrite(void *_context, void *_out)
{
    Context_t       *context = (Context_t *)_context;
    AudioVideoOut_t *out     = (AudioVideoOut_t *)_out;

    if (!out || !out->data || out->len == 0) return cERR_DREAMVIDEO_NO_ERROR;
    if (out->type && strcmp(out->type, "video") != 0)
        return cERR_DREAMVIDEO_NO_ERROR;

    pthread_mutex_lock(&dv_mutex);
    if (dv_fd < 0) { pthread_mutex_unlock(&dv_mutex); return cERR_DREAMVIDEO_ERROR; }

    if (dv_lazy_init(context, out) < 0) {
        pthread_mutex_unlock(&dv_mutex);
        return cERR_DREAMVIDEO_ERROR;
    }

    /* PTS handed to the decoder via TSTAMP (90 kHz, low 32 bits) on the
     * /dev/amvideo control fd — NOT amstream_vbuf. _IOW(... int) means
     * the kernel does copy_from_user, so we pass a POINTER, not the
     * value (libamcodec's codec_h_control plays the same trick by
     * passing 'unsigned long paramter' which on aarch64 is the same
     * width as a pointer). */
    if (out->pts != (int64_t)INVALID_PTS_VALUE && out->pts >= 0 && dv_cntl_fd >= 0) {
        unsigned int pts32 = (unsigned int)(out->pts & 0xFFFFFFFF);
        if (ioctl(dv_cntl_fd, AMSTREAM_IOC_TSTAMP, &pts32) < 0)
            DV_DBG("TSTAMP failed: %s", strerror(errno));
        dv_current_pts = (unsigned long long)out->pts;
    }

    /* Build the iovec via the existing per-codec writer. With the
     * raw-ES patch in h264.c (and friends), iov[0].iov_len = 0 so
     * the PES header is skipped — what comes out is Annex-B NALU. */
    char *Encoding = NULL;
    context->manager->video->Command(context, MANAGER_GETENCODING, &Encoding);
    Writer_t *writer = getWriter(Encoding);
    free(Encoding);

    if (!writer || !writer->writeData) {
        pthread_mutex_unlock(&dv_mutex);
        return cERR_DREAMVIDEO_ERROR;
    }

    WriterAVCallData_t call;
    memset(&call, 0, sizeof(call));
    call.fd           = dv_fd;
    call.data         = out->data;
    call.len          = out->len;
    call.Pts          = out->pts;
    call.Dts          = out->dts;
    call.private_data = out->extradata;
    call.private_size = out->extralen;
    call.FrameRate    = out->frameRate;
    call.FrameScale   = out->timeScale;
    call.Width        = out->width;
    call.Height       = out->height;
    call.InfoFlags    = out->infoFlags;
    call.Version      = 0;
    call.WriteV       = writev_with_retry;

    int res = writer->writeData(&call);
    pthread_mutex_unlock(&dv_mutex);

    if (res < 0) {
        DV_DBG("writeData %u bytes failed: %s", out->len, strerror(errno));
        return cERR_DREAMVIDEO_ERROR;
    }
    return cERR_DREAMVIDEO_NO_ERROR;
}

/* ----- Command dispatch ----------------------------------------------- */

static int DreamVideoCommand(void *_context, OutputCmd_t cmd, void *arg)
{
    Context_t *context = (Context_t *)_context;

    switch (cmd) {
    case OUTPUT_OPEN:    return DreamVideoOpen(context, (char *)arg);
    case OUTPUT_CLOSE:   return DreamVideoClose(context, (char *)arg);
    case OUTPUT_PLAY:    return DreamVideoPlay(context, (char *)arg);
    case OUTPUT_STOP:    return DreamVideoStop(context, (char *)arg);
    case OUTPUT_PTS:     return DreamVideoPts(context, (unsigned long long *)arg);
    case OUTPUT_FLUSH:
    case OUTPUT_PAUSE:
    case OUTPUT_CONTINUE:
    case OUTPUT_CLEAR:
    case OUTPUT_SWITCH:
    case OUTPUT_AVSYNC:
    case OUTPUT_SLOWMOTION:
    case OUTPUT_AUDIOMUTE:
    case OUTPUT_FASTFORWARD:
    case OUTPUT_REVERSE:
    case OUTPUT_DISCONTINUITY_REVERSE:
    case OUTPUT_GET_FRAME_COUNT:
    case OUTPUT_GET_PROGRESSIVE:
    case OUTPUT_SET_BUFFER_SIZE:
    case OUTPUT_GET_BUFFER_SIZE:
        return cERR_DREAMVIDEO_NO_ERROR;
    default:
        return cERR_DREAMVIDEO_NO_ERROR;
    }
}

static char *DreamVideoCapabilities[] = { "video", NULL };

struct Output_s DreamVideoOutput = {
    "DreamVideo",
    &DreamVideoCommand,
    &DreamVideoWrite,
    DreamVideoCapabilities
};
