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

static uint32_t g_device_rate = 44100; /* taux du périph (défini plus bas) */

/* ------------------------------------------------------------------ */
/* Mode DJ local : platine B (2e décodeur FFmpeg) mixé dans le device  */
/* ------------------------------------------------------------------ */
static AVFormatContext* g_dj_fmt = NULL;
static AVCodecContext*  g_dj_codec = NULL;
static SwrContext*      g_dj_swr = NULL;
static int              g_dj_stream = -1;
static volatile LONG    g_dj_active = 0;   /* platine B en lecture */
static volatile LONG    g_dj_eof = 0;
static volatile LONG    g_dj_paused = 0;
static volatile float   g_dj_vol_a = 1.0f; /* volumes des platines */
static volatile float   g_dj_vol_b = 1.0f;
static volatile float   g_dj_xf = 0.5f;    /* crossfader 0 = A, 1 = B */
static float            g_dj_buf[4096 * 2];/* tampon de décodage B */
static LONG             g_dj_buf_n = 0;
static LONG             g_dj_buf_pos = 0;

int mp_dj_b_open(const char* path)
{
    mp_dj_b_close();
    if (avformat_open_input(&g_dj_fmt, path, NULL, NULL) != 0) return -1;
    if (avformat_find_stream_info(g_dj_fmt, NULL) < 0) return -1;
    for (unsigned i = 0; i < g_dj_fmt->nb_streams; i++) {
        AVCodecParameters* p = g_dj_fmt->streams[i]->codecpar;
        if (p->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const AVCodec* codec = avcodec_find_decoder(p->codec_id);
        if (!codec) return -1;
        g_dj_codec = avcodec_alloc_context3(codec);
        if (!g_dj_codec) return -1;
        if (avcodec_parameters_to_context(g_dj_codec, p) < 0) return -1;
        if (avcodec_open2(g_dj_codec, codec, NULL) < 0) return -1;
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&g_dj_swr, &out_layout, AV_SAMPLE_FMT_FLT,
                                (int)g_device_rate,
                                &g_dj_codec->ch_layout, g_dj_codec->sample_fmt,
                                g_dj_codec->sample_rate, 0, NULL) == 0 &&
            g_dj_swr)
            swr_init(g_dj_swr);
        g_dj_stream = (int)i;
        break;
    }
    if (!g_dj_codec) return -1;
    g_dj_eof = 0;
    g_dj_paused = 0;
    g_dj_buf_n = 0;
    g_dj_buf_pos = 0;
    InterlockedExchange(&g_dj_active, 1);
    return 0;
}

void mp_dj_b_close(void)
{
    InterlockedExchange(&g_dj_active, 0);
    if (g_dj_swr) { swr_free(&g_dj_swr); g_dj_swr = NULL; }
    if (g_dj_codec) { avcodec_free_context(&g_dj_codec); g_dj_codec = NULL; }
    if (g_dj_fmt) { avformat_close_input(&g_dj_fmt); g_dj_fmt = NULL; }
    g_dj_stream = -1;
    g_dj_buf_n = 0;
    g_dj_buf_pos = 0;
}

int mp_dj_b_active(void) { return (int)g_dj_active; }
void mp_dj_b_pause(void) { g_dj_paused = !g_dj_paused; }
int  mp_dj_b_paused(void) { return (int)g_dj_paused; }
void mp_dj_b_set_vol(float v)
{
    if (v < 0.0f) v = 0.0f; else if (v > 1.5f) v = 1.5f;
    g_dj_vol_b = v;
}
float mp_dj_b_get_vol(void) { return g_dj_vol_b; }
void mp_dj_a_set_vol(float v)
{
    if (v < 0.0f) v = 0.0f; else if (v > 1.5f) v = 1.5f;
    g_dj_vol_a = v;
}
float mp_dj_a_get_vol(void) { return g_dj_vol_a; }
void mp_dj_set_xf(float x)
{
    if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
    g_dj_xf = x;
}
float mp_dj_get_xf(void) { return g_dj_xf; }

/* Remplit dst (frames stéréo) avec le décodage de la platine B. */
static uint32_t dj_b_fill(float* dst, uint32_t frames)
{
    uint32_t out = 0;
    while (out < frames) {
        if (g_dj_buf_pos < g_dj_buf_n) {
            uint32_t n = (uint32_t)(g_dj_buf_n - g_dj_buf_pos);
            if (n > frames - out) n = frames - out;
            memcpy(dst + (size_t)out * 2,
                   g_dj_buf + (size_t)g_dj_buf_pos * 2,
                   (size_t)n * 2 * sizeof(float));
            g_dj_buf_pos += (LONG)n;
            out += n;
            continue;
        }
        if (g_dj_eof) break;
        AVPacket* pkt = av_packet_alloc();
        AVFrame* fr = av_frame_alloc();
        int got_any = 0;
        while (!got_any && !g_dj_eof) {
            int r = av_read_frame(g_dj_fmt, pkt);
            if (r < 0) { g_dj_eof = 1; break; }
            if (pkt->stream_index != g_dj_stream) {
                av_packet_unref(pkt);
                continue;
            }
            if (avcodec_send_packet(g_dj_codec, pkt) == 0) {
                while (avcodec_receive_frame(g_dj_codec, fr) == 0) {
                    if (g_dj_swr) {
                        uint8_t* out_ptrs[1] = { (uint8_t*)g_dj_buf };
                        int got = swr_convert(g_dj_swr, out_ptrs, 4096,
                                              (const uint8_t**)fr->extended_data,
                                              fr->nb_samples);
                        if (got > 0) {
                            g_dj_buf_n = got;
                            g_dj_buf_pos = 0;
                            got_any = 1;
                        }
                    }
                    av_frame_unref(fr);
                    if (got_any) break;
                }
            }
            av_packet_unref(pkt);
        }
        av_frame_free(&fr);
        av_packet_free(&pkt);
        if (!got_any) break;
    }
    return out;
}

/* Lecture de la platine B (appelé par le callback du device). */
uint32_t mp_dj_b_read(float* dst, uint32_t frames)
{
    if (!g_dj_active || g_dj_paused) {
        memset(dst, 0, (size_t)frames * 2 * sizeof(float));
        return frames;
    }
    uint32_t n = dj_b_fill(dst, frames);
    if (n < frames)
        memset(dst + (size_t)n * 2, 0, (size_t)(frames - n) * 2 * sizeof(float));
    return frames;
}

/* Mixe la platine B dans dst (volumes + crossfader). */
void mp_dj_mix_into(float* dst, uint32_t frames)
{
    if (!g_dj_active) {
        for (uint32_t i = 0; i < frames * 2; i++)
            dst[i] *= g_dj_vol_a;
        return;
    }
    float tmp[4096 * 2];
    mp_dj_b_read(tmp, frames);
    for (uint32_t i = 0; i < frames * 2; i++) {
        float v = dst[i] * g_dj_vol_a * (1.0f - g_dj_xf) +
                  tmp[i] * g_dj_vol_b * g_dj_xf;
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        dst[i] = v;
    }
}



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

static ring_t         g_ring;

/* ------------------------------------------------------------------ */
/* Diffusion TeamSpeak : 2e sortie miniaudio vers un périphérique      */
/* (ex. Virtual Audio Cable "CABLE Input" — à sélectionner comme       */
/* micro dans TeamSpeak 3)                                             */
/* ------------------------------------------------------------------ */
static ma_device g_ts_device;
static ma_device_id g_ts_id;
static int        g_ts_has_id = 0;
static volatile LONG g_ts_active = 0;

static void ts_cb(ma_device* dev, void* out, const void* in, ma_uint32 frames)
{
    (void)dev; (void)in;
    float* dst = (float*)out;
    uint32_t got = ring_read(&g_ring, dst, frames);
    if (got < frames)
        memset(dst + (size_t)got * 2, 0,
               (size_t)(frames - got) * 2 * sizeof(float));
    /* platine B du mode DJ incluse dans la diffusion */
    mp_dj_mix_into(dst, frames);
    mp_plugins_audio_process(dst, frames, 2, 44100);
}

/* Liste les périphériques de sortie (noms UTF-8). Retourne le compte. */
int mp_ts_devices(char names[][256], int max)
{
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return 0;
    ma_device_info* infos = NULL;
    ma_uint32 n = 0;
    int count = 0;
    if (ma_context_get_devices(&ctx, NULL, NULL, &infos, &n) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < n && count < max; i++) {
            strncpy(names[count], infos[i].name, 255);
            names[count][255] = 0;
            count++;
        }
    }
    ma_context_uninit(&ctx);
    return count;
}

/* Démarre la diffusion vers le périphérique nommé (UTF-8). 0 = OK. */
int mp_ts_start(const char* name_utf8)
{
    mp_ts_stop();
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return -1;
    ma_device_info* infos = NULL;
    ma_uint32 n = 0;
    g_ts_has_id = 0;
    if (ma_context_get_devices(&ctx, NULL, NULL, &infos, &n) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < n; i++) {
            if (!strcmp(infos[i].name, name_utf8)) {
                g_ts_id = infos[i].id;
                g_ts_has_id = 1;
                break;
            }
        }
    }
    ma_context_uninit(&ctx);
    if (!g_ts_has_id) return -1;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 44100;
    cfg.dataCallback = ts_cb;
    cfg.playback.pDeviceID = &g_ts_id;
    if (ma_device_init(NULL, &cfg, &g_ts_device) != MA_SUCCESS) return -1;
    if (ma_device_start(&g_ts_device) != MA_SUCCESS) {
        ma_device_uninit(&g_ts_device);
        return -1;
    }
    InterlockedExchange(&g_ts_active, 1);
    return 0;
}

void mp_ts_stop(void)
{
    if (g_ts_active) {
        ma_device_uninit(&g_ts_device);
        InterlockedExchange(&g_ts_active, 0);
    }
}

int mp_ts_active(void) { return (int)g_ts_active; }
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
static volatile LONG  g_audio_out = 0;        /* 0 = PC, 1 = téléphone, 2 = les deux */

/* ------------------------------------------------------------------ */
/* Ring de diffusion web (téléphone) — best effort                     */
/* ------------------------------------------------------------------ */
#define WEB_RING_FRAMES (1 << 15)   /* 32768 frames ≈ 0,74 s */
#define WEB_RING_MASK   (WEB_RING_FRAMES - 1)
static float          g_web_ring[WEB_RING_FRAMES * 2];
static volatile LONG  g_web_head = 0;
static volatile LONG  g_web_tail = 0;

/* Écrit les frames décodées (float stéréo, déjà à la vitesse choisie).
 * Non bloquant : si le ring est plein, les données les plus anciennes
 * sont jetées (personne n'écoute → on ne ralentit pas la lecture). */
static void web_ring_write(const float* data, uint32_t frames)
{
    LONG h = g_web_head, t = g_web_tail;
    uint32_t used = (uint32_t)(t - h) & WEB_RING_MASK;
    if (used + frames > WEB_RING_FRAMES) {
        uint32_t drop = used + frames - WEB_RING_FRAMES;
        g_web_head = (LONG)((uint32_t)h + drop);
    }
    uint32_t wpos = (uint32_t)t & WEB_RING_MASK;
    uint32_t n1 = frames;
    if (n1 > WEB_RING_FRAMES - wpos) n1 = WEB_RING_FRAMES - wpos;
    memcpy(g_web_ring + (size_t)wpos * 2, data, (size_t)n1 * 2 * sizeof(float));
    if (n1 < frames)
        memcpy(g_web_ring, data + (size_t)n1 * 2,
               (size_t)(frames - n1) * 2 * sizeof(float));
    InterlockedExchange(&g_web_tail, (LONG)((uint32_t)t + frames));
}

uint32_t mp_web_read(float* dst, uint32_t frames)
{
    LONG h = g_web_head, t = g_web_tail;
    uint32_t used = (uint32_t)(t - h) & WEB_RING_MASK;
    if (used == 0) return 0;
    if (frames > used) frames = used;
    uint32_t rpos = (uint32_t)h & WEB_RING_MASK;
    uint32_t n1 = frames;
    if (n1 > WEB_RING_FRAMES - rpos) n1 = WEB_RING_FRAMES - rpos;
    memcpy(dst, g_web_ring + (size_t)rpos * 2, (size_t)n1 * 2 * sizeof(float));
    if (n1 < frames)
        memcpy(dst + (size_t)n1 * 2, g_web_ring,
               (size_t)(frames - n1) * 2 * sizeof(float));
    InterlockedExchange(&g_web_head, (LONG)((uint32_t)h + frames));
    return frames;
}

void mp_set_audio_out(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_audio_out = mode;
}

int mp_get_audio_out(void) { return (int)g_audio_out; }

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
                            web_ring_write(out_buf + (size_t)pushed * 2, w);
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
    } else {
        memset(dst, 0, (size_t)frames * 2 * sizeof(float));
    }

    /* platine B (mode DJ local) : mixée avec la platine A */
    mp_dj_mix_into(dst, frames);

    /* effets audio des plugins (si le périph a le bon format) */
    mp_plugins_audio_process(dst, frames, 2, g_device_rate);
    /* flux lecture seule pour les plugins visuels */
    mp_plugins_audio_frames(dst, frames, 2, g_device_rate);
    /* sortie "téléphone seul" : le haut-parleur local reste muet */
    if (g_audio_out == 1)
        memset(dst, 0, (size_t)frames * 2 * sizeof(float));
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

void mp_seek(double seconds)
{
    if (!g_fmt || g_state == MP_STATE_STOPPED) return;
    if (seconds < 0.0) seconds = 0.0;
    if (g_duration > 0.0 && seconds > g_duration) seconds = g_duration;

    /* arrête la boucle de décodage en cours */
    g_interrupt = 1;
    while (g_decoding) Sleep(5);
    g_interrupt = 0;

    /* seek dans le flux + purge du décodeur */
    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    av_seek_frame(g_fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (g_codec) avcodec_flush_buffers(g_codec);
    ring_clear(&g_ring);
    g_eof = 0;
    g_samples_decoded = (LONG64)(seconds * g_src_rate);

    if (g_state == MP_STATE_FINISHED) g_state = MP_STATE_PAUSED;
    if (g_state == MP_STATE_PLAYING) g_wake = 1;
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
