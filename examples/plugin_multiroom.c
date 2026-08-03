/*
 * Multiroom — plugin SERVICE
 * ==========================
 * Diffuse le flux audio courant en RTP (L16 44,1 kHz stéréo) vers
 * plusieurs destinataires simultanément :
 *   - le groupe multicast 239.255.0.2:5004 par défaut (toutes les pièces
 *     qui écoutent ce groupe reçoivent la même musique, sans réglage)
 *   - + les cibles du fichier multiroom.txt placé à côté de la DLL
 *     (une adresse par ligne, format "ip:port", ex. 192.168.10.50:5004)
 *
 * Réception : VLC "udp://@239.255.0.2:5004" sur chaque appareil.
 * Activable dans Plugins ▸ Services ▸ Multiroom.
 */
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/plugin.h"

#define MR_GROUP  "239.255.0.2"
#define MR_PORT   5004
#define PKT_FRAMES 1152

static const mp_host_api* g_h = NULL;
static volatile LONG g_running = 0;
static HANDLE g_thread = NULL;
static wchar_t g_cfg_path[MAX_PATH];

static void log_line(const char* msg)
{
    if (g_h && g_h->log) g_h->log(msg);
}

/* Charge les cibles : le multicast par défaut + le fichier multiroom.txt
 * (ip:port par ligne). Retourne le nombre de cibles. */
static int load_targets(struct sockaddr_in* tgt, int max)
{
    int n = 0;
    memset(&tgt[n], 0, sizeof(tgt[n]));
    tgt[n].sin_family = AF_INET;
    tgt[n].sin_port = htons(MR_PORT);
    tgt[n].sin_addr.s_addr = inet_addr(MR_GROUP);
    n++;
    FILE* f = _wfopen(g_cfg_path, L"r");
    if (f) {
        char line[128];
        while (n < max && fgets(line, sizeof(line), f)) {
            char* p = strchr(line, '\n');
            if (p) *p = 0;
            p = strchr(line, '\r');
            if (p) *p = 0;
            if (!line[0] || line[0] == '#') continue;
            char* colon = strchr(line, ':');
            int port = MR_PORT;
            if (colon) {
                *colon = 0;
                port = atoi(colon + 1);
            }
            unsigned long ip = inet_addr(line);
            if (ip == INADDR_NONE || port <= 0) continue;
            memset(&tgt[n], 0, sizeof(tgt[n]));
            tgt[n].sin_family = AF_INET;
            tgt[n].sin_port = htons((unsigned short)port);
            tgt[n].sin_addr.s_addr = ip;
            n++;
        }
        fclose(f);
    }
    return n;
}

static DWORD WINAPI mr_thread(LPVOID arg)
{
    (void)arg;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    unsigned char ttl = 8, loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));

    struct sockaddr_in tgt[8];
    int ntargets = load_targets(tgt, 8);

    static float fbuf[PKT_FRAMES * 2];
    static unsigned char pkt[12 + PKT_FRAMES * 4];
    unsigned short seq = 0;
    unsigned int ts = 0;

    log_line("Multiroom: streaming to 239.255.0.2:5004 (L16 44.1kHz)");
    while (g_running) {
        uint32_t n = g_h->web_read(fbuf, PKT_FRAMES);
        if (!n) { Sleep(20); continue; }
        unsigned char* p = pkt + 12;
        for (uint32_t i = 0; i < n * 2; i++) {
            float v = fbuf[i];
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            short s16 = (short)(v * 32767.0f);
            *p++ = (unsigned char)((s16 >> 8) & 0xff);
            *p++ = (unsigned char)(s16 & 0xff);
        }
        pkt[0] = 0x80;
        pkt[1] = 10;
        pkt[2] = (unsigned char)(seq >> 8);
        pkt[3] = (unsigned char)(seq & 0xff);
        pkt[4] = (unsigned char)(ts >> 24);
        pkt[5] = (unsigned char)((ts >> 16) & 0xff);
        pkt[6] = (unsigned char)((ts >> 8) & 0xff);
        pkt[7] = (unsigned char)(ts & 0xff);
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;
        for (int t = 0; t < ntargets; t++)
            sendto(s, (const char*)pkt, 12 + (int)(n * 4), 0,
                   (struct sockaddr*)&tgt[t], sizeof(tgt[t]));
        seq++;
        ts += n;
    }
    closesocket(s);
    return 0;
}

/* ------------------------------------------------------------------ */
static int mr_start(void)
{
    if (g_running) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, mr_thread, NULL, 0, NULL);
    return 0;
}

static void mr_stop(void)
{
    if (!g_running) return;
    InterlockedExchange(&g_running, 0);
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    WSACleanup();
}

/* ------------------------------------------------------------------ */
static const char* pl_name(void) { return "Multiroom"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Streams the audio to several rooms at once (RTP multicast 239.255.0.2:5004 + multiroom.txt targets)"; }
static unsigned pl_type(void) { return MP_PLUGIN_SERVICE; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    g_h = host;
    /* chemin du fichier multiroom.txt à côté de la DLL */
    wcsncpy(g_cfg_path, self->path, MAX_PATH - 1);
    g_cfg_path[MAX_PATH - 1] = 0;
    wchar_t* slash = wcsrchr(g_cfg_path, L'\\');
    if (slash) slash[1] = 0;
    wcscat(g_cfg_path, L"multiroom.txt");
    return 0;
}

static void pl_destroy(mp_plugin* self)
{
    (void)self;
    mr_stop();
    g_h = NULL;
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)data;
    if (!g_h) return;
    if (event != MP_SERVICE_WEB_APPLY && event != MP_SERVICE_CLICK) return;
    if (self->enabled) {
        if (!g_running && mr_start() == 0)
            log_line("Multiroom: started");
    } else {
        mr_stop();
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
