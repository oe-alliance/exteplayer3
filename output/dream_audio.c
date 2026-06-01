/*
 * DreamNextGen (AMLogic dreamone/dreamtwo) ALSA audio output.
 *
 * Replaces the linuxdvb_mipsel audio path for boxes that don't have a
 * kernel-side ES audio decoder. We force every codec through software
 * decode (see SetupSoftwareDecoders() called from main), so the bytes
 * arriving in Write() are interleaved S16 PCM and pcmPrivateData_t in
 * extradata carries rate/channels.
 *
 * Kernel /sys/class/tsync is disabled while we own audio so the kernel
 * video decoder runs free — exactly the same approach dream_alsa uses
 * for the GStreamer dreamaudiosink path. Userspace AV-sync polishing
 * lives outside this module.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>

#include <alsa/asoundlib.h>

#include "common.h"
#include "output.h"
#include "debug.h"
#include "misc.h"
#include "pcm.h"

#define cERR_DREAMAUDIO_NO_ERROR   0
#define cERR_DREAMAUDIO_ERROR     -1

#define DA_DBG(fmt, ...) do { fprintf(stderr, "[dream_audio] " fmt "\n", ##__VA_ARGS__); } while (0)

#define ALSA_DEVICE_DEFAULT     "default"
#define ALSA_BUFFER_TIME_US     500000   /* 500 ms — matches lib/dvb/alsa.cpp */
#define ALSA_OPEN_MAX_RETRIES   5
#define ALSA_OPEN_RETRY_MS      50

#define TSYNC_ENABLE            "/sys/class/tsync/enable"
#define TSYNC_PTS_AUDIO         "/sys/class/tsync/pts_audio"
#define TSYNC_DISCONTINUE       "/sys/class/tsync/discontinue"

/* ----- state ---------------------------------------------------------- */

static pthread_mutex_t da_mutex = PTHREAD_MUTEX_INITIALIZER;
static snd_pcm_t      *da_handle = NULL;
static unsigned int    da_rate = 0;
static unsigned int    da_channels = 0;
static int             da_configured = 0;
static int             da_paused = 0;
static int             da_running = 0;
static unsigned long long da_current_pts = 0;

static char           *da_device_name = NULL;   /* strdup'd on open, freed on close */

/* ----- sysfs helpers -------------------------------------------------- */

static void da_write_sysfs(const char *path, const char *val)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(val, f);
    fclose(f);
}

static void da_tsync_set_enabled(int on)
{
    da_write_sysfs(TSYNC_ENABLE, on ? "1" : "0");
}

static void da_tsync_checkin_apts(uint32_t pts_90khz)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%x", pts_90khz);
    da_write_sysfs(TSYNC_PTS_AUDIO, buf);
}

static void da_tsync_signal_discontinuity(void)
{
    da_write_sysfs(TSYNC_DISCONTINUE, "1");
}

/* ----- ALSA wrappers (caller holds da_mutex) -------------------------- */

static int da_alsa_open_handle(void)
{
    const char *dev = da_device_name ? da_device_name : ALSA_DEVICE_DEFAULT;
    int err = 0;
    for (int i = 0; i < ALSA_OPEN_MAX_RETRIES; ++i) {
        err = snd_pcm_open(&da_handle, dev,
                           SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (err == 0) break;
        if (err == -EBUSY && i < ALSA_OPEN_MAX_RETRIES - 1) {
            DA_DBG("open '%s' busy, retry %d/%d", dev, i + 1, ALSA_OPEN_MAX_RETRIES);
            usleep(ALSA_OPEN_RETRY_MS * 1000);
            da_handle = NULL;
            continue;
        }
        DA_DBG("open '%s' failed: %s", dev, snd_strerror(err));
        da_handle = NULL;
        return -1;
    }
    if (!da_handle) return -1;
    snd_pcm_nonblock(da_handle, 0);
    return 0;
}

static void da_alsa_close_handle(void)
{
    if (da_handle) {
        snd_pcm_drop(da_handle);
        snd_pcm_close(da_handle);
        da_handle = NULL;
        usleep(10000);
    }
    da_configured = 0;
    da_rate = 0;
    da_channels = 0;
}

static int da_alsa_setparams(unsigned int rate, unsigned int channels)
{
    if (da_configured && da_rate == rate && da_channels == channels)
        return 0;

    if (da_handle)
        da_alsa_close_handle();
    if (da_alsa_open_handle() < 0)
        return -1;

    int err = snd_pcm_set_params(da_handle,
                                 SND_PCM_FORMAT_S16,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 channels,
                                 rate,
                                 1,
                                 ALSA_BUFFER_TIME_US);
    if (err < 0) {
        DA_DBG("set_params(rate=%u ch=%u): %s", rate, channels, snd_strerror(err));
        return -1;
    }
    da_rate = rate;
    da_channels = channels;
    da_configured = 1;
    DA_DBG("configured rate=%u ch=%u", rate, channels);
    return 0;
}

static int da_alsa_write(const uint8_t *data, size_t size)
{
    if (!da_handle || !da_configured || !data || size == 0) return -1;
    const size_t frame_bytes = (size_t)da_channels * sizeof(int16_t);
    if (frame_bytes == 0 || (size % frame_bytes) != 0) return -1;

    snd_pcm_uframes_t frames = size / frame_bytes;
    size_t offset = 0;
    while (frames > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(da_handle, data + offset, frames);
        if (n < 0) {
            int err = snd_pcm_recover(da_handle, (int)n, 0);
            if (err < 0) {
                DA_DBG("recover: %s", snd_strerror(err));
                return -1;
            }
            continue;
        }
        offset += (size_t)n * frame_bytes;
        frames -= (snd_pcm_uframes_t)n;
    }
    return (int)size;
}

/* ----- Output_t Command handlers -------------------------------------- */

static int DreamAudioOpen(Context_t *context, char *type)
{
    if (strcmp(type, "audio") != 0) return cERR_DREAMAUDIO_NO_ERROR;

    pthread_mutex_lock(&da_mutex);
    if (!da_device_name) {
        const char *dev = getenv("EXTEPLAYER3_ALSA_DEVICE");
        da_device_name = strdup(dev && *dev ? dev : ALSA_DEVICE_DEFAULT);
    }
    /* Leave kernel tsync alone — dream_video relies on it for AV
     * sync (audio-PTS via TSYNC_PTS_AUDIO writes below, video-PTS
     * via the AMSTREAM_IOC_TSTAMP ioctl). For audio-only streams
     * tsync's VMASTER default doesn't pause anything: ALSA owns
     * its own timing here. */
    int ret = da_alsa_open_handle();
    da_paused = 0;
    da_running = 0;
    pthread_mutex_unlock(&da_mutex);

    if (ret < 0) {
        DA_DBG("open failed");
        return cERR_DREAMAUDIO_ERROR;
    }
    DA_DBG("opened device='%s'", da_device_name);
    return cERR_DREAMAUDIO_NO_ERROR;
}

static int DreamAudioClose(Context_t *context, char *type)
{
    if (strcmp(type, "audio") != 0) return cERR_DREAMAUDIO_NO_ERROR;

    pthread_mutex_lock(&da_mutex);
    da_alsa_close_handle();
    da_running = 0;
    da_paused = 0;
    da_current_pts = 0;
    pthread_mutex_unlock(&da_mutex);
    DA_DBG("closed");
    return cERR_DREAMAUDIO_NO_ERROR;
}

static int DreamAudioPlay(Context_t *context, char *type)
{
    if (strcmp(type, "audio") != 0) return cERR_DREAMAUDIO_NO_ERROR;
    pthread_mutex_lock(&da_mutex);
    da_running = 1;
    da_paused = 0;
    da_current_pts = 0;
    pthread_mutex_unlock(&da_mutex);
    DA_DBG("play");
    return cERR_DREAMAUDIO_NO_ERROR;
}

static int DreamAudioStop(Context_t *context, char *type)
{
    if (strcmp(type, "audio") != 0) return cERR_DREAMAUDIO_NO_ERROR;
    pthread_mutex_lock(&da_mutex);
    if (da_handle) {
        snd_pcm_drop(da_handle);
        snd_pcm_prepare(da_handle);
    }
    da_running = 0;
    da_current_pts = 0;
    pthread_mutex_unlock(&da_mutex);
    DA_DBG("stop");
    return cERR_DREAMAUDIO_NO_ERROR;
}

static int DreamAudioFlush(Context_t *context, char *type)
{
    if (strcmp(type, "audio") != 0) return cERR_DREAMAUDIO_NO_ERROR;
    pthread_mutex_lock(&da_mutex);
    if (da_handle) {
        snd_pcm_drop(da_handle);
        snd_pcm_prepare(da_handle);
    }
    da_tsync_signal_discontinuity();
    pthread_mutex_unlock(&da_mutex);
    DA_DBG("flush");
    return cERR_DREAMAUDIO_NO_ERROR;
}

static int DreamAudioPause(Context_t *context, char *type)
{
    if (strcmp(type, "audio") != 0) return cERR_DREAMAUDIO_NO_ERROR;
    pthread_mutex_lock(&da_mutex);
    if (da_handle) snd_pcm_pause(da_handle, 1);
    da_paused = 1;
    pthread_mutex_unlock(&da_mutex);
    return cERR_DREAMAUDIO_NO_ERROR;
}

static int DreamAudioContinue(Context_t *context, char *type)
{
    if (strcmp(type, "audio") != 0) return cERR_DREAMAUDIO_NO_ERROR;
    pthread_mutex_lock(&da_mutex);
    if (da_handle) snd_pcm_pause(da_handle, 0);
    da_paused = 0;
    pthread_mutex_unlock(&da_mutex);
    return cERR_DREAMAUDIO_NO_ERROR;
}

static int DreamAudioClear(Context_t *context, char *type)
{
    return DreamAudioFlush(context, type);
}

static int DreamAudioSwitch(Context_t *context, char *type)
{
    /* track switch — drop pending buffer so PCM from the new codec params
     * doesn't get mixed with the tail of the old track */
    return DreamAudioFlush(context, type);
}

static int DreamAudioPts(Context_t *context, unsigned long long *pts)
{
    pthread_mutex_lock(&da_mutex);
    *pts = da_current_pts;
    pthread_mutex_unlock(&da_mutex);
    return cERR_DREAMAUDIO_NO_ERROR;
}

/* ----- Write (the actual PCM hand-off) -------------------------------- */

static int DreamAudioWrite(void *_context, void *_out)
{
    Context_t *context     = (Context_t *)_context;
    AudioVideoOut_t *out   = (AudioVideoOut_t *)_out;

    if (!out || !out->data || out->len == 0) return cERR_DREAMAUDIO_NO_ERROR;
    if (out->type && strcmp(out->type, "audio") != 0)
        return cERR_DREAMAUDIO_NO_ERROR;

    /* container_ffmpeg.c wraps decoded PCM with pcmPrivateData_t. If extralen
     * doesn't match we got something we can't play (raw bypass-mode frames
     * for a codec where software_decode wasn't forced on). Bail loudly. */
    if (!out->extradata || out->extralen != sizeof(pcmPrivateData_t)) {
        DA_DBG("write: missing/invalid pcmPrivateData (extralen=%u expected=%zu) — "
               "codec wasn't forced through software decode?",
               out->extralen, sizeof(pcmPrivateData_t));
        return cERR_DREAMAUDIO_ERROR;
    }

    pcmPrivateData_t *pcm = (pcmPrivateData_t *)out->extradata;
    unsigned int rate     = (unsigned int)pcm->sample_rate;
    unsigned int channels = (unsigned int)pcm->channels;
    if (rate == 0 || channels == 0 || channels > 8) {
        DA_DBG("write: bad pcm header rate=%u ch=%u", rate, channels);
        return cERR_DREAMAUDIO_ERROR;
    }

    pthread_mutex_lock(&da_mutex);

    if (!da_handle) {
        if (da_alsa_open_handle() < 0) {
            pthread_mutex_unlock(&da_mutex);
            return cERR_DREAMAUDIO_ERROR;
        }
    }
    if (da_alsa_setparams(rate, channels) < 0) {
        pthread_mutex_unlock(&da_mutex);
        return cERR_DREAMAUDIO_ERROR;
    }

    /* PTS book-keeping: container_ffmpeg.c hands us 90 kHz units in
     * out->pts. Update sCURRENT_PTS for OUTPUT_PTS callers; checkin to
     * tsync as a diagnostic (no effect on AV sync per dream_avsync). */
    if (out->pts != INVALID_PTS_VALUE) {
        da_current_pts = (unsigned long long)out->pts;
        da_tsync_checkin_apts((uint32_t)(out->pts & 0xFFFFFFFFu));
    }

    int wrote = da_alsa_write(out->data, out->len);
    pthread_mutex_unlock(&da_mutex);

    if (wrote < 0) {
        DA_DBG("write %u bytes failed", out->len);
        return cERR_DREAMAUDIO_ERROR;
    }
    return cERR_DREAMAUDIO_NO_ERROR;
}

/* ----- Command dispatcher --------------------------------------------- */

static int DreamAudioCommand(void *_context, OutputCmd_t cmd, void *arg)
{
    Context_t *context = (Context_t *)_context;

    switch (cmd) {
    case OUTPUT_OPEN:       return DreamAudioOpen(context, (char *)arg);
    case OUTPUT_CLOSE:      return DreamAudioClose(context, (char *)arg);
    case OUTPUT_PLAY:       return DreamAudioPlay(context, (char *)arg);
    case OUTPUT_STOP:       return DreamAudioStop(context, (char *)arg);
    case OUTPUT_FLUSH:      return DreamAudioFlush(context, (char *)arg);
    case OUTPUT_PAUSE:      return DreamAudioPause(context, (char *)arg);
    case OUTPUT_CONTINUE:   return DreamAudioContinue(context, (char *)arg);
    case OUTPUT_CLEAR:      return DreamAudioClear(context, (char *)arg);
    case OUTPUT_SWITCH:     return DreamAudioSwitch(context, (char *)arg);
    case OUTPUT_PTS:        return DreamAudioPts(context, (unsigned long long *)arg);
    case OUTPUT_AVSYNC:
    case OUTPUT_AUDIOMUTE:
    case OUTPUT_FASTFORWARD:
    case OUTPUT_REVERSE:
    case OUTPUT_DISCONTINUITY_REVERSE:
    case OUTPUT_SLOWMOTION:
    case OUTPUT_GET_FRAME_COUNT:
    case OUTPUT_GET_PROGRESSIVE:
    case OUTPUT_SET_BUFFER_SIZE:
    case OUTPUT_GET_BUFFER_SIZE:
        /* video-side or stub-for-audio — silent no-op */
        return cERR_DREAMAUDIO_NO_ERROR;
    default:
        DA_DBG("unhandled cmd %d", cmd);
        return cERR_DREAMAUDIO_NO_ERROR;
    }
}

/* ----- Output_t binding ------------------------------------------------ */

static char *DreamAudioCapabilities[] = { "audio", NULL };

struct Output_s DreamAudioOutput = {
    "DreamAudio",
    &DreamAudioCommand,
    &DreamAudioWrite,
    DreamAudioCapabilities
};
