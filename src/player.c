/*
 * MusicPlayer — moteur de lecture
 * ===============================
 * Décodage : FFmpeg (libavformat/libavcodec) → resampling via
 * libswresample vers f32 stéréo → ring buffer SPSC → sortie miniaudio.
 *
 * - La vitesse est obtenue en changeant le taux de sortie du resampler
 *   (taux_périph × vitesse) : 2x = le fichier est consommé 2 fois plus vite.
 * - Le volume est appliqué au périphérique (ma_device_set_master_volume).
 * - Les plugins d'effets audio sont appliqués dans le callback du device.
 * - Le thread de décodage est persistant ; l'arrêt (stop) est fiable grâce
 *   à un callback d'interruption sur le contexte FFmpeg.
 */
#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "player.h"
#include "plugin_loader.h"

/* ------------------------------------------------------------------ */
/* Ring buffer SPSC (Single Producer / Single Consumer)                */
/* 2^18 frames stéréo f32 ≈ 6 s à 44,1 kHz — 2,1 Mo                    */
/* ------------------------------------------------------------------ */
#define RING_FRAMES (1u << 18)
#define RING_MASK   (RING_FRAMES - 1)

typedef struct {
    float* buf;
    volatile LONG head;             /* écrit par le décodeur */
    volatile LONG tail;             /* lu par le callback audio */
} ring_t;

static void ring_init(ring_t* r)
{
    r->buf = (float*)malloc(RING_FRAMES * 2 * sizeof(float));
    r->head = 0;
    r->tail = 0;
}

static uint32_t ring_filled(const ring_t* r)
{
    return (uint32_t)r->head - (uint32_t)r->tail;
}

static uint32_t ring_space(const ring_t* r)
{
    return RING_FRAMES - ring_filled(r);
}

static void ring_clear(ring_t* r)
{
    r->head = 0;
    r->tail = 0;
}

static uint32_t ring_write(ring_t* r, const float* data, uint32_t frames)
{
    uint32_t space = ring_space(r);
    if (frames > space) frames = space;
    if (frames == 0) return 0;

    uint32_t h = (uint32_t)r->head & RING_MASK;
    uint32_t n1 = frames;
    if (n1 > RING_FRAMES - h) n1 = RING_FRAMES - h;
    memcpy(r->buf + h * 2, data, (size_t)n1 * 2 * sizeof(float));
    memcpy(r->buf, data + (size_t)n1 * 2, (size_t)(frames - n1) * 2 * sizeof(float));

    r->head = (LONG)((uint32_t)r->head + frames);
    return frames;
}

static uint32_t ring_read(ring_t* r, float* dst, uint32_t frames)
{
    uint32_t filled = ring_filled(r);
    if (frames > filled) frames = filled;
    if (frames == 0) return 0;

    uint32_t t = (uint32_t)r->tail & RING_MASK;
    uint32_t n1 = frames;
    if (n1 > RING_FRAMES - t) n1 = RING_FRAMES - t;
    memcpy(dst, r->buf + t * 2, (size_t)n1 * 2 * sizeof(float));
    memcpy(dst + (size_t)n1 * 2, r->buf, (size_t)(frames - n1) * 2 * sizeof(float));

    r->tail = (LONG)((uint32_t)r->tail + frames);
    return frames;
}

/* ------------------------------------------------------------------ */
/* État global                                                        */
/* ------------------------------------------------------------------ */
static ma_device      g_device;
static int            g_device_ok = 0;
static uint32_t       g_device_rate = 44100;

static ring_t         g_ring;
static AVFormatContext* g_fmt = NULL;
static AVCodecContext*  g_codec = NULL;
static int            g_stream_idx = -1;
static SwrContext*    g_swr = NULL;
static CRITICAL_SECTION g_swr_lock;

static char*          g_path = NULL;
static double         g_duration = 0.0;
static double         g_src_rate = 44100.0;   /* taux d'échantillonnage du fichier */

static volatile LONG  g_state = MP_STATE_STOPPED;
static volatile LONG  g_interrupt = 0;        /* demande d'arrêt du décodage */
static volatile LONG  g_eof = 0;              /* fin de fichier atteinte */
static volatile LONG  g_shutdown = 0;         /* arrêt définitif du thread */
static volatile LONG  g_wake = 0;             /* signal "décode !" */
static volatile LONG  g_decoding = 0;         /* le thread est dans la boucle */
static volatile LONG  g_volume_pct = 80;      /* 0..100 */
static volatile float g_speed = 1.0f;
static volatile LONG64 g_samples_decoded = 0; /* frames poussées dans le ring */

static HANDLE g_thread = NULL;

/* ------------------------------------------------------------------ */
/* Interruption FFmpeg : permet d'arrêter av_read_frame rapidement     */
/* ------------------------------------------------------------------ */
static int interrupt_cb(void* opaque)
{
    (void)opaque;
    return g_interrupt ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Resampler : sortie f32 stéréo à (taux_périph × vitesse)             */
/* ------------------------------------------------------------------ */
static int setup_swr(void)
{
    EnterCriticalSection(&g_swr_lock);
    if (g_swr) { swr_free(&g_swr); g_swr = NULL; }
    int ret = -1;
    if (g_codec && g_codec->ch_layout.nb_channels > 0) {
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        int out_rate = (int)(g_device_rate * g_speed);
        if (out_rate < 8000)  out_rate = 8000;
        if (out_rate > 192000) out_rate = 192000;
        if (swr_alloc_set_opts2(&g_swr, &out_layout, AV_SAMPLE_FMT_FLT, out_rate,
                                &g_codec->ch_layout, g_codec->sample_fmt,
                                g_codec->sample_rate, 0, NULL) == 0 && g_swr) {
            ret = swr_init(g_swr);
        }
    }
    LeaveCriticalSection(&g_swr_lock);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Thread de décodage                                                  */
/* ------------------------------------------------------------------ */
static DWORD WINAPI decode_thread(LPVOID unused)
{
    (void)unused;

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    float*    out_buf = NULL;
    int       out_cap = 0;

    while (!g_shutdown) {
        /* Attend le signal de lecture */
        while (!g_wake && !g_shutdown) Sleep(10);
        if (g_shutdown) break;
        g_wake = 0;
        if (g_state != MP_STATE_PLAYING || !g_fmt) continue;

        g_decoding = 1;
        while (g_state == MP_STATE_PLAYING && !g_interrupt && !g_shutdown) {
            int r = av_read_frame(g_fmt, pkt);
            if (r == AVERROR_EOF) { g_eof = 1; break; }
            if (r < 0) { if (!g_interrupt) g_eof = 1; break; }

            if (pkt->stream_index == g_stream_idx) {
                int sr = avcodec_send_packet(g_codec, pkt);
                av_packet_unref(pkt);
                if (sr < 0 && sr != AVERROR(EAGAIN)) break;

                for (;;) {
                    int rr = avcodec_receive_frame(g_codec, frame);
                    if (rr == AVERROR(EAGAIN)) break;
                    if (rr == AVERROR_EOF) { g_eof = 1; break; }
                    if (rr < 0) break;

                    EnterCriticalSection(&g_swr_lock);
                    int out_samples = g_swr ? swr_get_out_samples(g_swr, frame->nb_samples) : 0;
                    if (out_samples > out_cap) {
                        free(out_buf);
                        out_buf = (float*)malloc((size_t)out_samples * 2 * sizeof(float));
                        out_cap = out_samples;
                    }
                    int got = 0;
                    if (g_swr) {
                        uint8_t* out_ptrs[1] = { (uint8_t*)out_buf };
                        got = swr_convert(g_swr, out_ptrs, out_samples,
                                          (const uint8_t**)frame->extended_data,
                                          frame->nb_samples);
                    }
                    LeaveCriticalSection(&g_swr_lock);
                    av_frame_unref(frame);

                    if (got > 0) {
                        /* push avec backpressure (le callback consomme) */
                        uint32_t pushed = 0;
                        while (pushed < (uint32_t)got &&
                               g_state == MP_STATE_PLAYING &&
                               !g_interrupt && !g_shutdown) {
                            uint32_t w = ring_write(&g_ring, out_buf + (size_t)pushed * 2,
                                                    (uint32_t)got - pushed);
                            pushed += w;
                            g_samples_decoded += w;
                            if (pushed < (uint32_t)got) Sleep(5);
                        }
                        if (g_state != MP_STATE_PLAYING || g_interrupt || g_shutdown) break;
                    }
                }
            } else {
                av_packet_unref(pkt);
            }
        }
        g_decoding = 0;
    }

    free(out_buf);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Callback miniaudio                                                  */
/* ------------------------------------------------------------------ */
static void data_cb(ma_device* dev, void* out, const void* in, ma_uint32 frames)
{
    (void)dev; (void)in;
    float* dst = (float*)out;

    if (g_state == MP_STATE_PLAYING) {
        uint32_t got = ring_read(&g_ring, dst, frames);
        if (got < frames) {
            /* sous-remplissage : silence + détection de fin */
            memset(dst + (size_t)got * 2, 0, (size_t)(frames - got) * 2 * sizeof(float));
            if (g_eof) InterlockedCompareExchange((LONG*)&g_state, MP_STATE_FINISHED, MP_STATE_PLAYING);
        }
        /* effets audio des plugins (si le périph a le bon format) */
        mp_plugins_audio_process(dst, frames, 2, g_device_rate);
        /* flux lecture seule pour les plugins visuels */
        mp_plugins_audio_frames(dst, frames, 2, g_device_rate);
    } else {
        memset(dst, 0, (size_t)frames * 2 * sizeof(float));
    }
}

/* ------------------------------------------------------------------ */
/* API publique                                                        */
/* ------------------------------------------------------------------ */
int mp_init(void)
{
    InitializeCriticalSection(&g_swr_lock);
    ring_init(&g_ring);

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 44100;
    cfg.dataCallback = data_cb;

    if (ma_device_init(NULL, &cfg, &g_device) == MA_SUCCESS) {
        if (ma_device_start(&g_device) == MA_SUCCESS) {
            g_device_ok = 1;
            if (g_device.sampleRate > 0) g_device_rate = g_device.sampleRate;
            ma_device_set_master_volume(&g_device, (float)g_volume_pct / 100.0f);
        } else {
            ma_device_uninit(&g_device);
        }
    }
    /* si aucun périphérique : mode silencieux, le décodage fonctionne quand même */

    g_thread = CreateThread(NULL, 0, decode_thread, NULL, 0, NULL);
    return 0;
}

void mp_shutdown(void)
{
    g_shutdown = 1;
    g_interrupt = 1;
    g_wake = 1;
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_device_ok) {
        ma_device_uninit(&g_device);
        g_device_ok = 0;
    }
    if (g_swr) { swr_free(&g_swr); g_swr = NULL; }
    if (g_codec) { avcodec_free_context(&g_codec); g_codec = NULL; }
    if (g_fmt) { avformat_close_input(&g_fmt); g_fmt = NULL; }
    free(g_path); g_path = NULL;
    g_stream_idx = -1;
    g_state = MP_STATE_STOPPED;
    DeleteCriticalSection(&g_swr_lock);
}

int mp_open(const char* path)
{
    mp_close();

    g_fmt = avformat_alloc_context();
    if (!g_fmt) return -1;
    g_fmt->interrupt_callback.callback = interrupt_cb;
    g_fmt->interrupt_callback.opaque = NULL;

    if (avformat_open_input(&g_fmt, path, NULL, NULL) < 0) { mp_close(); return -1; }
    if (avformat_find_stream_info(g_fmt, NULL) < 0)        { mp_close(); return -1; }

    g_stream_idx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (g_stream_idx < 0) { mp_close(); return -1; }

    AVStream* st = g_fmt->streams[g_stream_idx];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) { mp_close(); return -1; }

    g_codec = avcodec_alloc_context3(dec);
    if (!g_codec) { mp_close(); return -1; }
    if (avcodec_parameters_to_context(g_codec, st->codecpar) < 0) { mp_close(); return -1; }
    if (avcodec_open2(g_codec, dec, NULL) < 0)                    { mp_close(); return -1; }

    g_src_rate = g_codec->sample_rate > 0 ? (double)g_codec->sample_rate : 44100.0;
    g_duration = 0.0;
    if (g_fmt->duration > 0)
        g_duration = (double)g_fmt->duration / AV_TIME_BASE;
    else if (st->duration > 0)
        g_duration = (double)st->duration * av_q2d(st->time_base);

    free(g_path);
    g_path = _strdup(path);

    if (setup_swr() < 0) { mp_close(); return -1; }

    ring_clear(&g_ring);
    g_eof = 0;
    g_interrupt = 0;
    g_samples_decoded = 0;
    g_state = MP_STATE_PLAYING;
    g_wake = 1;
    return 0;
}

void mp_close(void)
{
    if (g_state != MP_STATE_STOPPED || g_decoding) {
        g_interrupt = 1;
        while (g_decoding) Sleep(5);
    }
    g_interrupt = 0;
    if (g_swr) { EnterCriticalSection(&g_swr_lock); swr_free(&g_swr); g_swr = NULL; LeaveCriticalSection(&g_swr_lock); }
    if (g_codec) { avcodec_free_context(&g_codec); g_codec = NULL; }
    if (g_fmt) { avformat_close_input(&g_fmt); g_fmt = NULL; }
    free(g_path); g_path = NULL;
    g_stream_idx = -1;
    g_duration = 0.0;
    g_eof = 0;
    g_samples_decoded = 0;
    ring_clear(&g_ring);
    g_state = MP_STATE_STOPPED;
}

void mp_play(void)
{
    if (g_state == MP_STATE_PAUSED || g_state == MP_STATE_FINISHED || g_state == MP_STATE_STOPPED) {
        if (g_fmt) {
            /* retour à 0 si on était en FINISHED, sinon reprise */
            if (g_state == MP_STATE_FINISHED) {
                g_interrupt = 1;
                while (g_decoding) Sleep(5);
                g_interrupt = 0;
                av_seek_frame(g_fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(g_codec);
                ring_clear(&g_ring);
                g_eof = 0;
                g_samples_decoded = 0;
            }
            g_state = MP_STATE_PLAYING;
            g_wake = 1;
        }
    }
}

void mp_pause(void)
{
    if (g_state == MP_STATE_PLAYING)
        InterlockedCompareExchange((LONG*)&g_state, MP_STATE_PAUSED, MP_STATE_PLAYING);
}

void mp_play_pause(void)
{
    if (g_state == MP_STATE_PLAYING) mp_pause();
    else mp_play();
}

void mp_stop(void)
{
    if (g_state == MP_STATE_STOPPED) return;
    /* arrête le décodage en cours */
    g_interrupt = 1;
    while (g_decoding) Sleep(5);
    g_interrupt = 0;
    /* retour à 0 */
    if (g_fmt) {
        av_seek_frame(g_fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
        if (g_codec) avcodec_flush_buffers(g_codec);
    }
    ring_clear(&g_ring);
    g_eof = 0;
    g_samples_decoded = 0;
    g_state = MP_STATE_STOPPED;
}

void mp_set_volume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;   /* 0..100% + booster jusqu'à 200% */
    g_volume_pct = (LONG)(v * 100.0f + 0.5f);
    if (g_device_ok)
        ma_device_set_master_volume(&g_device, v);
}

float mp_get_volume(void) { return (float)g_volume_pct / 100.0f; }

void mp_set_speed(float s)
{
    if (s < 0.5f) s = 0.5f;
    if (s > 2.0f) s = 2.0f;
    g_speed = s;
    setup_swr();   /* recrée le resampler : effet au prochain bloc */
}

float mp_get_speed(void) { return g_speed; }

int mp_audio_device_ok(void) { return g_device_ok; }

mp_state mp_get_state(void) { return (mp_state)g_state; }

double mp_get_position(void) { return (double)g_samples_decoded / g_src_rate; }

double mp_get_duration(void) { return g_duration; }

const char* mp_get_file_name(void) { return g_path; }
