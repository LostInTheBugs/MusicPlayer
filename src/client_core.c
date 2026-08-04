/* src/client_core.c — pont client → moteur (musicplayer-core.exe).
 *
 * Le client (MusicPlayer.exe) :
 *   - lance le moteur au démarrage (cc_start) s'il ne tourne pas ;
 *   - pilote par POST /api/cmd (JSON, anti-CSRF) ;
 *   - lit l'état par GET /api/state (polling ~4×/s) ;
 *   - rafraîchit sa playlist locale via GET /api/plist.
 *
 * En mode service (phase 3), cc_start se contente de se connecter. */
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client_core.h"
/* playlist locale du client (cache de /api/plist) */
#define PLAYLIST_MAX 512
extern wchar_t* g_plist[PLAYLIST_MAX];
extern int g_plist_n;
extern int g_plist_idx;

static cc_state_t g_cc;
static volatile LONG g_started = 0;
static HANDLE g_core_proc = NULL;   /* processus du moteur (si lancé ici) */

/* ------------------------------------------------------------------ */
/* HTTP minimal (WinINet, tout en char : l'URL est ASCII)              */
/* ------------------------------------------------------------------ */
typedef struct {
    int    code;        /* 200, 403… */
    char*  body;        /* malloc, NUL-terminé */
    size_t len;
} http_resp;

static http_resp cc_http2(const char* method, const char* path,
                          const char* body)
{
    http_resp r = { 0, NULL, 0 };
    HINTERNET inet = InternetOpenA("MusicPlayer-Client/1.0",
                                   INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!inet) return r;
    DWORD to = 3000;
    InternetSetOption(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOption(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

    char host[64];
    snprintf(host, sizeof(host), "127.0.0.1:%d", CC_PORT);
    HINTERNET conn = InternetConnectA(inet, "127.0.0.1", (INTERNET_PORT)CC_PORT,
                                      NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (conn) {
        HINTERNET h = HttpOpenRequestA(conn, method, path, NULL, NULL, NULL,
                                       INTERNET_FLAG_RELOAD |
                                       INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (h) {
            BOOL ok = body
                ? HttpSendRequestA(h, "Content-Type: application/json\r\n",
                                   (DWORD)-1L, (LPVOID)body, (DWORD)strlen(body))
                : HttpSendRequestA(h, NULL, 0, NULL, 0);
            if (ok) {
                DWORD code = 0, cbs = sizeof(code);
                HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                               &code, &cbs, NULL);
                r.code = (int)code;
                char buf[8192];
                DWORD rd = 0;
                size_t cap = 0;
                while (InternetReadFile(h, buf, sizeof(buf), &rd) && rd > 0) {
                    char* nb = (char*)realloc(r.body, cap + rd + 1);
                    if (!nb) break;
                    r.body = nb;
                    memcpy(r.body + cap, buf, rd);
                    cap += rd;
                    r.body[cap] = 0;
                }
                r.len = cap;
            }
            InternetCloseHandle(h);
        }
        InternetCloseHandle(conn);
    }
    InternetCloseHandle(inet);
    return r;
}

/* ------------------------------------------------------------------ */
/* API publique                                                        */
/* ------------------------------------------------------------------ */
int cc_ping(void)
{
    http_resp r = cc_http2("GET", "/health", NULL);
    int ok = (r.code == 200 && r.body && !strcmp(r.body, "ok"));
    free(r.body);
    return ok;
}

int cc_start(void)
{
    if (InterlockedCompareExchange(&g_started, 1, 0) == 1)
        return cc_ping() ? 0 : -1;

    if (!cc_ping()) {
        /* lance musicplayer-core.exe à côté de l'exe */
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) *slash = 0;
        wchar_t core[MAX_PATH];
        swprintf(core, MAX_PATH, L"%ls\\musicplayer-core.exe", exe);
        if (GetFileAttributesW(core) != INVALID_FILE_ATTRIBUTES) {
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            memset(&si, 0, sizeof(si));
            memset(&pi, 0, sizeof(pi));
            si.cb = sizeof(si);
            if (CreateProcessW(core, NULL, NULL, NULL, FALSE, 0, NULL, NULL,
                               &si, &pi)) {
                CloseHandle(pi.hThread);
                g_core_proc = pi.hProcess;   /* arrêt garanti à la fermeture */
            }
        }
        /* attend que le moteur réponde (jusqu'à ~10 s) */
        for (int i = 0; i < 50; i++) {
            if (cc_ping()) return 0;
            Sleep(200);
        }
        return -1;
    }
    return 0;
}

void cc_stop(void)
{
    /* 1) arrêt propre par l'API */
    cc_http2("POST", "/api/cmd", "{\"cmd\":\"shutdown\"}");
    /* 2) si le moteur a été lancé par CE client et ne s'arrête pas :
     * arrêt forcé (le moteur est conçu pour être relancé) */
    if (g_core_proc) {
        if (WaitForSingleObject(g_core_proc, 1500) != WAIT_OBJECT_0)
            TerminateProcess(g_core_proc, 0);
        CloseHandle(g_core_proc);
        g_core_proc = NULL;
    }
    InterlockedExchange(&g_started, 0);
}

void cc_cmd(const char* cmd)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"cmd\":\"%s\"}", cmd);
    http_resp r = cc_http2("POST", "/api/cmd", body);
    free(r.body);
}

void cc_cmd_val(const char* cmd, double value)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"cmd\":\"%s\",\"value\":%g}", cmd, value);
    http_resp r = cc_http2("POST", "/api/cmd", body);
    free(r.body);
}

void cc_cmd_path(const char* cmd, const char* path)
{
    /* échappement JSON du chemin */
    char esc[2048];
    int o = 0;
    for (const char* p = path; *p && o < (int)sizeof(esc) - 8; p++) {
        if (*p == '"' || *p == '\\') { esc[o++] = '\\'; esc[o++] = *p; }
        else esc[o++] = *p;
    }
    esc[o] = 0;
    char body[2304];
    snprintf(body, sizeof(body), "{\"cmd\":\"%s\",\"path\":\"%s\"}", cmd, esc);
    http_resp r = cc_http2("POST", "/api/cmd", body);
    free(r.body);
}

static double json_num(const char* s, const char* key, double def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = s ? strstr(s, pat) : NULL;
    if (!p) return def;
    return atof(p + strlen(pat));
}

static void json_str(const char* s, const char* key, char* out, int outsz)
{
    out[0] = 0;
    if (!s) return;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = strstr(s, pat);
    if (!p) return;
    p += strlen(pat);
    const char* e = strchr(p, '"');
    if (!e) return;
    int n = (int)(e - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, (size_t)n);
    out[n] = 0;
    /* dé-échappement simple */
    char* w = out;
    for (char* r = out; *r; r++) {
        if (*r == '\\' && r[1]) { r++; *w++ = *r; }
        else *w++ = *r;
    }
    *w = 0;
}

void cc_poll(void)
{
    http_resp r = cc_http2("GET", "/api/state", NULL);
    if (r.body && r.code == 200) {
        g_cc.state    = (int)json_num(r.body, "state", 0);
        g_cc.pos      = json_num(r.body, "pos", 0);
        g_cc.dur      = json_num(r.body, "dur", 0);
        g_cc.idx      = (int)json_num(r.body, "idx", -1);
        g_cc.count    = (int)json_num(r.body, "count", 0);
        g_cc.speed    = (float)json_num(r.body, "speed", 1.0);
        g_cc.shuffle  = (int)json_num(r.body, "shuffle", 0);
        json_str(r.body, "name",   g_cc.name,   sizeof(g_cc.name));
        json_str(r.body, "title",  g_cc.title,  sizeof(g_cc.title));
        json_str(r.body, "artist", g_cc.artist, sizeof(g_cc.artist));
        json_str(r.body, "album",  g_cc.album,  sizeof(g_cc.album));
        json_str(r.body, "year",   g_cc.year,   sizeof(g_cc.year));
    }
    free(r.body);
}

const cc_state_t* cc_state(void) { return &g_cc; }

void cc_plist_refresh(void)
{
    http_resp r = cc_http2("GET", "/api/plist", NULL);
    if (!r.body || r.code != 200) { free(r.body); return; }
    /* {"items":["path1","path2",...]} — remplit le cache local.
     * Accès mono-thread (thread UI) : pas de verrou nécessaire. */
    for (int i = 0; i < g_plist_n; i++) free(g_plist[i]);
    g_plist_n = 0;
    const char* p = strstr(r.body, "\"items\":[");
    if (p) {
        p += 9;
        while (*p && *p != ']' && g_plist_n < PLAYLIST_MAX) {
            if (*p == '"') {
                p++;
                char item[MAX_PATH * 2];
                int o = 0;
                while (*p && *p != '"' && o < (int)sizeof(item) - 1) {
                    if (*p == '\\' && p[1]) { p++; }
                    item[o++] = *p++;
                }
                item[o] = 0;
                wchar_t wp[MAX_PATH * 2];
                MultiByteToWideChar(CP_UTF8, 0, item, -1, wp, MAX_PATH * 2);
                g_plist[g_plist_n++] = _wcsdup(wp);
                if (*p == '"') p++;
            } else p++;
        }
    }
    free(r.body);
}

const wchar_t* cc_current_path(void)
{
    if (g_plist_idx >= 0 && g_plist_idx < g_plist_n)
        return g_plist[g_plist_idx];
    return NULL;
}

/* Nom de fichier du morceau courant (sans le dossier). */
const char* cc_name(void)
{
    static char name[512];
    name[0] = 0;
    const wchar_t* full = cc_current_path();
    if (!full) return name;
    char utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, full, -1, utf8, sizeof(utf8), NULL, NULL);
    const char* b = strrchr(utf8, '\\');
    snprintf(name, sizeof(name), "%s", b ? b + 1 : utf8);
    return name;
}

/* Ouvre un fichier (ou un dossier) via le moteur. Retourne 0. */
int cc_open(const char* path)
{
    cc_cmd_path("open", path);
    return 0;
}
