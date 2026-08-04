/* src/stream_player.c — lecteur de flux du client.
 *
 * Le client ne décode plus : il reçoit le PCM du moteur via
 * GET /stream (WAV 44,1 kHz stéréo 16 bits) sur un thread WinINet,
 * le met dans un ring local, et le callback miniaudio le joue avec
 * volume local + effets plugins + mix DJ + analyse visuels. */
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vendor/miniaudio.h"  /* implémentation fournie par player.c */

#include "stream_player.h"
#include "player.h"
#include "plugin_loader.h"
#include "client_core.h"

/* ------------------------------------------------------------------ */
/* Ring local (SPSC : thread réseau → callback audio)                  */
/* ------------------------------------------------------------------ */
#define SP_RING_FRAMES (1u << 15)   /* 32768 frames ≈ 0,74 s */
#define SP_RING_MASK   (SP_RING_FRAMES - 1)

static float* g_ring = NULL;
static LONG   g_head = 0;   /* écrit par le thread réseau (RELEASE) */
static LONG   g_tail = 0;   /* lu par le callback audio (RELEASE) */

static void sp_ring_write(const float* data, uint32_t frames)
{
    LONG h = g_head;
    LONG t = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
    uint32_t used = (uint32_t)(t - h) & SP_RING_MASK;
    if (used + frames > SP_RING_FRAMES) {
        /* dépassement : on jette les plus anciennes */
        uint32_t drop = used + frames - SP_RING_FRAMES;
        g_head = (LONG)((uint32_t)h + drop);
    }
    uint32_t wpos = (uint32_t)t & SP_RING_MASK;
    uint32_t n1 = frames;
    if (n1 > SP_RING_FRAMES - wpos) n1 = SP_RING_FRAMES - wpos;
    memcpy(g_ring + (size_t)wpos * 2, data, (size_t)n1 * 2 * sizeof(float));
    if (n1 < frames)
        memcpy(g_ring, data + (size_t)n1 * 2,
               (size_t)(frames - n1) * 2 * sizeof(float));
    __atomic_store_n(&g_tail, (LONG)((uint32_t)t + frames), __ATOMIC_RELEASE);
}

static uint32_t sp_ring_read(float* dst, uint32_t frames)
{
    LONG h = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    LONG t = g_tail;
    uint32_t filled = (uint32_t)h - (uint32_t)t;
    if (frames > filled) frames = filled;
    if (frames == 0) return 0;
    uint32_t rpos = (uint32_t)t & SP_RING_MASK;
    uint32_t n1 = frames;
    if (n1 > SP_RING_FRAMES - rpos) n1 = SP_RING_FRAMES - rpos;
    memcpy(dst, g_ring + (size_t)rpos * 2, (size_t)n1 * 2 * sizeof(float));
    if (n1 < frames)
        memcpy(dst + (size_t)n1 * 2, g_ring,
               (size_t)(frames - n1) * 2 * sizeof(float));
    __atomic_store_n(&g_tail, (LONG)((uint32_t)t + frames), __ATOMIC_RELEASE);
    return frames;
}

uint32_t sp_web_read(float* dst, uint32_t frames)
{
    return sp_ring_read(dst, frames);
}

/* ------------------------------------------------------------------ */
/* Device + volume + état                                              */
/* ------------------------------------------------------------------ */
static ma_device g_device;
static int  g_device_ok = 0;
static volatile LONG g_stop = 0;
static HANDLE g_thread = NULL;
static volatile LONG g_volume_pct = 80;   /* 0..100 (défaut config) */

void sp_set_volume(float v)
{
    LONG p = (LONG)(v * 100.0f);
    if (p < 0) p = 0;
    if (p > 200) p = 200;
    g_volume_pct = p;
    if (g_device_ok)
        ma_device_set_master_volume(&g_device, v);
}

float sp_get_volume(void) { return (float)g_volume_pct / 100.0f; }

/* ------------------------------------------------------------------ */
/* Callback audio : joue le flux reçu                                  */
/* ------------------------------------------------------------------ */
static void data_cb(ma_device* dev, void* out, const void* in, ma_uint32 frames)
{
    (void)dev; (void)in;
    float* dst = (float*)out;
    uint32_t got = sp_ring_read(dst, frames);
    if (got < frames)
        memset(dst + (size_t)got * 2, 0, (size_t)(frames - got) * 2 * sizeof(float));

    /* platine B (mode DJ local) : mixée avec la platine A */
    mp_dj_mix_into(dst, frames);

    /* effets audio des plugins (equalizer, sound quality…) */
    mp_plugins_audio_process(dst, frames, 2, 44100);
    /* flux d'analyse pour les plugins visuels */
    mp_plugins_audio_frames(dst, frames, 2, 44100);
}

/* ------------------------------------------------------------------ */
/* Thread réseau : GET /stream → ring                                  */
/* ------------------------------------------------------------------ */
static DWORD WINAPI stream_thread(LPVOID arg)
{
    (void)arg;
    while (!g_stop) {
        HINTERNET inet = InternetOpenA("MusicPlayer-Client/1.0",
                                       INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!inet) { Sleep(1000); continue; }
        char path[64];
        snprintf(path, sizeof(path), "/stream");
        HINTERNET conn = InternetConnectA(inet, "127.0.0.1", (INTERNET_PORT)CC_PORT,
                                          NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        if (!conn) { InternetCloseHandle(inet); Sleep(1000); continue; }
        HINTERNET h = HttpOpenRequestA(conn, "GET", path, NULL, NULL, NULL,
                                       INTERNET_FLAG_RELOAD |
                                       INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!h) { InternetCloseHandle(conn); InternetCloseHandle(inet); Sleep(1000); continue; }
        /* lecture par petits blocs avec timeout : g_stop est vérifié
         * régulièrement (pas de fermeture de handle depuis l'extérieur,
         * instable sous Wine) */
        DWORD rto = 500;
        InternetSetOption(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &rto, sizeof(rto));
        InternetSetOption(h, INTERNET_OPTION_CONNECT_TIMEOUT, &rto, sizeof(rto));
        if (HttpSendRequestA(h, NULL, 0, NULL, 0)) {
            /* saute l'en-tête WAV (44 octets) */
            unsigned char wav[44];
            DWORD rd = 0;
            DWORD total = 0;
            while (total < 44) {
                if (!InternetReadFile(h, wav + total, 44 - total, &rd) || rd == 0) break;
                total += rd;
            }
            /* boucle de réception : s16 → f32 → ring */
            unsigned char buf[8192];
            float* fbuf = (float*)malloc(4096 * 2 * sizeof(float));
            if (fbuf) {
                while (!g_stop) {
                    if (!InternetReadFile(h, buf, sizeof(buf), &rd) || rd == 0) break;
                    if (rd % 4) rd -= rd % 4;
                    for (DWORD i = 0; i + 3 < rd; i += 4) {
                        short l = (short)((unsigned short)buf[i] | ((unsigned short)buf[i + 1] << 8));
                        short r = (short)((unsigned short)buf[i + 2] | ((unsigned short)buf[i + 3] << 8));
                        fbuf[(i / 4) * 2]     = (float)l / 32768.0f;
                        fbuf[(i / 4) * 2 + 1] = (float)r / 32768.0f;
                    }
                    sp_ring_write(fbuf, rd / 4);
                }
                free(fbuf);
            }
        }
        InternetCloseHandle(h);
        InternetCloseHandle(conn);
        InternetCloseHandle(inet);
        if (!g_stop) Sleep(500);   /* reconnexion */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
int sp_start(void)
{
    g_stop = 0;
    g_ring = (float*)malloc(SP_RING_FRAMES * 2 * sizeof(float));
    if (!g_ring) return -1;
    g_head = g_tail = 0;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 44100;
    cfg.dataCallback = data_cb;

    if (ma_device_init(NULL, &cfg, &g_device) == MA_SUCCESS) {
        if (ma_device_start(&g_device) == MA_SUCCESS) {
            g_device_ok = 1;
            ma_device_set_master_volume(&g_device, sp_get_volume());
        } else {
            ma_device_uninit(&g_device);
        }
    }

    g_thread = CreateThread(NULL, 0, stream_thread, NULL, 0, NULL);
    return 0;
}

void sp_stop(void)
{
    g_stop = 1;
    if (g_thread) {
        /* le thread sort seul : timeout de lecture 500 ms */
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_device_ok) {
        ma_device_uninit(&g_device);
        g_device_ok = 0;
    }
    free(g_ring);
    g_ring = NULL;
}
