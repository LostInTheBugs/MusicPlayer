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
#include <math.h>
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
/* Décodage dans un thread dédié + ring buffer SPSC (comme la platine  */
/* A : aucune I/O bloquante dans le callback audio).                   */
/* ------------------------------------------------------------------ */
static AVFormatContext* g_dj_fmt = NULL;
static AVCodecContext*  g_dj_codec = NULL;
static SwrContext*      g_dj_swr = NULL;
static int              g_dj_stream = -1;
static volatile LONG    g_dj_active = 0;   /* platine B en lecture */
static volatile LONG    g_dj_eof = 0;
static volatile LONG    g_dj_paused = 0;
static volatile LONG    g_dj_stop = 0;     /* arrêt demandé du thread */
static HANDLE           g_dj_thread = NULL;
static volatile float   g_dj_vol_a = 1.0f; /* volumes des platines */
static volatile float   g_dj_vol_b = 1.0f;
static volatile float   g_dj_xf = 0.5f;    /* crossfader 0 = A, 1 = B */

/* Ring SPSC de la platine B : 2^16 frames stéréo f32 ≈ 1,5 s à 44,1 kHz */
#define DJB_RING_FRAMES (1u << 16)
#define DJB_RING_MASK   (DJB_RING_FRAMES - 1)
static float            g_djb_ring[DJB_RING_FRAMES * 2];
static LONG             g_djb_head = 0;    /* écrit par le thread décodeur */
static LONG             g_djb_tail = 0;    /* lu par le callback audio */

static void dj_ring_write(const float* data, uint32_t frames)
{
    LONG h = g_djb_head;
    LONG t = __atomic_load_n(&g_djb_tail, __ATOMIC_ACQUIRE);
    uint32_t filled = (uint32_t)h - (uint32_t)t;
    uint32_t space = DJB_RING_FRAMES - filled;
    if (frames > space) frames = space;
    if (frames == 0) return;
    uint32_t wpos = (uint32_t)h & DJB_RING_MASK;
    uint32_t n1 = frames;
    if (n1 > DJB_RING_FRAMES - wpos) n1 = DJB_RING_FRAMES - wpos;
    memcpy(g_djb_ring + (size_t)wpos * 2, data, (size_t)n1 * 2 * sizeof(float));
    if (n1 < frames)
        memcpy(g_djb_ring, data + (size_t)n1 * 2,
               (size_t)(frames - n1) * 2 * sizeof(float));
    __atomic_store_n(&g_djb_head, (LONG)((uint32_t)h + frames),
                     __ATOMIC_RELEASE);
}

static uint32_t dj_ring_read(float* dst, uint32_t frames)
{
    LONG h = __atomic_load_n(&g_djb_head, __ATOMIC_ACQUIRE);
    LONG t = g_djb_tail;
    uint32_t filled = (uint32_t)h - (uint32_t)t;
    if (frames > filled) frames = filled;
    if (frames == 0) return 0;
    uint32_t rpos = (uint32_t)t & DJB_RING_MASK;
    uint32_t n1 = frames;
    if (n1 > DJB_RING_FRAMES - rpos) n1 = DJB_RING_FRAMES - rpos;
    memcpy(dst, g_djb_ring + (size_t)rpos * 2, (size_t)n1 * 2 * sizeof(float));
    if (n1 < frames)
        memcpy(dst + (size_t)n1 * 2, g_djb_ring,
               (size_t)(frames - n1) * 2 * sizeof(float));
    __atomic_store_n(&g_djb_tail, (LONG)((uint32_t)t + frames),
                     __ATOMIC_RELEASE);
    return frames;
}

/* interruption de av_read_frame : arrêt rapide du thread de décodage */
static int dj_interrupt_cb(void* opaque)
{
    (void)opaque;
    return g_dj_stop ? 1 : 0;
}

/* Thread de décodage de la platine B */
static DWORD WINAPI dj_decode_thread(LPVOID arg)
{
    (void)arg;
    while (!g_dj_stop) {
        if (!g_dj_active || g_dj_paused || !g_dj_fmt || !g_dj_codec) {
            Sleep(5);
            continue;
        }
        uint32_t filled = (uint32_t)g_djb_head - (uint32_t)g_djb_tail;
        if (DJB_RING_FRAMES - filled < 512) { Sleep(5); continue; }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* fr = av_frame_alloc();
        float buf[8192 * 2];
        int got = 0;
        while (!got && !g_dj_stop && g_dj_active && !g_dj_eof) {
            int r = av_read_frame(g_dj_fmt, pkt);
            if (r < 0) { g_dj_eof = 1; break; }
            if (pkt->stream_index != g_dj_stream) {
                av_packet_unref(pkt);
                continue;
            }
            if (avcodec_send_packet(g_dj_codec, pkt) == 0) {
                while (!got && avcodec_receive_frame(g_dj_codec, fr) == 0) {
                    if (g_dj_swr) {
                        uint8_t* out_ptrs[1] = { (uint8_t*)buf };
                        int n = swr_convert(g_dj_swr, out_ptrs, 8192,
                                            (const uint8_t**)fr->extended_data,
                                            fr->nb_samples);
                        if (n > 0) got = n;
                    }
                    av_frame_unref(fr);
                }
            }
            av_packet_unref(pkt);
        }
        av_frame_free(&fr);
        av_packet_free(&pkt);
        if (got > 0)
            dj_ring_write(buf, (uint32_t)got);
        else
            Sleep(10);   /* fin de piste ou rien à décoder : évite le spin */
    }
    return 0;
}

int mp_dj_b_open(const char* path)
{
    mp_dj_b_close();
    int ret = -1;
    int stream = -1;
    AVFormatContext* fmt = NULL;
    AVCodecContext*  codec = NULL;
    SwrContext*      swr = NULL;

    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) goto done;
    fmt->interrupt_callback.callback = dj_interrupt_cb;
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters* p = fmt->streams[i]->codecpar;
        if (p->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const AVCodec* dec = avcodec_find_decoder(p->codec_id);
        if (!dec) goto done;
        codec = avcodec_alloc_context3(dec);
        if (!codec) goto done;
        if (avcodec_parameters_to_context(codec, p) < 0) goto done;
        if (avcodec_open2(codec, dec, NULL) < 0) goto done;
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT,
                                (int)g_device_rate,
                                &codec->ch_layout, codec->sample_fmt,
                                codec->sample_rate, 0, NULL) == 0 && swr)
            swr_init(swr);
        stream = (int)i;
        break;
    }
    if (!codec || stream < 0) goto done;

    g_dj_fmt = fmt; g_dj_codec = codec; g_dj_swr = swr; g_dj_stream = stream;
    fmt = NULL; codec = NULL; swr = NULL;   /* transférés au global */
    g_dj_eof = 0;
    g_dj_paused = 0;
    g_djb_head = 0;
    g_djb_tail = 0;
    InterlockedExchange(&g_dj_active, 1);
    g_dj_stop = 0;
    g_dj_thread = CreateThread(NULL, 0, dj_decode_thread, NULL, 0, NULL);
    ret = 0;
done:
    /* chemins d'erreur : libérer proprement (pas de fuite) */
    if (swr) { swr_free(&swr); swr = NULL; }
    if (codec) { avcodec_free_context(&codec); codec = NULL; }
    if (fmt) { avformat_close_input(&fmt); fmt = NULL; }
    return ret;
}

void mp_dj_b_close(void)
{
    InterlockedExchange(&g_dj_active, 0);
    InterlockedExchange(&g_dj_stop, 1);
    if (g_dj_thread) {
        /* l'interrupt_callback fait sortir av_read_frame : le thread
         * se termine vite, puis on libère les contextes en sécurité */
        WaitForSingleObject(g_dj_thread, 3000);
        CloseHandle(g_dj_thread);
        g_dj_thread = NULL;
    }
    g_dj_stop = 0;
    if (g_dj_swr) { swr_free(&g_dj_swr); g_dj_swr = NULL; }
    if (g_dj_codec) { avcodec_free_context(&g_dj_codec); g_dj_codec = NULL; }
    if (g_dj_fmt) { avformat_close_input(&g_dj_fmt); g_dj_fmt = NULL; }
    g_dj_stream = -1;
    g_djb_head = 0;
    g_djb_tail = 0;
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

/* Lecture de la platine B (appelé par le callback du device). */
uint32_t mp_dj_b_read(float* dst, uint32_t frames)
{
    if (!g_dj_active || g_dj_paused) {
        memset(dst, 0, (size_t)frames * 2 * sizeof(float));
        return frames;
    }
    uint32_t n = dj_ring_read(dst, frames);
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
    /* par blocs de 4096 : pas de gros tampon fixe sur la pile du
     * callback (frames peut dépasser 4096 sur certains périphs) */
    uint32_t off = 0;
    while (off < frames) {
        uint32_t chunk = frames - off;
        if (chunk > 4096) chunk = 4096;
        float tmp[4096 * 2];
        mp_dj_b_read(tmp, chunk);
        for (uint32_t i = 0; i < chunk * 2; i++) {
            float v = dst[off * 2 + i] * g_dj_vol_a * (1.0f - g_dj_xf) +
                      tmp[i] * g_dj_vol_b * g_dj_xf;
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            dst[off * 2 + i] = v;
        }
        off += chunk;
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
    LONG head;             /* écrit par le décodeur (store RELEASE) */
    LONG tail;             /* lu par le callback audio (store RELEASE) */
} ring_t;

static void ring_init(ring_t* r)
{
    r->buf = (float*)malloc(RING_FRAMES * 2 * sizeof(float));
    r->head = 0;
    r->tail = 0;
}

/* barrières mémoire explicites : la visibilité des données écrites
 * avant le store RELEASE est garantie pour le load ACQUIRE de l'autre
 * thread (contrairement à volatile seul sous GCC) */
#ifndef MP_CORE
static void ring_clear(ring_t* r)
{
    __atomic_store_n(&r->head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->tail, 0, __ATOMIC_RELAXED);
}
#else
/* core : le ring principal n'est pas utilisé, mais les fonctions
 * (close/seek/stop) l'appellent pour rester identiques */
static void ring_clear(ring_t* r) { (void)r; }
#endif

#ifndef MP_CORE
static uint32_t ring_write(ring_t* r, const float* data, uint32_t frames)
{
    LONG t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    LONG h = r->head;   /* seul le producteur écrit head */
    uint32_t filled = (uint32_t)h - (uint32_t)t;
    uint32_t space = RING_FRAMES - filled;
    if (frames > space) frames = space;
    if (frames == 0) return 0;

    uint32_t hp = (uint32_t)h & RING_MASK;
    uint32_t n1 = frames;
    if (n1 > RING_FRAMES - hp) n1 = RING_FRAMES - hp;
    memcpy(r->buf + hp * 2, data, (size_t)n1 * 2 * sizeof(float));
    memcpy(r->buf, data + (size_t)n1 * 2, (size_t)(frames - n1) * 2 * sizeof(float));

    __atomic_store_n(&r->head, (LONG)((uint32_t)h + frames), __ATOMIC_RELEASE);
    return frames;
}
#endif /* !MP_CORE */

#ifndef MP_CORE
static uint32_t ring_read(ring_t* r, float* dst, uint32_t frames)
{
    LONG h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    LONG t = r->tail;   /* seul le consommateur écrit tail */
    uint32_t filled = (uint32_t)h - (uint32_t)t;
    if (frames > filled) frames = filled;
    if (frames == 0) return 0;

    uint32_t tp = (uint32_t)t & RING_MASK;
    uint32_t n1 = frames;
    if (n1 > RING_FRAMES - tp) n1 = RING_FRAMES - tp;
    memcpy(dst, r->buf + tp * 2, (size_t)n1 * 2 * sizeof(float));
    memcpy(dst + (size_t)n1 * 2, r->buf, (size_t)(frames - n1) * 2 * sizeof(float));

    __atomic_store_n(&r->tail, (LONG)((uint32_t)t + frames), __ATOMIC_RELEASE);
    return frames;
}
#endif /* !MP_CORE */

/* ------------------------------------------------------------------ */
/* État global                                                        */
/* ------------------------------------------------------------------ */
static ma_device      g_device;
static int            g_device_ok = 0;

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
static LONG64         g_played = 0;      /* frames réellement jouées
                                            (comptées dans le callback) */
static volatile LONG  g_audio_out = 0;   /* 0 = PC, 1 = téléphone, 2 = les deux */

/* ------------------------------------------------------------------ */
/* Ring de diffusion web (téléphone) — best effort                     */
/* ------------------------------------------------------------------ */
#define WEB_RING_FRAMES (1 << 15)   /* 32768 frames ≈ 0,74 s */
#define WEB_RING_MASK   (WEB_RING_FRAMES - 1)
static float          g_web_ring[WEB_RING_FRAMES * 2];
static LONG           g_web_tail = 0;      /* écrit par le décodeur */
static LONG           g_web_rd[MP_WEB_READERS];       /* curseur de chaque lecteur */
static LONG           g_web_rd_used[MP_WEB_READERS];  /* 1 = curseur réservé */
static CRITICAL_SECTION g_web_rd_lock;     /* protège l'ouverture/fermeture */

/* Position en deçà de laquelle plus aucun lecteur n'a besoin des
 * données. S'il n'y a aucun lecteur ouvert, on renvoie l'écriture
 * (rien à conserver). */
static LONG web_read_floor(void)
{
    LONG wr = __atomic_load_n(&g_web_tail, __ATOMIC_ACQUIRE);
    LONG floor = wr;
    int any = 0;
    for (int i = 0; i < MP_WEB_READERS; i++) {
        if (!g_web_rd_used[i]) continue;
        LONG r = __atomic_load_n(&g_web_rd[i], __ATOMIC_ACQUIRE);
        if (!any || (int32_t)(r - floor) < 0) floor = r;
        any = 1;
    }
    return floor;
}

/* Écrit les frames décodées (float stéréo, déjà à la vitesse choisie).
 * Non bloquant : si le ring est plein, les données les plus anciennes
 * sont jetées (personne n'écoute → on ne ralentit pas la lecture).
 * Accès SPSC avec barrières mémoire explicites (__atomic). */
#ifndef MP_CORE
static void web_ring_write(const float* data, uint32_t frames)
{
    LONG wr = g_web_tail;
    LONG fl = web_read_floor();
    uint32_t used = (uint32_t)wr - (uint32_t)fl;
    if (used + frames > WEB_RING_FRAMES) {
        /* jette les plus anciennes : avance les lecteurs trop en retard */
        uint32_t drop = used + frames - WEB_RING_FRAMES;
        uint32_t newfl = (uint32_t)wr - drop;
        for (int i = 0; i < MP_WEB_READERS; i++) {
            if (!g_web_rd_used[i]) continue;
            LONG r = __atomic_load_n(&g_web_rd[i], __ATOMIC_ACQUIRE);
            if ((uint32_t)r < newfl)
                __atomic_store_n(&g_web_rd[i], (LONG)newfl, __ATOMIC_RELEASE);
        }
        fl = (LONG)newfl;
    }
    uint32_t wpos = (uint32_t)wr & WEB_RING_MASK;
    uint32_t n1 = frames;
    if (n1 > WEB_RING_FRAMES - wpos) n1 = WEB_RING_FRAMES - wpos;
    memcpy(g_web_ring + (size_t)wpos * 2, data, (size_t)n1 * 2 * sizeof(float));
    if (n1 < frames)
        memcpy(g_web_ring, data + (size_t)n1 * 2,
               (size_t)(frames - n1) * 2 * sizeof(float));
    __atomic_store_n(&g_web_tail, (LONG)((uint32_t)wr + frames),
                     __ATOMIC_RELEASE);
}
#endif /* !MP_CORE */

#ifdef MP_CORE
/* Variante bloquante (core) : attend que de la place se libère, pour
 * que le décodage avance au rythme des clients qui consomment. */
static uint32_t web_ring_write_bp(const float* data, uint32_t frames)
{
    uint32_t pushed = 0;
    int stalled = 0;
    /* Le décodage doit rester interruptible : sans les tests
     * g_interrupt / g_state, mp_stop() et mp_seek() attendent
     * g_decoding indéfiniment dès que le ring est plein. */
    while (pushed < frames && !g_shutdown && !g_interrupt &&
           g_state == MP_STATE_PLAYING) {
        LONG wr = g_web_tail;
        LONG fl = web_read_floor();
        uint32_t used = (uint32_t)wr - (uint32_t)fl;
        uint32_t free = WEB_RING_FRAMES - used;
        uint32_t w = frames - pushed;
        if (w > free) w = free;
        if (w == 0) {
            if (++stalled > 400) {          /* 400 × 5 ms = 2 s sans progrès */
                /* Un lecteur ne consomme plus (socket mort, client tué…).
                 * On le recale de force plutôt que de bloquer le moteur
                 * pour tout le monde : il perd des données, les autres
                 * continuent. */
                EnterCriticalSection(&g_web_rd_lock);
                for (int i = 0; i < MP_WEB_READERS; i++) {
                    if (!g_web_rd_used[i]) continue;
                    LONG r = __atomic_load_n(&g_web_rd[i], __ATOMIC_ACQUIRE);
                    if ((uint32_t)((uint32_t)wr - (uint32_t)r) >= WEB_RING_FRAMES)
                        __atomic_store_n(&g_web_rd[i],
                            (LONG)((uint32_t)wr - WEB_RING_FRAMES / 2),
                            __ATOMIC_RELEASE);
                }
                LeaveCriticalSection(&g_web_rd_lock);
                stalled = 0;
            }
            Sleep(5);
            continue;
        }
        stalled = 0;
        uint32_t wpos = (uint32_t)wr & WEB_RING_MASK;
        uint32_t n1 = w;
        if (n1 > WEB_RING_FRAMES - wpos) n1 = WEB_RING_FRAMES - wpos;
        memcpy(g_web_ring + (size_t)wpos * 2, data + (size_t)pushed * 2,
               (size_t)n1 * 2 * sizeof(float));
        if (n1 < w)
            memcpy(g_web_ring, data + (size_t)(pushed + n1) * 2,
                   (size_t)(w - n1) * 2 * sizeof(float));
        __atomic_store_n(&g_web_tail, (LONG)((uint32_t)wr + w),
                         __ATOMIC_RELEASE);
        pushed += w;
    }
    return pushed;
}
#endif /* MP_CORE */

/* Variante sans verrou : l'appelant doit déjà tenir g_web_rd_lock. */
static int mp_web_reader_open_locked(void)
{
    for (int i = 0; i < MP_WEB_READERS; i++) {
        if (!g_web_rd_used[i]) {
            g_web_rd_used[i] = 1;
            /* démarre sur les données les plus récentes */
            __atomic_store_n(&g_web_rd[i],
                             __atomic_load_n(&g_web_tail, __ATOMIC_ACQUIRE),
                             __ATOMIC_RELEASE);
            return i;
        }
    }
    return -1;
}

int mp_web_reader_open(void)
{
    EnterCriticalSection(&g_web_rd_lock);
    int id = mp_web_reader_open_locked();
    LeaveCriticalSection(&g_web_rd_lock);
    return id;
}

void mp_web_reader_close(int id)
{
    if (id < 0 || id >= MP_WEB_READERS) return;
    EnterCriticalSection(&g_web_rd_lock);
    g_web_rd_used[id] = 0;
    LeaveCriticalSection(&g_web_rd_lock);
}

uint32_t mp_web_read_n(int id, float* dst, uint32_t frames)
{
    if (id < 0 || id >= MP_WEB_READERS || !g_web_rd_used[id]) return 0;
    LONG wr = __atomic_load_n(&g_web_tail, __ATOMIC_ACQUIRE);
    LONG rd = __atomic_load_n(&g_web_rd[id], __ATOMIC_ACQUIRE);
    uint32_t used = (uint32_t)wr - (uint32_t)rd;
    if (used > WEB_RING_FRAMES) {          /* lecteur décroché : recalage */
        rd = (LONG)((uint32_t)wr - WEB_RING_FRAMES);
        used = WEB_RING_FRAMES;
    }
#ifdef MP_CORE
    if (used == 0) {
        if (g_eof && g_state == MP_STATE_PLAYING)
            InterlockedCompareExchange((LONG*)&g_state, MP_STATE_FINISHED,
                                       MP_STATE_PLAYING);
        return 0;
    }
#else
    if (used == 0) return 0;
#endif
    if (frames > used) frames = used;

#ifdef MP_CORE
    LONG floor_before = web_read_floor();
#endif

    uint32_t rpos = (uint32_t)rd & WEB_RING_MASK;
    uint32_t n1 = frames;
    if (n1 > WEB_RING_FRAMES - rpos) n1 = WEB_RING_FRAMES - rpos;
    memcpy(dst, g_web_ring + (size_t)rpos * 2, (size_t)n1 * 2 * sizeof(float));
    if (n1 < frames)
        memcpy(dst + (size_t)n1 * 2, g_web_ring,
               (size_t)(frames - n1) * 2 * sizeof(float));
    __atomic_store_n(&g_web_rd[id], (LONG)((uint32_t)rd + frames),
                     __ATOMIC_RELEASE);

#ifdef MP_CORE
    /* La position du morceau avance quand les données sont
     * DÉFINITIVEMENT consommées, c'est-à-dire quand le lecteur le plus
     * en retard progresse — pas à chaque lecture d'un lecteur donné,
     * sinon deux clients feraient avancer la position deux fois. */
    LONG floor_after = web_read_floor();
    int32_t d = (int32_t)(floor_after - floor_before);
    if (d > 0) {
        float s = g_speed;
        if (s < 0.1f) s = 1.0f;
        LONG64 adv = (LONG64)llround((double)d / (double)s);
        if (adv < 1) adv = 1;
        __atomic_add_fetch(&g_played, adv, __ATOMIC_RELAXED);
    }
#endif
    return frames;
}

uint32_t mp_web_read(float* dst, uint32_t frames)
{
    /* Lecteur ouvert à la PREMIÈRE utilisation seulement : un lecteur
     * réservé mais jamais lu retiendrait le plancher de lecture et
     * bloquerait le moteur entier (cause de la panne 2026.08.040-c4). */
    static LONG s_id = -1;
    if (__atomic_load_n(&s_id, __ATOMIC_ACQUIRE) < 0) {
        EnterCriticalSection(&g_web_rd_lock);
        if (s_id < 0) {
            int id = mp_web_reader_open_locked();
            __atomic_store_n(&s_id, (LONG)id, __ATOMIC_RELEASE);
        }
        LeaveCriticalSection(&g_web_rd_lock);
    }
    LONG id = __atomic_load_n(&s_id, __ATOMIC_ACQUIRE);
    if (id < 0) return 0;
    return mp_web_read_n((int)id, dst, frames);
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
#ifdef MP_CORE
                            /* core : pas de carte son — la backpressure
                             * se fait sur le ring de diffusion : le
                             * décodage attend que les clients (/stream)
                             * consomment, donc avance au rythme réel */
                            uint32_t w = web_ring_write_bp(
                                out_buf + (size_t)pushed * 2,
                                (uint32_t)got - pushed);
                            pushed += w;
#else
                            uint32_t w = ring_write(&g_ring, out_buf + (size_t)pushed * 2,
                                                    (uint32_t)got - pushed);
                            web_ring_write(out_buf + (size_t)pushed * 2, w);
                            pushed += w;
                            if (pushed < (uint32_t)got) Sleep(5);
#endif
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
/* Callback miniaudio (CLIENT uniquement — le core n'a pas de carte    */
/* son : le flux part vers les clients via /stream)                    */
/* ------------------------------------------------------------------ */
#ifndef MP_CORE
static void data_cb(ma_device* dev, void* out, const void* in, ma_uint32 frames)
{
    (void)dev; (void)in;
    float* dst = (float*)out;

    if (g_state == MP_STATE_PLAYING) {
        uint32_t got = ring_read(&g_ring, dst, frames);
        if (got > 0) {
            /* position = temps DU MORCEAU : le resampler sort à
             * device_rate × speed, donc chaque bloc joué représente
             * got / speed frames de morceau ; pondérer par la vitesse
             * courante reste exact même si elle change en cours */
            float s = g_speed;
            if (s < 0.1f) s = 1.0f;
            LONG64 adv = (LONG64)llround((double)got / (double)s);
            if (adv < 1) adv = 1;
            __atomic_add_fetch(&g_played, adv, __ATOMIC_RELAXED);
        }
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
#endif /* !MP_CORE */

/* ------------------------------------------------------------------ */
/* API publique                                                        */
/* ------------------------------------------------------------------ */
int mp_init(void)
{
    InitializeCriticalSection(&g_swr_lock);
    InitializeCriticalSection(&g_web_rd_lock);
    ring_init(&g_ring);

#ifndef MP_CORE
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
#else
    /* core : pas de carte son — le flux est diffusé aux clients */
#endif

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
#ifndef MP_CORE
    if (g_device_ok) {
        ma_device_uninit(&g_device);
        g_device_ok = 0;
    }
#endif
    if (g_swr) { swr_free(&g_swr); g_swr = NULL; }
    if (g_codec) { avcodec_free_context(&g_codec); g_codec = NULL; }
    if (g_fmt) { avformat_close_input(&g_fmt); g_fmt = NULL; }
    free(g_path); g_path = NULL;
    g_stream_idx = -1;
    g_state = MP_STATE_STOPPED;
    DeleteCriticalSection(&g_web_rd_lock);
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

#ifndef MP_CORE
    ring_clear(&g_ring);
#endif
    g_eof = 0;
    g_interrupt = 0;
    g_played = 0;
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
    g_played = 0;
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
                g_played = 0;
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
    g_played = 0;
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
    __atomic_store_n(&g_played, (LONG64)(seconds * (double)g_device_rate),
                     __ATOMIC_RELAXED);

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

/* Position de lecture : frames réellement jouées (comptées à la sortie
 * du callback, après resampling → device_rate) divisées par le taux du
 * périphérique. Indépendant du speed et du remplissage du ring. */
double mp_get_position(void)
{
    return (double)__atomic_load_n(&g_played, __ATOMIC_ACQUIRE) /
           (double)g_device_rate;
}

double mp_get_duration(void) { return g_duration; }

const char* mp_get_file_name(void) { return g_path; }
