/* src/core/core_http.c — API publique du moteur (client/serveur).
 *
 * Le core (musicplayer-core.exe) expose son état et ses commandes via
 * cette API REST standard, documentée dans API.md :
 *
 *   GET  /api/state   état complet (JSON)
 *   GET  /api/plist   playlist (JSON)
 *   GET  /api/cover   jaquette du morceau courant
 *   GET  /stream      flux audio PCM WAV 44,1 kHz stéréo 16 bits
 *   GET  /api/levels  niveaux audio {l, r} (visuels du client)
 *   GET  /health      "ok"
 *   POST /api/cmd     commande JSON (anti-CSRF : Content-Type json)
 *
 * Port : config.yml svc_rest_port (défaut 8080). Écoute sur 127.0.0.1
 * et sur les IPs cochées dans Settings ▸ Network… (svc_rest_ips). */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../plugin_loader.h"
#include "../player.h"
#include "../config.h"
#include "core_playlist.h"
#include "../../examples/http_util.h"

static volatile LONG g_running = 1;
static volatile float g_lvl_l = 0.0f, g_lvl_r = 0.0f;
static SOCKET g_socks[8];
static int    g_nsocks = 0;
extern HWND g_core_hwnd;
const unsigned char* core_get_cover(const char* path, size_t* len);

void core_http_stop(void)
{
    InterlockedExchange(&g_running, 0);
    for (int i = 0; i < g_nsocks; i++) {
        if (g_socks[i] != INVALID_SOCKET) closesocket(g_socks[i]);
        g_socks[i] = INVALID_SOCKET;
    }
}

/* ------------------------------------------------------------------ */
/* Échappement JSON (chaînes UTF-8)                                    */
/* ------------------------------------------------------------------ */
static void json_escape(const char* in, char* out, int out_chars)
{
    int o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o < out_chars - 8; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += snprintf(out + o, (size_t)(out_chars - o), "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* État du lecteur (JSON)                                              */
/* ------------------------------------------------------------------ */
static void build_state(char* out, int cap)
{
    const char* fn = mp_get_file_name();
    const char* base = fn ? strrchr(fn, '\\') : NULL;
    base = base ? base + 1 : fn;
    const char* title = fn ? mp_plugins_get_metadata(fn, "title") : NULL;
    const char* artist = fn ? mp_plugins_get_metadata(fn, "artist") : NULL;
    const char* album = fn ? mp_plugins_get_metadata(fn, "album") : NULL;
    const char* year = fn ? mp_plugins_get_metadata(fn, "year") : NULL;
    char ename[512], etitle[512], eartist[512], ealbum[512], eyear[64];
    json_escape(base ? base : "", ename, sizeof(ename));
    json_escape(title ? title : "", etitle, sizeof(etitle));
    json_escape(artist ? artist : "", eartist, sizeof(eartist));
    json_escape(album ? album : "", ealbum, sizeof(ealbum));
    json_escape(year ? year : "", eyear, sizeof(eyear));

    core_plist_lock();
    int idx = g_plist_idx, count = g_plist_n;
    int shuffle = core_plist_get_shuffle();
    core_plist_unlock();

    snprintf(out, (size_t)cap,
        "{\"state\":%d,\"pos\":%.3f,\"dur\":%.3f,\"idx\":%d,\"count\":%d,"
        "\"speed\":%.2f,\"shuffle\":%d,\"name\":\"%s\",\"title\":\"%s\","
        "\"artist\":\"%s\",\"album\":\"%s\",\"year\":\"%s\",\"items\":%d}",
        (int)mp_get_state(), mp_get_position(), mp_get_duration(),
        idx, count, mp_get_speed(), shuffle,
        ename, etitle, eartist, ealbum, eyear, count);
}

/* ------------------------------------------------------------------ */
/* POST /api/cmd — commandes                                           */
/* ------------------------------------------------------------------ */
static const char* json_str(const char* body, const char* key,
                            char* out, int outsz)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = strstr(body, pat);
    if (!p) return NULL;
    p += strlen(pat);
    const char* e = strchr(p, '"');
    if (!e) return NULL;
    int n = (int)(e - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, (size_t)n);
    out[n] = 0;
    return out;
}

static double json_num(const char* body, const char* key, double def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = strstr(body, pat);
    if (!p) return def;
    return atof(p + strlen(pat));
}

static void handle_cmd(SOCKET c, const char* body)
{
    char cmd[64] = "";
    json_str(body, "cmd", cmd, sizeof(cmd));

    if (!strcmp(cmd, "play")) {
        if (mp_get_state() != MP_STATE_PLAYING) mp_play_pause();
    } else if (!strcmp(cmd, "pause")) {
        if (mp_get_state() == MP_STATE_PLAYING) mp_play_pause();
    } else if (!strcmp(cmd, "playpause")) {
        mp_play_pause();
    } else if (!strcmp(cmd, "stop")) {
        mp_stop();
    } else if (!strcmp(cmd, "next")) {
        /* hors verrou : core_plist_next appelle mp_open */
        core_plist_next();
    } else if (!strcmp(cmd, "prev")) {
        /* hors verrou : core_plist_prev appelle mp_open */
        core_plist_prev();
    } else if (!strcmp(cmd, "shuffle")) {
        core_plist_lock();
        core_plist_set_shuffle(!core_plist_get_shuffle());
        core_plist_unlock();
    } else if (!strcmp(cmd, "seek")) {
        mp_seek(json_num(body, "value", 0.0));
    } else if (!strcmp(cmd, "speed")) {
        float s = (float)json_num(body, "value", 1.0);
        if (s >= 0.5f && s <= 2.0f) mp_set_speed(s);
    } else if (!strcmp(cmd, "volume")) {
        float v = (float)json_num(body, "value", 1.0);
        if (v >= 0.0f && v <= 1.0f) mp_set_volume(v);
    } else if (!strcmp(cmd, "playidx")) {
        /* PAS de verrou pendant mp_open : le décodeur attend ce verrou
         * à la fin d'un morceau (enchaînement) → deadlock sinon */
        core_plist_lock();
        int pi = (int)json_num(body, "value", 0.0);
        core_plist_unlock();
        core_plist_play_index(pi);
    } else if (!strcmp(cmd, "open")) {
        char path[MAX_PATH * 2] = "";
        json_str(body, "path", path, sizeof(path));
        if (path[0]) {
            wchar_t wp[MAX_PATH * 2];
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, MAX_PATH * 2);
            DWORD attr = GetFileAttributesW(wp);
            if (attr != INVALID_FILE_ATTRIBUTES &&
                (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                /* scan sous verrou, lecture (mp_open) après libération */
                core_plist_lock();
                core_plist_open_folder(wp);
                core_plist_unlock();
            } else {
                mp_open(path);
            }
        }
    } else if (!strcmp(cmd, "shutdown")) {
        /* le client ferme le moteur */
        core_http_stop();
        PostMessageW(g_core_hwnd, WM_APP + 1, 0, 0);
    }

    http_response(c, 200, "application/json", "{\"ok\":1}");
}

/* ------------------------------------------------------------------ */
/* GET /stream — flux PCM (WAV 44,1 kHz stéréo 16 bits)                */
/* ------------------------------------------------------------------ */
static void stream_loop(SOCKET c)
{
    int rid = mp_web_reader_open();
    if (rid < 0) return;
    /* petit tampon d'émission : borne l'audio en transit à ~46 ms, pour
     * que stop/seek prennent effet quasi immédiatement chez le client */
    int sndbuf = 8192;
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
    unsigned char wav[44] = {
        'R','I','F','F', 0xff,0xff,0xff,0x7f, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x44,0xac,0,0, 0x10,0xb1,0x02,0, 4,0, 16,0,
        'd','a','t','a', 0xff,0xff,0xff,0x7f
    };
    http_send_all(c, (const char*)wav, 44);

    float* fbuf = (float*)malloc(2048 * 2 * sizeof(float));
    unsigned char* obuf = (unsigned char*)malloc(16384);
    if (!fbuf || !obuf) {
        free(fbuf); free(obuf);
        mp_web_reader_close(rid);
        return;
    }
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        uint32_t n = mp_web_read_n(rid, fbuf, 2048);
        if (!n) { Sleep(5); continue; }
        /* niveaux RMS par canal (pour les visuels du client) */
        float sl = 0, sr = 0;
        for (uint32_t i = 0; i < n; i++) {
            float l = fbuf[i * 2], r = fbuf[i * 2 + 1];
            sl += l * l; sr += r * r;
        }
        InterlockedExchange((volatile LONG*)&g_lvl_l, (LONG)((sl / (float)n) * 100000.0f));
        InterlockedExchange((volatile LONG*)&g_lvl_r, (LONG)((sr / (float)n) * 100000.0f));
        http_f32_to_s16(fbuf, obuf, n, 1.0f);
        if (send(c, (const char*)obuf, (int)(n * 4), 0) <= 0) break;
    }
    free(fbuf);
    free(obuf);
    mp_web_reader_close(rid);
}

/* ------------------------------------------------------------------ */
/* Dispatch HTTP par connexion                                         */
/* ------------------------------------------------------------------ */
static DWORD WINAPI client_thread(LPVOID arg)
{
    SOCKET c = (SOCKET)(INT_PTR)arg;
    char req[16384];
    int rn = http_read_request(c, req, sizeof(req));
    if (rn <= 0) { closesocket(c); return 0; }

    char method[16] = "", path[512] = "";
    sscanf(req, "%15s %511s", method, path);

    if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
        http_response(c, 200, "text/plain", "ok");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/state")) {
        char js[2048];
        build_state(js, sizeof(js));
        http_response(c, 200, "application/json", js);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/plist")) {
        char* js = (char*)malloc(PLAYLIST_MAX * (MAX_PATH * 2 + 32));
        if (js) {
            char* p = js;
            int left = PLAYLIST_MAX * (MAX_PATH * 2 + 32);
            int n = snprintf(p, (size_t)left, "{\"items\":[");
            p += n; left -= n;
            core_plist_lock();
            for (int i = 0; i < g_plist_n && left > 64; i++) {
                char esc[MAX_PATH * 2];
                char utf8[MAX_PATH * 3];
                WideCharToMultiByte(CP_UTF8, 0, g_plist[i], -1,
                                    utf8, sizeof(utf8), NULL, NULL);
                json_escape(utf8, esc, sizeof(esc));
                n = snprintf(p, (size_t)left, "%s\"%s\"",
                             i ? "," : "", esc);
                p += n; left -= n;
            }
            core_plist_unlock();
            snprintf(p, (size_t)left, "]}");
            http_response(c, 200, "application/json", js);
            free(js);
        }
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/cover")) {
        const char* fn = mp_get_file_name();
        if (fn) {
            size_t len = 0;
            const unsigned char* img = core_get_cover(fn, &len);
            if (img && len > 0) {
                http_response_len(c, 200, "image/jpeg",
                                  (const char*)img, (int)len);
                closesocket(c);
                return 0;
            }
        }
        http_response(c, 404, "text/plain", "no cover");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/levels")) {
        char js[64];
        snprintf(js, sizeof(js), "{\"l\":%.4f,\"r\":%.4f}",
                 (float)g_lvl_l / 100000.0f, (float)g_lvl_r / 100000.0f);
        http_response(c, 200, "application/json", js);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/stream")) {
        stream_loop(c);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/config")) {
        /* corps : "web_enabled=1&web_port=8000&web_ips=..." */
        const char* body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : "";
        const char* p;
        if ((p = strstr(body, "web_enabled=")) != NULL)
            g_cfg.web_enabled = atoi(p + 12);
        if ((p = strstr(body, "web_port=")) != NULL)
            g_cfg.web_port = atoi(p + 9);
        if ((p = strstr(body, "web_ips=")) != NULL) {
            p += 8;
            const char* e = strchr(p, '&');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n >= sizeof(g_cfg.web_ips)) n = sizeof(g_cfg.web_ips) - 1;
            memcpy(g_cfg.web_ips, p, n);
            g_cfg.web_ips[n] = 0;
        }
        /* applique à chaud */
        mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL);
        http_response(c, 200, "application/json", "{\"status\":\"ok\"}");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/cmd")) {
        if (!http_post_is_json(req)) {
            http_response(c, 403, "text/plain", "forbidden");
        } else {
            /* corps : après la fin des en-têtes */
            const char* hdr = strstr(req, "\r\n\r\n");
            const char* body = hdr ? hdr + 4 : "";
            handle_cmd(c, body);
        }
    } else {
        http_response(c, 404, "text/plain", "not found");
    }
    closesocket(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Démarrage : socket(s) d'écoute                                      */
/* ------------------------------------------------------------------ */
static DWORD WINAPI accept_loop(LPVOID arg)
{
    SOCKET s = (SOCKET)(INT_PTR)arg;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        SOCKET c = accept(s, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        HANDLE h = CreateThread(NULL, 0, client_thread, (LPVOID)(INT_PTR)c, 0, NULL);
        if (h) CloseHandle(h);
        else closesocket(c);
    }
    return 0;
}

static void add_listener(SOCKET s)
{
    if (g_nsocks < 8) {
        g_socks[g_nsocks++] = s;
        HANDLE h = CreateThread(NULL, 0, accept_loop, (LPVOID)(INT_PTR)s, 0, NULL);
        if (h) CloseHandle(h);
    } else {
        closesocket(s);
    }
}

/* Bind une socket sur l'adresse IP donnée (chaîne "a.b.c.d" ou NULL =
 * toutes les interfaces). Retourne INVALID_SOCKET en cas d'échec. */
static SOCKET make_socket(const char* ip, int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (ip && ip[0])
        sa.sin_addr.s_addr = inet_addr(ip);
    else
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (listen(s, 8) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

void core_http_start(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    int port = g_cfg.svc_rest_port > 0 ? g_cfg.svc_rest_port : 8080;
    for (int i = 0; i < 8; i++) g_socks[i] = INVALID_SOCKET;
    g_nsocks = 0;

    /* toujours localhost, plus les IPs cochées (svc_rest_ips) */
    SOCKET s = make_socket("127.0.0.1", port);
    if (s != INVALID_SOCKET) add_listener(s);

    if (g_cfg.svc_rest_ips[0]) {
        char ips[1024];
        strncpy(ips, g_cfg.svc_rest_ips, sizeof(ips) - 1);
        ips[sizeof(ips) - 1] = 0;
        char* tok = strtok(ips, ";");
        while (tok) {
            s = make_socket(tok, port);
            if (s != INVALID_SOCKET) add_listener(s);
            tok = strtok(NULL, ";");
        }
    }
}
