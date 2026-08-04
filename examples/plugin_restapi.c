/*
 * REST API — plugin SERVICE
 * =========================
 * Serveur HTTP JSON sur le port 8080 (activable dans Plugins ▸ Services) :
 *
 *   GET  /api/state    → état complet (JSON : state, pos, dur, vol, speed,
 *                         audio, shuffle, dj, idx, count, name, title,
 *                         artist, album, year, items)
 *   GET  /api/playlist → items de la playliste (JSON)
 *   GET  /api/cover    → jaquette du morceau courant (JPEG/PNG)
 *   POST /api/cmd      → commande (body : play|pause|stop|next|prev|volup|
 *                         voldown|vol=0.5|speed=1.2|speedup|speeddown|
 *                         audio|shuffle|dj|playidx=N)
 *   GET  /api/stream   → flux audio (WAV PCM 44,1 kHz stéréo)
 *   GET  /health       → "ok"
 *
 * Sécurité CORS (assumée) :
 *   - LECTURE ouverte : les GET répondent Access-Control-Allow-Origin: *
 *     (des clients web externes peuvent afficher l'état du lecteur).
 *   - COMMANDES protégées : POST /api/cmd exige Content-Type:
 *     application/json (en-tête non-simple). Un site tiers déclencherait
 *     un preflight OPTIONS qui échoue (le serveur ne renvoie pas
 *     Access-Control-Allow-Headers) : les commandes cross-origin sont
 *     donc bloquées.
 */
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/plugin.h"
#include "http_util.h"

#define REST_PORT 8080

static const mp_host_api* g_h = NULL;
static volatile LONG g_running = 0;
static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_thread = NULL;

/* ------------------------------------------------------------------ */
static void log_line(const char* msg)
{
    if (g_h && g_h->log) g_h->log(msg);
}

static void json_escape(const wchar_t* in, char* out, int max)
{
    int o = 0;
    for (int i = 0; in[i] && o < max - 4; i++) {
        if (in[i] < 128 && in[i] != '"' && in[i] != '\\') {
            out[o++] = (char)in[i];
        } else {
            o += snprintf(out + o, max - o, "\\u%04x", (unsigned)in[i]);
        }
    }
    out[o] = 0;
}

static void json_escape_a(const char* in, char* out, int max)
{
    int o = 0;
    for (int i = 0; in[i] && o < max - 4; i++) {
        if ((unsigned char)in[i] >= 32 && in[i] != '"' && in[i] != '\\') {
            out[o++] = in[i];
        } else {
            o += snprintf(out + o, max - o, "\\u%04x", (unsigned char)in[i]);
        }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
static void http_headers(SOCKET c, int code, const char* ctype, int len)
{
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, code == 200 ? "OK" : "Not Found",
        ctype ? ctype : "application/json", len);
    http_send_all(c, hdr, (int)strlen(hdr));
}

static void send_body(SOCKET c, const char* body, const char* ctype)
{
    http_headers(c, 200, ctype, (int)strlen(body));
    send(c, body, (int)strlen(body), 0);
}

static void send_404(SOCKET c)
{
    const char* b = "{\"error\":\"not found\"}";
    http_headers(c, 404, "application/json", (int)strlen(b));
    send(c, b, (int)strlen(b), 0);
}

/* ------------------------------------------------------------------ */
static void api_state(SOCKET c)
{
    char body[32768];
    const char* name = g_h->get_file_name();
    const char* title = name ? g_h->get_metadata(name, "title") : NULL;
    const char* artist = name ? g_h->get_metadata(name, "artist") : NULL;
    const char* album = name ? g_h->get_metadata(name, "album") : NULL;
    const char* year = name ? g_h->get_metadata(name, "year") : NULL;
    int idx = g_h->plist_index();
    int count = g_h->plist_count();
    int st = g_h->get_state();
    const char* sts = st == 1 ? "playing" : st == 2 ? "paused" : "stopped";
    char tname[512], ttitle[512], tartist[512], talbum[512], tyear[128];
    json_escape_a(name ? name : "", tname, sizeof(tname));
    json_escape_a(title ? title : "", ttitle, sizeof(ttitle));
    json_escape_a(artist ? artist : "", tartist, sizeof(tartist));
    json_escape_a(album ? album : "", talbum, sizeof(talbum));
    json_escape_a(year ? year : "", tyear, sizeof(tyear));
    char items[16384];
    int off = 0;
    for (int i = 0; i < count && off < (int)sizeof(items) - 128; i++) {
        const wchar_t* p = g_h->plist_path(i);
        const wchar_t* base = p;
        const wchar_t* bs = wcsrchr(p, L'\\');
        const wchar_t* fs = wcsrchr(p, L'/');
        if (bs && (!fs || bs > fs)) base = bs + 1;
        else if (fs) base = fs + 1;
        char t[256];
        json_escape(base, t, sizeof(t));
        off += snprintf(items + off, sizeof(items) - off, "%s\"%s\"",
                         i ? "," : "", t);
    }
    items[off] = 0;
    snprintf(body, sizeof(body),
        "{\"state\":\"%s\",\"pos\":%.1f,\"dur\":%.1f,\"vol\":%.2f,"
        "\"speed\":%.2f,\"audio\":%d,\"shuffle\":%d,\"dj\":%d,"
        "\"idx\":%d,\"count\":%d,\"name\":\"%s\",\"title\":\"%s\","
        "\"artist\":\"%s\",\"album\":\"%s\",\"year\":\"%s\","
        "\"items\":[%s]}",
        sts, g_h->get_position(), g_h->get_duration(), g_h->get_volume(),
        g_h->get_speed(), g_h->get_audio_out(), g_h->get_shuffle(),
        g_h->get_dj_mode(), idx, count, tname, ttitle, tartist, talbum,
        tyear, items);
    send_body(c, body, "application/json");
}

static void api_playlist(SOCKET c)
{
    char body[16384];
    int count = g_h->plist_count();
    int off = 0;
    for (int i = 0; i < count && off < (int)sizeof(body) - 128; i++) {
        const wchar_t* p = g_h->plist_path(i);
        const wchar_t* base = p;
        const wchar_t* bs = wcsrchr(p, L'\\');
        const wchar_t* fs = wcsrchr(p, L'/');
        if (bs && (!fs || bs > fs)) base = bs + 1;
        else if (fs) base = fs + 1;
        char t[256];
        json_escape(base, t, sizeof(t));
        off += snprintf(body + off, sizeof(body) - off, "%s\"%s\"",
                         i ? "," : "", t);
    }
    body[off] = 0;
    char out[16400];
    snprintf(out, sizeof(out), "{\"count\":%d,\"items\":[%s]}", count, body);
    send_body(c, out, "application/json");
}

static void api_cover(SOCKET c)
{
    const char* name = g_h->get_file_name();
    if (!name) { send_404(c); return; }
    size_t len = 0;
    const unsigned char* img = g_h->get_cover(name, &len);
    if (!img || !len) { send_404(c); return; }
    http_headers(c, 200, "image/jpeg", (int)len);
    int sent = 0;
    while (sent < (int)len) {
        int n = send(c, img + sent, (int)len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

static void api_cmd(SOCKET c, const char* cmd)
{
    char resp[128] = "{\"ok\":true}";
    if (cmd) {
        if (!strcmp(cmd, "play"))            g_h->play_pause();
        else if (!strcmp(cmd, "pause"))      g_h->play_pause();
        else if (!strcmp(cmd, "stop"))       g_h->stop();
        else if (!strcmp(cmd, "next"))       g_h->next();
        else if (!strncmp(cmd, "volup", 5))
            g_h->set_volume(g_h->get_volume() + 0.05f);
        else if (!strncmp(cmd, "voldown", 7))
            g_h->set_volume(g_h->get_volume() - 0.05f);
        else if (!strncmp(cmd, "speedup", 7))
            g_h->set_speed(g_h->get_speed() + 0.25f);
        else if (!strncmp(cmd, "speeddown", 9))
            g_h->set_speed(g_h->get_speed() - 0.25f);
        else if (!strncmp(cmd, "vol=", 4))
            g_h->set_volume((float)atof(cmd + 4));
        else if (!strncmp(cmd, "speed=", 6))
            g_h->set_speed((float)atof(cmd + 6));
        else if (!strncmp(cmd, "audio", 5))
            g_h->set_audio_out((g_h->get_audio_out() + 1) % 3);
        else if (!strcmp(cmd, "shuffle"))    g_h->shuffle_toggle();
        else if (!strcmp(cmd, "dj"))         g_h->dj_toggle();
        else if (!strncmp(cmd, "playidx=", 8)) {
            int i = atoi(cmd + 8);
            if (i >= 0 && i < g_h->plist_count()) g_h->plist_play(i);
        } else {
            snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"unknown command\"}");
        }
    }
    send_body(c, resp, "application/json");
}

static void api_stream(SOCKET c)
{
    /* en-tête WAV 44,1 kHz stéréo 16 bits (http_util.h) */
    unsigned char hdr[44];
    http_wav_header44k(hdr);
    http_headers(c, 200, "audio/wav", -1);
    char hlen[64];
    snprintf(hlen, sizeof(hlen), "%d", 44);
    /* on renvoie le vrai header WAV puis le flux brut */
    (void)hlen;
    /* Content-Length inconnu : re-send avec un header sans longueur */
    /* (http_headers a mis -1 → on force chunked n'est pas supporté :
     * on renvoie le header en 2 parties) */
    /* tampons propres à CE thread (un thread par connexion) */
    float* fbuf = (float*)malloc(1024 * 2 * sizeof(float));
    unsigned char* obuf = (unsigned char*)malloc(4096);
    if (!fbuf || !obuf) {
        free(fbuf);
        free(obuf);
        closesocket(c);
        return;
    }
    send(c, (const char*)hdr, 44, 0);
    while (g_running) {
        uint32_t n = g_h->web_read(fbuf, 1024);
        if (!n) { Sleep(20); continue; }
        http_f32_to_s16(fbuf, obuf, n, 1.0f);
        if (send(c, (const char*)obuf, (int)(n * 4), 0) <= 0) break;
    }
    free(fbuf);
    free(obuf);
}

/* ------------------------------------------------------------------ */
static void handle_client(SOCKET c)
{
    char req[8192];
    int got = http_read_request(c, req, sizeof(req));
    if (got < 8) { closesocket(c); return; }
    char method[16] = "", path[1024] = "";
    sscanf(req, "%15s %1023s", method, path);
    if (!strcmp(method, "OPTIONS")) {
        http_headers(c, 200, "text/plain", 0);
    } else if (!strcmp(method, "POST")) {
        /* CSRF : un POST « simple » (sans Content-Type JSON) peut être
         * envoyé par n'importe quel site sans CORS → refusé */
        if (!http_post_is_json(req)) {
            send_body(c, "{\"error\":\"forbidden\"}", "application/json");
        } else if (!strcmp(path, "/api/cmd")) {
            const char* body = strstr(req, "\r\n\r\n");
            if (!body) body = strstr(req, "\n\n");
            api_cmd(c, body ? body + (body[0] == '\r' ? 4 : 2) : "");
        } else {
            send_404(c);
        }
    } else if (!strcmp(path, "/api/state")) {
        api_state(c);
    } else if (!strcmp(path, "/api/playlist")) {
        api_playlist(c);
    } else if (!strcmp(path, "/api/cover")) {
        api_cover(c);
    } else if (!strcmp(path, "/api/cmd")) {
        const char* body = strstr(req, "\r\n\r\n");
        if (!body) body = strstr(req, "\n\n");
        api_cmd(c, body ? body + (body[0] == '\r' ? 4 : 2) : "");
    } else if (!strcmp(path, "/api/stream")) {
        api_stream(c);
    } else if (!strcmp(path, "/health")) {
        send_body(c, "{\"status\":\"ok\"}", "application/json");
    } else {
        send_404(c);
    }
    closesocket(c);
}

static DWORD WINAPI client_thread(LPVOID arg)
{
    SOCKET c = (SOCKET)(SIZE_T)arg;
    handle_client(c);
    return 0;
}

static DWORD WINAPI accept_loop(LPVOID arg)
{
    (void)arg;
    while (g_running) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        HANDLE t = CreateThread(NULL, 0, client_thread, (LPVOID)(SIZE_T)c, 0, NULL);
        if (t) CloseHandle(t);
        else closesocket(c);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
static int server_start(void)
{
    if (g_running) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { WSACleanup(); return -1; }
    int yes = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    /* IP configurée (Settings ▸ Network…) : sinon toutes les interfaces */
    const char* ips = g_h->svc_ips ? g_h->svc_ips("rest") : "";
    if (ips && ips[0]) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s", ips);
        char* tok = strtok(tmp, ";");
        addr.sin_addr.s_addr = inet_addr(tok ? tok : "0.0.0.0");
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    int port = g_h->svc_port ? g_h->svc_port("rest") : REST_PORT;
    addr.sin_port = htons((unsigned short)port);
    if (bind(g_listen, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }
    listen(g_listen, 8);
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, accept_loop, NULL, 0, NULL);
    return 0;
}

static void server_stop(void)
{
    if (!g_running) return;
    InterlockedExchange(&g_running, 0);
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    WSACleanup();
}

/* ------------------------------------------------------------------ */
static const char* pl_name(void) { return "REST API"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "HTTP JSON API on port 8080 (state, playlist, cover, commands, stream)"; }
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
    server_stop();
    g_h = NULL;
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)data;
    if (!g_h) return;
    if (event != MP_SERVICE_WEB_APPLY && event != MP_SERVICE_CLICK) return;
    if (self->enabled) {
        if (!g_running) {
            if (server_start() == 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "REST API listening on port %d",
                          g_h->svc_port ? g_h->svc_port("rest") : REST_PORT);
                log_line(msg);
            } else {
                log_line("REST API: port unavailable");
            }
        }
    } else {
        server_stop();
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
