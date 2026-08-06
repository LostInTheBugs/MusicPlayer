/*
 * Podcasts — plugin SERVICE (moteur, core_plugins/)
 * ==================================================
 * Abonnements RSS de podcasts + liste des épisodes + état lu/non lu +
 * position de reprise + téléchargement hors-ligne.
 *
 * Le MOTEUR joue un épisode directement par son URL (FFmpeg ouvre les
 * URL HTTP/HTTPS) : le client envoie {"cmd":"open","path":"<url>"}.
 *
 * Serveur HTTP sur le port 8082 (PODCAST_PORT) :
 *   GET  /podcasts              → liste des abonnements (JSON)
 *   POST /podcasts              → abonner {"url":"https://.../feed.xml"}
 *   POST /podcasts/del          → désabonner {"url":"..."}
 *   GET  /podcasts/episodes?feed=<url> → épisodes d'un abonnement
 *   POST /episodes              → état {"feed","url","played","pos"}
 *   POST /refresh               → vérifie tous les flux (nouveaux épisodes)
 *   POST /download              → télécharge un épisode {"url":"..."}
 *
 * Données dans %APPDATA%\MusicPlayer\podcasts\ (podcasts.txt +
 * episodes.txt, une entrée par ligne, séparateur '|').
 */
#include <winsock2.h>
#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/plugin.h"
#include "http_util.h"

#define PODCAST_PORT 8082

#define MAX_POD 32
#define MAX_EP  1024
#define MAX_ITEM 20000

static const mp_host_api* g_h = NULL;
static volatile LONG g_running = 0;
static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_thread = NULL;

/* ------------------------------------------------------------------ */
/* Store : abonnements + épisodes                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char url[512];
    char title[256];
} podcast_t;

typedef struct {
    char feed[512];
    char url[512];
    char title[256];
    char date[64];
    int  dur;          /* secondes, 0 = inconnue */
    int  played;
    double pos;
} episode_t;

static podcast_t g_pods[MAX_POD];
static int g_pod_n = 0;
static episode_t g_eps[MAX_EP];
static int g_ep_n = 0;

static void store_dir(wchar_t* out, int cap);
static void strip_pipe(char* s);
static void log_line(const char* msg);

/* --- sources de podcasts (flux directs + annuaires de recherche) --- */
#define MAX_SRC 16

typedef struct {
    char type[16];      /* "rss" (flux direct) ou "search" (annuaire) */
    char name[128];
    char url[512];      /* les annuaires contiennent {query} */
} pod_source_t;

static pod_source_t g_sources[MAX_SRC];
static int g_src_n = 0;

static void source_defaults(void)
{
    static const pod_source_t defs[] = {
        { "search", "Apple Podcasts",
          "https://itunes.apple.com/search?term={query}&media=podcast&limit=15" },
    };
    g_src_n = (int)(sizeof(defs) / sizeof(defs[0]));
    for (int i = 0; i < g_src_n && i < MAX_SRC; i++)
        g_sources[i] = defs[i];
}

static void source_load(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    store_dir(dir, MAX_PATH);
    swprintf(path, MAX_PATH, L"%ls\\sources.txt", dir);
    FILE* f = _wfopen(path, L"r");
    if (!f) { source_defaults(); return; }
    g_src_n = 0;
    char line[768];
    while (g_src_n < MAX_SRC && fgets(line, sizeof(line), f)) {
        /* type|name|url */
        char* p1 = strchr(line, '|');
        if (!p1) continue;
        char* p2 = strchr(p1 + 1, '|');
        if (!p2) continue;
        *p1 = *p2 = 0;
        char* url = p2 + 1;
        url[strcspn(url, "\r\n")] = 0;
        if (line[0] && url[0]) {
            strncpy(g_sources[g_src_n].type, line, sizeof(g_sources[0].type) - 1);
            strncpy(g_sources[g_src_n].name, p1 + 1, sizeof(g_sources[0].name) - 1);
            strncpy(g_sources[g_src_n].url, url, sizeof(g_sources[0].url) - 1);
            strip_pipe(g_sources[g_src_n].name);
            g_src_n++;
        }
    }
    fclose(f);
    if (g_src_n == 0) source_defaults();
}

static void source_save(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    store_dir(dir, MAX_PATH);
    swprintf(path, MAX_PATH, L"%ls\\sources.txt", dir);
    FILE* f = _wfopen(path, L"w");
    if (f) {
        for (int i = 0; i < g_src_n; i++)
            fprintf(f, "%s|%s|%s\n", g_sources[i].type, g_sources[i].name,
                    g_sources[i].url);
        fclose(f);
    }
}

static void store_dir(wchar_t* out, int cap)
{
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, out) == S_OK) {
        wcscat_s(out, cap, L"\\MusicPlayer\\podcasts");
        CreateDirectoryW(out, NULL);
    } else {
        wcscpy_s(out, cap, L"podcasts");
    }
}

static void strip_pipe(char* s)
{
    for (char* p = s; *p; p++)
        if (*p == '|' || *p == '\r' || *p == '\n') *p = ' ';
}

static void store_load(void)
{
    g_pod_n = 0;
    g_ep_n = 0;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    store_dir(dir, MAX_PATH);
    /* podcasts.txt */
    swprintf(path, MAX_PATH, L"%ls\\podcasts.txt", dir);
    FILE* f = _wfopen(path, L"r");
    if (f) {
        char line[1024];
        while (g_pod_n < MAX_POD && fgets(line, sizeof(line), f)) {
            char* bar = strchr(line, '|');
            if (!bar) continue;
            *bar = 0;
            char* t = bar + 1;
            t[strcspn(t, "\r\n")] = 0;
            strip_pipe(line);
            strip_pipe(t);
            if (line[0]) {
                strncpy(g_pods[g_pod_n].url, line, sizeof(g_pods[g_pod_n].url) - 1);
                strncpy(g_pods[g_pod_n].title, t, sizeof(g_pods[g_pod_n].title) - 1);
                g_pod_n++;
            }
        }
        fclose(f);
    }
    /* episodes.txt */
    swprintf(path, MAX_PATH, L"%ls\\episodes.txt", dir);
    f = _wfopen(path, L"r");
    if (f) {
        char line[2048];
        while (g_ep_n < MAX_EP && fgets(line, sizeof(line), f)) {
            /* feed|url|title|date|dur|played|pos */
            char* p = line;
            char* f1 = strchr(p, '|');  if (!f1) continue;
            char* f2 = strchr(f1 + 1, '|'); if (!f2) continue;
            char* f3 = strchr(f2 + 1, '|'); if (!f3) continue;
            char* f4 = strchr(f3 + 1, '|'); if (!f4) continue;
            char* f5 = strchr(f4 + 1, '|'); if (!f5) continue;
            char* f6 = strchr(f5 + 1, '|'); if (!f6) continue;
            *f1 = *f2 = *f3 = *f4 = *f5 = *f6 = 0;
            episode_t* e = &g_eps[g_ep_n];
            strncpy(e->feed, p, sizeof(e->feed) - 1);
            strncpy(e->url, f1 + 1, sizeof(e->url) - 1);
            strncpy(e->title, f2 + 1, sizeof(e->title) - 1);
            strncpy(e->date, f3 + 1, sizeof(e->date) - 1);
            e->dur = atoi(f4 + 1);
            e->played = atoi(f5 + 1);
            e->pos = atof(f6 + 1);
            strip_pipe(e->title);
            g_ep_n++;
        }
        fclose(f);
    }
}

static void store_save(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    store_dir(dir, MAX_PATH);
    swprintf(path, MAX_PATH, L"%ls\\podcasts.txt", dir);
    FILE* f = _wfopen(path, L"w");
    if (f) {
        for (int i = 0; i < g_pod_n; i++)
            fprintf(f, "%s|%s\n", g_pods[i].url, g_pods[i].title);
        fclose(f);
    }
    swprintf(path, MAX_PATH, L"%ls\\episodes.txt", dir);
    f = _wfopen(path, L"w");
    if (f) {
        for (int i = 0; i < g_ep_n; i++)
            fprintf(f, "%s|%s|%s|%s|%d|%d|%.1f\n", g_eps[i].feed,
                    g_eps[i].url, g_eps[i].title, g_eps[i].date,
                    g_eps[i].dur, g_eps[i].played, g_eps[i].pos);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/* Fetch HTTP (WinINet, direct) + parsing RSS                          */
/* ------------------------------------------------------------------ */
static int fetch_url(const char* url, char** out, int* out_len)
{
    wchar_t wurl[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
    /* 2 tentatives : les erreurs réseau sont souvent transitoires */
    for (int attempt = 0; attempt < 2; attempt++) {
        {
            char m[400];
            snprintf(m, sizeof(m), "Podcasts: fetching %s (attempt %d/%d)",
                     url, attempt + 1, 2);
            log_line(m);
        }
        HINTERNET inet = InternetOpenW(L"MusicPlayer-Podcasts",
                                       INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!inet) return -1;
        DWORD to = 20000;
        InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
        InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
        HINTERNET uh = InternetOpenUrlW(inet, wurl, NULL, 0,
                                        INTERNET_FLAG_RELOAD |
                                        INTERNET_FLAG_NO_CACHE_WRITE |
                                        INTERNET_FLAG_SECURE, 0);
        if (!uh) {
            /* réessai sans le flag SECURE (http simple) */
            uh = InternetOpenUrlW(inet, wurl, NULL, 0,
                                  INTERNET_FLAG_RELOAD |
                                  INTERNET_FLAG_NO_CACHE_WRITE, 0);
        }
        if (!uh) {
            InternetCloseHandle(inet);
            if (attempt == 0) { Sleep(1500); continue; }
            return -1;
        }
        char buf[8192];
        int cap = 16384, len = 0;
        char* body = (char*)malloc(cap);
        if (!body) {
            InternetCloseHandle(uh);
            InternetCloseHandle(inet);
            return -1;
        }
        int ok = 1;
        for (;;) {
            DWORD got = 0;
            if (!InternetReadFile(uh, buf, sizeof(buf), &got) || got == 0) break;
            if (len + (int)got + 1 > cap) {
                cap = (len + (int)got + 1) * 2;
                char* nb = (char*)realloc(body, cap);
                if (!nb) { ok = 0; break; }
                body = nb;
            }
            memcpy(body + len, buf, got);
            len += (int)got;
        }
        InternetCloseHandle(uh);
        InternetCloseHandle(inet);
        if (ok && len > 0) {
            body[len] = 0;
            *out = body;
            *out_len = len;
            return 0;
        }
        free(body);
        if (attempt == 0) Sleep(1500);
    }
    return -1;
}

static void html_unescape(char* s)
{
    char* r = s;
    char* w = s;
    while (*r) {
        if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
        else if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; }
        else if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; }
        else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
        else if (strncmp(r, "&#39;", 5) == 0) { *w++ = '\''; r += 5; }
        else if (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; }
        else if (strncmp(r, "&nbsp;", 6) == 0) { *w++ = ' '; r += 6; }
        else if (*r == '&' && *(r + 1) == '#') {
            /* entité numérique &#NNN; */
            char* e = strchr(r, ';');
            if (e && e - r < 12) {
                *w++ = (char)atoi(r + 2);
                r = e + 1;
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

/* extrait le texte entre <tag> et </tag> (première occurrence) */
static void tag_text(const char* xml, const char* tag, char* out, int outsz)
{
    out[0] = 0;
    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char* a = strstr(xml, open);
    if (!a) return;
    a += strlen(open);
    const char* b = strstr(a, close);
    if (!b) return;
    int n = (int)(b - a);
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, a, n);
    out[n] = 0;
    html_unescape(out);
    /* nettoie les retours/lignes multiples */
    for (char* p = out; *p; p++) {
        if (*p == '\r' || *p == '\n') *p = ' ';
    }
    /* compacte les espaces multiples */
    char* r = out;
    char* w = out;
    int sp = 0;
    while (*r) {
        if (*r == ' ') { if (!sp) *w++ = ' '; sp = 1; }
        else { *w++ = *r; sp = 0; }
        r++;
    }
    *w = 0;
}

/* extrait l'attribut attr="..." de la balise <tag ...> */
static void tag_attr(const char* xml, const char* tag, const char* attr,
                     char* out, int outsz)
{
    out[0] = 0;
    char pat[96];
    snprintf(pat, sizeof(pat), "<%s", tag);
    const char* a = strstr(xml, pat);
    if (!a) return;
    const char* e = strchr(a, '>');
    if (!e) return;
    char tmp[1024];
    int n = (int)(e - a);
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    memcpy(tmp, a, n);
    tmp[n] = 0;
    snprintf(pat, sizeof(pat), "%s=\"", attr);
    const char* v = strstr(tmp, pat);
    if (!v) {
        snprintf(pat, sizeof(pat), "%s='", attr);
        v = strstr(tmp, pat);
        if (v) {
            v += strlen(pat);
            const char* q = strchr(v, '\'');
            if (!q) return;
            n = (int)(q - v);
        }
    } else {
        v += strlen(pat);
        const char* q = strchr(v, '"');
        if (!q) return;
        n = (int)(q - v);
    }
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, v, n);
    out[n] = 0;
    html_unescape(out);
}

/* "10:00" / "1:10:00" / "600" → secondes */
static int parse_duration(const char* s)
{
    int a = 0, b = 0, c = 0;
    if (sscanf(s, "%d:%d:%d", &a, &b, &c) == 3) return a * 3600 + b * 60 + c;
    if (sscanf(s, "%d:%d", &a, &b) == 2) return a * 60 + b;
    return atoi(s);
}

/* parse un flux RSS : titre du channel + épisodes */
static void parse_feed(const char* xml, char* title, int title_sz,
                       episode_t* eps, int* n, int max, const char* feed_url)
{
    title[0] = 0;
    /* titre du channel : premier <title> avant le premier <item> */
    const char* first_item = strstr(xml, "<item");
    const char* t = strstr(xml, "<title>");
    if (t && (!first_item || t < first_item)) {
        const char* close = strstr(t, "</title>");
        if (close) {
            int len = (int)(close - (t + 7));
            if (len > title_sz - 1) len = title_sz - 1;
            memcpy(title, t + 7, len);
            title[len] = 0;
            html_unescape(title);
        }
    }
    /* épisodes */
    const char* it = xml;
    while (*n < max && (it = strstr(it, "<item")) != NULL) {
        const char* end = strstr(it, "</item>");
        if (!end) break;
        int len = (int)(end - it);
        if (len > MAX_ITEM) len = MAX_ITEM;
        char* tmp = (char*)malloc((size_t)len + 1);
        if (!tmp) break;
        memcpy(tmp, it, len);
        tmp[len] = 0;
        episode_t* e = &eps[*n];
        memset(e, 0, sizeof(*e));
        strncpy(e->feed, feed_url, sizeof(e->feed) - 1);
        tag_text(tmp, "title", e->title, sizeof(e->title));
        tag_attr(tmp, "enclosure", "url", e->url, sizeof(e->url));
        if (!e->url[0])
            tag_text(tmp, "link", e->url, sizeof(e->url));
        /* pubDate : on garde les 3 premiers mots (Mon, 03 Aug 2026) */
        {
            char d[128];
            tag_text(tmp, "pubDate", d, sizeof(d));
            char w1[16], w2[16], w3[16], w4[16];
            if (sscanf(d, "%15s %15s %15s %15s", w1, w2, w3, w4) == 4)
                snprintf(e->date, sizeof(e->date), "%s %s %s %s", w1, w2, w3, w4);
            else
                snprintf(e->date, sizeof(e->date), "%s", d);
        }
        {
            char d[64];
            tag_text(tmp, "itunes:duration", d, sizeof(d));
            if (!d[0]) tag_text(tmp, "duration", d, sizeof(d));
            e->dur = parse_duration(d);
        }
        free(tmp);
        if (e->url[0]) {
            (*n)++;
        }
        it = end + 7;
    }
}

/* ------------------------------------------------------------------ */
/* Abonnement : fetch + parse + ajout (avec détection des nouveaux)    */
/* ------------------------------------------------------------------ */
static int add_subscription(const char* url, char* title_out, int title_sz,
                            int* new_eps)
{
    char* xml = NULL;
    int len = 0;
    if (fetch_url(url, &xml, &len) != 0) {
        char m[320];
        snprintf(m, sizeof(m), "Podcasts: fetch failed for %s", url);
        log_line(m);
        return -1;
    }
    episode_t eps[MAX_EP];
    int n = 0;
    char title[256];
    parse_feed(xml, title, sizeof(title), eps, &n, MAX_EP, url);
    {
        char m[320];
        snprintf(m, sizeof(m), "Podcasts: fetched %d o, %d episodes, title '%s'",
                 len, n, title[0] ? title : "(none)");
        log_line(m);
        /* début du contenu (diagnostic : HTML, binaire, XML…) */
        char head[160] = "";
        int hn = 0;
        for (int i = 0; i < len && hn < 120; i++) {
            unsigned char ch = (unsigned char)xml[i];
            if (ch >= 32 && ch < 127) head[hn++] = (char)ch;
            else if (ch == '\n' || ch == '\r') head[hn++] = ' ';
            else head[hn++] = '.';
        }
        head[hn] = 0;
        snprintf(m, sizeof(m), "Podcasts: head: %s", head);
        log_line(m);
    }
    free(xml);
    if (n == 0 && !title[0]) {
        log_line("Podcasts: invalid feed (no title, no episodes)");
        return -2;   /* pas un flux RSS valide */
    }

    /* abonnement existant ? */
    int idx = -1;
    for (int i = 0; i < g_pod_n; i++)
        if (!strcmp(g_pods[i].url, url)) { idx = i; break; }
    if (idx < 0) {
        if (g_pod_n >= MAX_POD) return -2;
        idx = g_pod_n;
        strncpy(g_pods[idx].url, url, sizeof(g_pods[idx].url) - 1);
        g_pod_n++;
    }
    if (title[0])
        strncpy(g_pods[idx].title, title, sizeof(g_pods[idx].title) - 1);

    /* épisodes : ajoute les inconnus (played=0) */
    int added = 0;
    for (int i = 0; i < n; i++) {
        int known = 0;
        for (int j = 0; j < g_ep_n; j++)
            if (!strcmp(g_eps[j].url, eps[i].url)) { known = 1; break; }
        if (!known) {
            if (g_ep_n >= MAX_EP) break;
            g_eps[g_ep_n++] = eps[i];
            added++;
        }
    }
    if (new_eps) *new_eps = added;
    if (title_out)
        strncpy(title_out, g_pods[idx].title, title_sz - 1);
    store_save();
    return 0;
}

static int refresh_all(int* new_total)
{
    int total = 0;
    for (int i = 0; i < g_pod_n; i++) {
        int added = 0;
        add_subscription(g_pods[i].url, NULL, 0, &added);
        total += added;
    }
    if (new_total) *new_total = total;
    return 0;
}

static int download_episode(const char* url)
{
    char* data = NULL;
    int len = 0;
    if (fetch_url(url, &data, &len) != 0) return -1;
    if (len < 1000) { free(data); return -1; }   /* pas un MP3 */
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    store_dir(dir, MAX_PATH);
    /* nom : dernier segment de l'URL */
    const char* name = strrchr(url, '/');
    name = name ? name + 1 : url;
    if (!*name) { free(data); return -1; }
    wchar_t wname[128];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128);
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir, wname);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { free(data); return -1; }
    DWORD wr = 0;
    BOOL ok = WriteFile(f, data, (DWORD)len, &wr, NULL);
    CloseHandle(f);
    free(data);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */
static void log_line(const char* msg)
{
    if (g_h && g_h->log) g_h->log(msg);
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

static const char* json_str_val(const char* body, const char* key,
                                char* out, int outsz)
{
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return out;
    p = strchr(p, ':');
    if (!p) return out;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return out;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < outsz - 1) out[n++] = *p++;
    out[n] = 0;
    return out;
}

static double json_num(const char* body, const char* key, double def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return def;
    p = strchr(p, ':');
    if (!p) return def;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atof(p);
}

/* ------------------------------------------------------------------ */
/* Serveur HTTP (pattern des plugins réseau)                           */
/* ------------------------------------------------------------------ */
static void send_json(SOCKET c, int code, const char* body)
{
    char hdr[512];
    int len = (int)strlen(body);
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, code == 200 ? "OK" : "Error", len);
    send(c, hdr, (int)strlen(hdr), 0);
    send(c, body, len, 0);
}

/* encode URL une chaîne (query string) */
static void url_encode(const char* in, char* out, int outsz)
{
    int o = 0;
    for (int i = 0; in[i] && o < outsz - 4; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out[o++] = (char)c;
        } else {
            o += snprintf(out + o, outsz - o, "%%%02X", c);
        }
    }
    out[o] = 0;
}

/* décode %XX d'une query string */
static void url_decode(char* s)
{
    char* r = s;
    char* w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char h[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(h, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

/* recherche dans un annuaire : fetch + parse des 3 formats connus
 * (iTunes/Apple, Listen Notes, Podcast Index) */
static void handle_search(SOCKET c, const char* query)
{
    /* query: source=<url encodé>&query=<terme encodé> */
    char src_url[512] = "", term[256] = "";
    {
        const char* p = strstr(query, "source=");
        if (p) {
            p += 7;
            int n = 0;
            while (p[n] && p[n] != '&' && n < (int)sizeof(src_url) - 1)
                src_url[n++] = p[n];
            src_url[n] = 0;
        }
        p = strstr(query, "query=");
        if (p) {
            p += 6;
            int n = 0;
            while (p[n] && p[n] != '&' && n < (int)sizeof(term) - 1)
                term[n++] = p[n];
            term[n] = 0;
        }
    }
    url_decode(src_url);
    url_decode(term);
    if (!src_url[0] || !term[0]) {
        send_json(c, 400, "{\"error\":\"source and query required\"}");
        return;
    }
    /* remplace {query} par le terme encodé */
    char enc[512];
    url_encode(term, enc, sizeof(enc));
    char url[1024] = "";
    {
        const char* ph = strstr(src_url, "{query}");
        if (ph) {
            int pre = (int)(ph - src_url);
            if (pre > (int)sizeof(url) - 1) pre = (int)sizeof(url) - 1;
            memcpy(url, src_url, pre);
            url[pre] = 0;
            strncat(url, enc, sizeof(url) - strlen(url) - 1);
            strncat(url, ph + 7, sizeof(url) - strlen(url) - 1);
        } else {
            strncpy(url, src_url, sizeof(url) - 1);
        }
    }
    char* body = NULL;
    int len = 0;
    if (fetch_url(url, &body, &len) != 0) {
        send_json(c, 400, "{\"error\":\"search failed (network)\"}");
        return;
    }
    /* parse les objets { ... } à la recherche des clés connues */
    char resp[16384];
    int o = snprintf(resp, sizeof(resp), "{\"results\":[");
    int n = 0;
    const char* p = body;
    while ((p = strchr(p, '{')) != NULL) {
        const char* end = strchr(p, '}');
        if (!end) break;
        if (end - p > 8 && end - p < 4000) {
            char obj[4096];
            int olen = (int)(end - p) + 1;
            memcpy(obj, p, olen);
            obj[olen] = 0;
            /* le flux : feedUrl / rss / url */
            char feed[512] = "", title[256] = "", author[256] = "";
            char tmp[512];
            json_str_val(obj, "feedUrl", tmp, sizeof(tmp));
            if (tmp[0]) strncpy(feed, tmp, sizeof(feed) - 1);
            if (!feed[0]) {
                json_str_val(obj, "rss", tmp, sizeof(tmp));
                if (tmp[0]) strncpy(feed, tmp, sizeof(feed) - 1);
            }
            if (!feed[0]) {
                json_str_val(obj, "url", tmp, sizeof(tmp));
                /* url peut être l'URL du site : on ne garde que si .rss/.xml */
                if (tmp[0] && (strstr(tmp, ".rss") || strstr(tmp, ".xml") ||
                               strstr(tmp, "feed") || strstr(tmp, "podcast")))
                    strncpy(feed, tmp, sizeof(feed) - 1);
            }
            json_str_val(obj, "collectionName", tmp, sizeof(tmp));
            if (tmp[0]) strncpy(title, tmp, sizeof(title) - 1);
            if (!title[0]) {
                json_str_val(obj, "title_original", tmp, sizeof(tmp));
                if (tmp[0]) strncpy(title, tmp, sizeof(title) - 1);
            }
            if (!title[0]) {
                json_str_val(obj, "title", tmp, sizeof(tmp));
                if (tmp[0]) strncpy(title, tmp, sizeof(title) - 1);
            }
            json_str_val(obj, "artistName", tmp, sizeof(tmp));
            if (tmp[0]) strncpy(author, tmp, sizeof(author) - 1);
            if (!author[0]) {
                json_str_val(obj, "publisher_original", tmp, sizeof(tmp));
                if (tmp[0]) strncpy(author, tmp, sizeof(author) - 1);
            }
            if (!author[0]) {
                json_str_val(obj, "author", tmp, sizeof(tmp));
                if (tmp[0]) strncpy(author, tmp, sizeof(author) - 1);
            }
            if (feed[0] && strncmp(feed, "https", 5) == 0 &&
                strncmp(feed, "http", 4) == 0) {
                char efeed[1024], etitle[512], eauth[512];
                json_escape_a(feed, efeed, sizeof(efeed));
                json_escape_a(title, etitle, sizeof(etitle));
                json_escape_a(author, eauth, sizeof(eauth));
                o += snprintf(resp + o, sizeof(resp) - o,
                              "%s{\"title\":\"%s\",\"author\":\"%s\","
                              "\"feed\":\"%s\"}",
                              n ? "," : "", etitle, eauth, efeed);
                n++;
                if (o > (int)sizeof(resp) - 256) break;
            }
        }
        p = end + 1;
    }
    free(body);
    snprintf(resp + o, sizeof(resp) - o, "]}");
    send_json(c, 200, resp);
}

static void handle_get(SOCKET c, const char* path, const char* query)
{
    if (!strcmp(path, "/podcasts/sources")) {
        char body[8192];
        int n = snprintf(body, sizeof(body), "{\"sources\":[");
        for (int i = 0; i < g_src_n; i++) {
            char ename[256], eurl[1024];
            json_escape_a(g_sources[i].name, ename, sizeof(ename));
            json_escape_a(g_sources[i].url, eurl, sizeof(eurl));
            n += snprintf(body + n, sizeof(body) - n,
                          "%s{\"type\":\"%s\",\"name\":\"%s\",\"url\":\"%s\"}",
                          i ? "," : "", g_sources[i].type, ename, eurl);
            if (n > (int)sizeof(body) - 256) break;
        }
        snprintf(body + n, sizeof(body) - n, "]}");
        send_json(c, 200, body);
    } else if (!strcmp(path, "/podcasts/search")) {
        handle_search(c, query ? query : "");
    } else if (!strcmp(path, "/podcasts") || !strcmp(path, "/podcasts/")) {
        char body[8192];
        int n = snprintf(body, sizeof(body), "{\"podcasts\":[");
        for (int i = 0; i < g_pod_n; i++) {
            int unread = 0;
            for (int j = 0; j < g_ep_n; j++)
                if (!strcmp(g_eps[j].feed, g_pods[i].url) && !g_eps[j].played)
                    unread++;
            char eurl[2048], etitle[512];
            json_escape_a(g_pods[i].url, eurl, sizeof(eurl));
            json_escape_a(g_pods[i].title, etitle, sizeof(etitle));
            n += snprintf(body + n, sizeof(body) - n,
                          "%s{\"url\":\"%s\",\"title\":\"%s\",\"unread\":%d}",
                          i ? "," : "", eurl, etitle, unread);
            if (n > (int)sizeof(body) - 256) break;
        }
        snprintf(body + n, sizeof(body) - n, "]}");
        send_json(c, 200, body);
    } else if (!strcmp(path, "/podcasts/episodes")) {
        char feed[512];
        const char* q = query ? strstr(query, "feed=") : NULL;
        if (!q) { send_json(c, 400, "{\"error\":\"feed required\"}"); return; }
        q += 5;
        int nq = 0;
        while (q[nq] && q[nq] != '&' && nq < (int)sizeof(feed) - 1) {
            feed[nq] = q[nq] == '+' ? ' ' : q[nq];
            nq++;
        }
        feed[nq] = 0;
        /* décodage %XX */
        {
            char dec[512];
            int d = 0;
            for (int i = 0; feed[i] && d < (int)sizeof(dec) - 1; i++) {
                if (feed[i] == '%' && feed[i + 1] && feed[i + 2]) {
                    char h[3] = { feed[i + 1], feed[i + 2], 0 };
                    dec[d++] = (char)strtol(h, NULL, 16);
                    i += 2;
                } else {
                    dec[d++] = feed[i];
                }
            }
            dec[d] = 0;
            strncpy(feed, dec, sizeof(feed) - 1);
        }
        char body[16384];
        int n = snprintf(body, sizeof(body), "{\"episodes\":[");
        for (int i = 0; i < g_ep_n; i++) {
            if (strcmp(g_eps[i].feed, feed)) continue;
            char eurl[2048], etitle[512];
            json_escape_a(g_eps[i].url, eurl, sizeof(eurl));
            json_escape_a(g_eps[i].title, etitle, sizeof(etitle));
            n += snprintf(body + n, sizeof(body) - n,
                          "%s{\"url\":\"%s\",\"title\":\"%s\",\"date\":\"%s\","
                          "\"dur\":%d,\"played\":%d,\"pos\":%.1f}",
                          n > 14 ? "," : "", eurl, etitle, g_eps[i].date,
                          g_eps[i].dur, g_eps[i].played, g_eps[i].pos);
            if (n > (int)sizeof(body) - 256) break;
        }
        snprintf(body + n, sizeof(body) - n, "]}");
        send_json(c, 200, body);
    } else {
        send_json(c, 404, "{\"error\":\"not found\"}");
    }
}

static void handle_post(SOCKET c, const char* path, const char* body)
{
    if (!strcmp(path, "/podcasts/sources")) {
        char type[16], name[128], url[512];
        json_str_val(body, "type", type, sizeof(type));
        json_str_val(body, "name", name, sizeof(name));
        json_str_val(body, "url", url, sizeof(url));
        if (!url[0] || !name[0]) {
            send_json(c, 400, "{\"error\":\"name and url required\"}");
            return;
        }
        if (!type[0]) strcpy(type, "search");
        if (g_src_n >= MAX_SRC) {
            send_json(c, 400, "{\"error\":\"too many sources\"}");
            return;
        }
        strncpy(g_sources[g_src_n].type, type, sizeof(g_sources[0].type) - 1);
        strncpy(g_sources[g_src_n].name, name, sizeof(g_sources[0].name) - 1);
        strncpy(g_sources[g_src_n].url, url, sizeof(g_sources[0].url) - 1);
        g_src_n++;
        source_save();
        send_json(c, 200, "{\"ok\":1}");
    } else if (!strcmp(path, "/podcasts/sources/del")) {
        char url[512];
        json_str_val(body, "url", url, sizeof(url));
        for (int i = 0; i < g_src_n; i++) {
            if (!strcmp(g_sources[i].url, url)) {
                for (int j = i; j < g_src_n - 1; j++)
                    g_sources[j] = g_sources[j + 1];
                g_src_n--;
                source_save();
                send_json(c, 200, "{\"ok\":1}");
                return;
            }
        }
        send_json(c, 404, "{\"error\":\"not found\"}");
    } else if (!strcmp(path, "/podcasts")) {
        char url[512];
        json_str_val(body, "url", url, sizeof(url));
        if (!url[0]) { send_json(c, 400, "{\"error\":\"url required\"}"); return; }
        char title[256] = "";
        int added = 0;
        int rc = add_subscription(url, title, sizeof(title), &added);
        if (rc == -1) {
            send_json(c, 400, "{\"error\":\"network\"}");
        } else if (rc == -2) {
            send_json(c, 400, "{\"error\":\"too many subscriptions\"}");
        } else {
            char resp[1024];
            char etitle[512];
            json_escape_a(title, etitle, sizeof(etitle));
            snprintf(resp, sizeof(resp),
                     "{\"ok\":1,\"title\":\"%s\",\"new\":%d}", etitle, added);
            send_json(c, 200, resp);
        }
    } else if (!strcmp(path, "/podcasts/del")) {
        char url[512];
        json_str_val(body, "url", url, sizeof(url));
        for (int i = 0; i < g_pod_n; i++) {
            if (!strcmp(g_pods[i].url, url)) {
                for (int j = i; j < g_pod_n - 1; j++) g_pods[j] = g_pods[j + 1];
                g_pod_n--;
                /* supprime aussi ses épisodes */
                int w = 0;
                for (int j = 0; j < g_ep_n; j++) {
                    if (strcmp(g_eps[j].feed, url)) g_eps[w++] = g_eps[j];
                }
                g_ep_n = w;
                store_save();
                send_json(c, 200, "{\"ok\":1}");
                return;
            }
        }
        send_json(c, 404, "{\"error\":\"not subscribed\"}");
    } else if (!strcmp(path, "/episodes")) {
        char feed[512], url[512];
        json_str_val(body, "feed", feed, sizeof(feed));
        json_str_val(body, "url", url, sizeof(url));
        if (!url[0]) { send_json(c, 400, "{\"error\":\"url required\"}"); return; }
        for (int i = 0; i < g_ep_n; i++) {
            if (!strcmp(g_eps[i].url, url)) {
                g_eps[i].played = (int)json_num(body, "played", 0);
                g_eps[i].pos = json_num(body, "pos", 0);
                store_save();
                send_json(c, 200, "{\"ok\":1}");
                return;
            }
        }
        /* épisode inconnu : on l'ajoute (cas d'un flux déjà lu avant) */
        episode_t* e = g_ep_n < MAX_EP ? &g_eps[g_ep_n] : NULL;
        if (e) {
            memset(e, 0, sizeof(*e));
            strncpy(e->feed, feed, sizeof(e->feed) - 1);
            strncpy(e->url, url, sizeof(e->url) - 1);
            e->played = (int)json_num(body, "played", 0);
            e->pos = json_num(body, "pos", 0);
            g_ep_n++;
            store_save();
        }
        send_json(c, 200, "{\"ok\":1}");
    } else if (!strcmp(path, "/refresh")) {
        int total = 0;
        refresh_all(&total);
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":1,\"new\":%d}", total);
        send_json(c, 200, resp);
    } else if (!strcmp(path, "/download")) {
        char url[512];
        json_str_val(body, "url", url, sizeof(url));
        if (!url[0]) { send_json(c, 400, "{\"error\":\"url required\"}"); return; }
        if (download_episode(url) == 0)
            send_json(c, 200, "{\"ok\":1}");
        else
            send_json(c, 400, "{\"error\":\"download failed\"}");
    } else {
        send_json(c, 404, "{\"error\":\"not found\"}");
    }
}

static void handle_client(SOCKET c)
{
    char req[16384];
    int rn = http_read_request(c, req, sizeof(req));
    if (rn <= 0) { closesocket(c); return; }
    char method[8] = "", path[1024] = "", query[1024] = "";
    char* sp1 = strchr(req, ' ');
    if (!sp1) { closesocket(c); return; }
    int ml = (int)(sp1 - req);
    if (ml > 7) ml = 7;
    memcpy(method, req, ml);
    method[ml] = 0;
    char* sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) { closesocket(c); return; }
    int pl = (int)(sp2 - sp1 - 1);
    if (pl > (int)sizeof(path) - 1) pl = (int)sizeof(path) - 1;
    memcpy(path, sp1 + 1, pl);
    path[pl] = 0;
    char* qm = strchr(path, '?');
    if (qm) {
        strncpy(query, qm + 1, sizeof(query) - 1);
        *qm = 0;
    }
    /* corps (après la fin des en-têtes) */
    const char* body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";

    if (!strcmp(method, "GET")) {
        /* les vieux clients envoient un GET avec un corps au lieu d'un
         * vrai POST : on le traite comme un POST par tolérance */
        if (body[0]) handle_post(c, path, body);
        else handle_get(c, path, query);
    } else if (!strcmp(method, "POST")) {
        handle_post(c, path, body);
    } else {
        send_json(c, 405, "{\"error\":\"method\"}");
    }
    closesocket(c);
}

static DWORD WINAPI accept_loop(LPVOID arg)
{
    (void)arg;
    while (g_running) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) {
            if (!g_running) break;
            Sleep(50);
            continue;
        }
        handle_client(c);
    }
    return 0;
}

static int server_start(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { WSACleanup(); return -1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PODCAST_PORT);
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
static const char* pl_name(void) { return "Podcasts"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{
    return "RSS podcast subscriptions: episodes, read/unread, resume, download (port 8082)";
}
static unsigned pl_type(void) { return MP_PLUGIN_SERVICE; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_h = host;
    store_load();
    source_load();
    return 0;
}

static void pl_destroy(mp_plugin* self)
{
    (void)self;
    server_stop();
    store_save();
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
                snprintf(msg, sizeof(msg),
                         "Podcasts: listening on port %d", PODCAST_PORT);
                log_line(msg);
            } else {
                log_line("Podcasts: port unavailable");
            }
        }
    }
}

static const mp_plugin_api pl_api = {
    .api_version = MP_PLUGIN_API_VERSION,
    .name = pl_name,
    .version = pl_version,
    .description = pl_description,
    .type = pl_type,
    .init = pl_init,
    .destroy = pl_destroy,
    .service = pl_service,
};

const mp_plugin_api* mp_plugin_entry(void) { return &pl_api; }
