/*
 * RTP/AES67 — plugin SERVICE
 * ==========================
 * Diffuse le flux audio courant en RTP (RFC 3550) :
 *   - payload L16 (PCM 16 bits big-endian), 44 100 Hz, 2 canaux
 *     (payload type 10 — profil AES67 de base)
 *   - vers le groupe multicast 239.255.0.1:5004
 *   - annonce SAP (RFC 2974) du flux sur 224.2.127.254:9875
 *     (les récepteurs AES67/RTSP découvrent la source)
 *
 * Réception : VLC "udp://@239.255.0.1:5004" ou tout appareil AES67.
 * Activable dans Plugins ▸ Services ▸ RTP/AES67.
 */
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/plugin.h"

#define RTP_GROUP   "239.255.0.1"
#define RTP_PORT    5004
#define SAP_GROUP   "224.2.127.254"
#define SAP_PORT    9875
#define PKT_FRAMES  1152      /* 26 ms @ 44,1 kHz (paquet AES67 typique) */

static const mp_host_api* g_h = NULL;
static volatile LONG g_running = 0;
static HANDLE g_rtp_thread = NULL;
static HANDLE g_sap_thread = NULL;

static void log_line(const char* msg)
{
    if (g_h && g_h->log) g_h->log(msg);
}

/* ------------------------------------------------------------------ */
static DWORD WINAPI rtp_thread(LPVOID arg)
{
    (void)arg;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    unsigned char ttl = 8, loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
    /* interface multicast : la première IP configurée (Network…) */
    if (g_h && g_h->svc_ips) {
        const char* ips = g_h->svc_ips("rtp");
        if (ips && ips[0]) {
            char tmp[128];
            _snprintf(tmp, sizeof(tmp), "%s", ips);
            char* tok = strtok(tmp, ";");
            if (tok) {
                struct in_addr ifa;
                ifa.s_addr = inet_addr(tok);
                if (ifa.s_addr != INADDR_NONE)
                    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
                               (const char*)&ifa, sizeof(ifa));
            }
        }
    }
    int port = g_h && g_h->svc_port ? g_h->svc_port("rtp") : RTP_PORT;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((unsigned short)port);
    dst.sin_addr.s_addr = inet_addr(RTP_GROUP);

    static float fbuf[PKT_FRAMES * 2];
    static unsigned char pkt[12 + PKT_FRAMES * 4];
    unsigned short seq = 0;
    unsigned int ts = 0;

    /* lecteur dédié : chaque diffuseur a son propre curseur, sinon ils
     * se volent les échantillons (un seul flux partagé) */
    int rid = -1;
    if (g_h->web_reader_open) rid = g_h->web_reader_open();

    {
        char msg[160];
        _snprintf(msg, sizeof(msg),
                  "RTP: multicast streaming to %s:%d (L16 44.1kHz)",
                  RTP_GROUP, port);
        log_line(msg);
    }
    while (g_running) {
        uint32_t n = (rid >= 0 && g_h->web_read_n)
                   ? g_h->web_read_n(rid, fbuf, PKT_FRAMES)
                   : g_h->web_read(fbuf, PKT_FRAMES);
        if (!n) { Sleep(20); continue; }
        /* PCM 16 bits big-endian (L16) */
        unsigned char* p = pkt + 12;
        for (uint32_t i = 0; i < n * 2; i++) {
            float v = fbuf[i];
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            short s16 = (short)(v * 32767.0f);
            *p++ = (unsigned char)((s16 >> 8) & 0xff);
            *p++ = (unsigned char)(s16 & 0xff);
        }
        /* en-tête RTP v2, PT=10 (L16) */
        pkt[0] = 0x80;
        pkt[1] = 10;
        pkt[2] = (unsigned char)(seq >> 8);
        pkt[3] = (unsigned char)(seq & 0xff);
        pkt[4] = (unsigned char)(ts >> 24);
        pkt[5] = (unsigned char)((ts >> 16) & 0xff);
        pkt[6] = (unsigned char)((ts >> 8) & 0xff);
        pkt[7] = (unsigned char)(ts & 0xff);
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;   /* SSRC */
        sendto(s, (const char*)pkt, 12 + (int)(n * 4), 0,
               (struct sockaddr*)&dst, sizeof(dst));
        seq++;
        ts += n;
    }
    if (rid >= 0 && g_h->web_reader_close) g_h->web_reader_close(rid);
    closesocket(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Annonce SAP du flux (SDP) toutes les 30 secondes                     */
/* ------------------------------------------------------------------ */
static void sap_announce(SOCKET s)
{
    const char* sdp =
        "v=0\r\n"
        "o=MusicPlayer 0 0 IN IP4 " RTP_GROUP "\r\n"
        "s=MusicPlayer Audio\r\n"
        "c=IN IP4 " RTP_GROUP "/1\r\n"
        "t=0 0\r\n"
        "m=audio " /* 5004 */ " RTP/AVP 10\r\n"
        "a=rtpmap:10 L16/44100/2\r\n";
    char msg[512];
    /* en-tête SAP : v=1, type annonce */
    msg[0] = 0x20;
    msg[1] = 0;
    msg[2] = 0;
    msg[3] = 0;
    msg[4] = 0;
    msg[5] = 0;
    msg[6] = 0;
    msg[7] = 0;
    int o = 8;
    const char* ct = "application/sdp";
    int l = (int)strlen(ct) + 1;
    memcpy(msg + o, ct, l);
    o += l;
    o += _snprintf(msg + o, sizeof(msg) - o, "m=audio %d RTP/AVP 10\r\n"
                   "a=rtpmap:10 L16/44100/2\r\n", RTP_PORT);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SAP_PORT);
    dst.sin_addr.s_addr = inet_addr(SAP_GROUP);
    sendto(s, msg, o, 0, (struct sockaddr*)&dst, sizeof(dst));
    (void)sdp;
}

static DWORD WINAPI sap_thread(LPVOID arg)
{
    (void)arg;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    unsigned char ttl = 8;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    while (g_running) {
        sap_announce(s);
        for (int i = 0; i < 30 && g_running; i++) Sleep(1000);
    }
    closesocket(s);
    return 0;
}

/* ------------------------------------------------------------------ */
static int rtp_start(void)
{
    if (g_running) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    InterlockedExchange(&g_running, 1);
    g_rtp_thread = CreateThread(NULL, 0, rtp_thread, NULL, 0, NULL);
    g_sap_thread = CreateThread(NULL, 0, sap_thread, NULL, 0, NULL);
    return 0;
}

static void rtp_stop(void)
{
    if (!g_running) return;
    InterlockedExchange(&g_running, 0);
    if (g_rtp_thread) {
        WaitForSingleObject(g_rtp_thread, 2000);
        CloseHandle(g_rtp_thread);
        g_rtp_thread = NULL;
    }
    if (g_sap_thread) {
        WaitForSingleObject(g_sap_thread, 2000);
        CloseHandle(g_sap_thread);
        g_sap_thread = NULL;
    }
    WSACleanup();
}

/* ------------------------------------------------------------------ */
static const char* pl_name(void) { return "RTP/AES67 Output"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{ return "Streams the audio as RTP L16 multicast (239.255.0.1:5004) with SAP announce (AES67)"; }
static unsigned pl_type(void) { return MP_PLUGIN_SERVICE; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_h = host;
    return 0;
}

static void pl_destroy(mp_plugin* self)
{
    (void)self;
    rtp_stop();
    g_h = NULL;
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)data;
    if (!g_h) return;
    if (event != MP_SERVICE_WEB_APPLY && event != MP_SERVICE_CLICK) return;
    if (self->enabled) {
        if (!g_running && rtp_start() == 0)
            log_line("RTP/AES67: started");
    } else {
        rtp_stop();
    }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    pl_init, pl_destroy,
    NULL, NULL, NULL, NULL,   /* process, audio_frames, render, apply_skin */
    pl_service, NULL          /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
