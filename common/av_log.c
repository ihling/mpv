/*
 * av_log to mp_msg converter
 * Copyright (C) 2006 Michael Niedermayer
 * Copyright (C) 2009 Uoti Urpala
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>

#include "av_log.h"
#include "config.h"
#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"

#include <libavutil/avutil.h>
#include <libavutil/log.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#if HAVE_LIBAVDEVICE
#include <libavdevice/avdevice.h>
#endif

#if HAVE_LIBAVFILTER
#include <libavfilter/avfilter.h>
#endif

#if HAVE_LIBAVRESAMPLE
#include <libavresample/avresample.h>
#endif
#if HAVE_LIBSWRESAMPLE
#include <libswresample/swresample.h>
#endif

#if LIBAVCODEC_VERSION_MICRO >= 100
#define LIB_PREFIX "ffmpeg"
#else
#define LIB_PREFIX "libav"
#endif

// Needed because the av_log callback does not provide a library-safe message
// callback.
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mpv_global *log_mpv_instance;
static struct mp_log *log_root, *log_decaudio, *log_decvideo, *log_demuxer;
static bool log_print_prefix = true;

static int av_log_level_to_mp_level(int av_level)
{
    if (av_level > AV_LOG_VERBOSE)
        return MSGL_DBG2;
    if (av_level > AV_LOG_INFO)
        return MSGL_V;
    if (av_level > AV_LOG_WARNING)
        return MSGL_V;
    if (av_level > AV_LOG_ERROR)
        return MSGL_WARN;
    if (av_level > AV_LOG_FATAL)
        return MSGL_ERR;
    return MSGL_FATAL;
}

static struct mp_log *get_av_log(void *ptr)
{
    if (!ptr)
        return log_root;

    AVClass *avc = *(AVClass **)ptr;
    if (!avc) {
        mp_warn(log_root,
               "av_log callback called with bad parameters (NULL AVClass).\n"
               "This is a bug in one of Libav/FFmpeg libraries used.\n");
        return log_root;
    }

    if (!strcmp(avc->class_name, "AVCodecContext")) {
        AVCodecContext *s = ptr;
        if (s->codec) {
            if (s->codec->type == AVMEDIA_TYPE_AUDIO) {
                if (s->codec->decode)
                    return log_decaudio;
            } else if (s->codec->type == AVMEDIA_TYPE_VIDEO) {
                if (s->codec->decode)
                    return log_decvideo;
            }
        }
    }

    if (!strcmp(avc->class_name, "AVFormatContext")) {
        AVFormatContext *s = ptr;
        if (s->iformat)
            return log_demuxer;
    }

    return log_root;
}

static void mp_msg_av_log_callback(void *ptr, int level, const char *fmt,
                                   va_list vl)
{
    AVClass *avc = ptr ? *(AVClass **)ptr : NULL;
    int mp_level = av_log_level_to_mp_level(level);

    // Note: mp_log is thread-safe, but destruction of the log instances is not.
    pthread_mutex_lock(&log_lock);

    if (!log_mpv_instance) {
        pthread_mutex_unlock(&log_lock);
        // Fallback to stderr
        vfprintf(stderr, fmt, vl);
        return;
    }

    struct mp_log *log = get_av_log(ptr);

    if (mp_msg_test_log(log, mp_level)) {
        if (log_print_prefix)
            mp_msg_log(log, mp_level, "%s: ", avc ? avc->item_name(ptr) : "?");
        log_print_prefix = fmt[strlen(fmt) - 1] == '\n';

        mp_msg_log_va(log, mp_level, fmt, vl);
    }

    pthread_mutex_unlock(&log_lock);
}

void init_libav(struct mpv_global *global)
{
    pthread_mutex_lock(&log_lock);
    if (!log_mpv_instance) {
        log_mpv_instance = global;
        log_root = mp_log_new(NULL, global->log, LIB_PREFIX);
        log_decaudio = mp_log_new(log_root, log_root, "audio");
        log_decvideo = mp_log_new(log_root, log_root, "video");
        log_demuxer = mp_log_new(log_root, log_root, "demuxer");
        av_log_set_callback(mp_msg_av_log_callback);
    }
    pthread_mutex_unlock(&log_lock);

    avcodec_register_all();
    av_register_all();
    avformat_network_init();

#if HAVE_LIBAVFILTER
    avfilter_register_all();
#endif
#if HAVE_LIBAVDEVICE
    avdevice_register_all();
#endif
}

void uninit_libav(struct mpv_global *global)
{
    pthread_mutex_lock(&log_lock);
    if (log_mpv_instance == global) {
        log_mpv_instance = NULL;
        talloc_free(log_root);
    }
    pthread_mutex_unlock(&log_lock);
}

#define V(x) (x)>>16, (x)>>8 & 255, (x) & 255
static void print_version(struct mp_log *log, int v, char *name,
                          unsigned buildv, unsigned runv)
{
    mp_msg_log(log, v, "   %-15s %d.%d.%d", name, V(buildv));
    if (buildv != runv)
        mp_msg_log(log, v, " (runtime %d.%d.%d)", V(runv));
    mp_msg_log(log, v, "\n");
}
#undef V

void print_libav_versions(struct mp_log *log, int v)
{
    mp_msg_log(log, v, "%s library versions:\n", LIB_PREFIX);

    print_version(log, v, "libavutil",     LIBAVUTIL_VERSION_INT,     avutil_version());
    print_version(log, v, "libavcodec",    LIBAVCODEC_VERSION_INT,    avcodec_version());
    print_version(log, v, "libavformat",   LIBAVFORMAT_VERSION_INT,   avformat_version());
    print_version(log, v, "libswscale",    LIBSWSCALE_VERSION_INT,    swscale_version());
#if HAVE_LIBAVFILTER
    print_version(log, v, "libavfilter",   LIBAVFILTER_VERSION_INT,   avfilter_version());
#endif
#if HAVE_LIBAVRESAMPLE
    print_version(log, v, "libavresample", LIBAVRESAMPLE_VERSION_INT, avresample_version());
#endif
#if HAVE_LIBSWRESAMPLE
    print_version(log, v, "libswresample", LIBSWRESAMPLE_VERSION_INT, swresample_version());
#endif
}