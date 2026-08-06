/*
 * MusicPlayer — interface graphique Windows (Win32, Unicode)
 * Menu, status bar, glisser-déposer, raccourcis clavier, mode --selftest.
 *
 * Conventions : toute l'UI est en UTF-16 (W). Les chemins de fichiers sont
 * convertis en UTF-8 pour le moteur (FFmpeg gère l'UTF-8 sur Windows).
 */
#include <winsock2.h>
#include <windows.h>
#include <wininet.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/avutil.h>

#include "player.h"
#include "client_core.h"
#include "stream_player.h"
#include "svc.h"
#include "repo.h"
#include "plugin.h"
#include "plugin_loader.h"
#include "lang.h"
#include "update.h"

/* requête HTTP vers le plugin podcasts du moteur (127.0.0.1:8082) —
 * prototype : défini plus bas, utilisé par le menu (create_menus). */
static char* podcast_http(const char* method, const char* path,
                          const char* body, int* out_len);
static void pod_json_str(const char* body, const char* key, char* out, int outsz);
static void pod_json_unescape(char* s);
static INT_PTR CALLBACK podcast_search_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l);

/* 1 si le plugin podcasts du moteur est présent ET actif (le service
 * répond sur le port 8082). Résultat mis en cache. */
/* GET brut vers le REST du moteur (8080) : retourne le corps (malloc) ou NULL */
static char* engine_http_get(const char* path, int* out_len)
{
    wchar_t url[1200];
    char upath[1024];
    snprintf(upath, sizeof(upath), "http://127.0.0.1:8080%s", path);
    MultiByteToWideChar(CP_UTF8, 0, upath, -1, url, 1200);
    HINTERNET inet = InternetOpenW(L"MusicPlayer", INTERNET_OPEN_TYPE_DIRECT,
                                   NULL, NULL, 0);
    if (!inet) return NULL;
    DWORD to = 15000;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    HINTERNET uh = InternetOpenUrlW(inet, url, NULL, 0,
                                    INTERNET_FLAG_RELOAD |
                                    INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!uh) { InternetCloseHandle(inet); return NULL; }
    char buf[4096];
    int cap = 8192, len = 0;
    char* resp = (char*)malloc(cap);
    if (!resp) { InternetCloseHandle(uh); InternetCloseHandle(inet); return NULL; }
    for (;;) {
        DWORD got = 0;
        if (!InternetReadFile(uh, buf, sizeof(buf), &got) || got == 0) break;
        if (len + (int)got + 1 > cap) {
            cap = (len + (int)got + 1) * 2;
            char* nb = (char*)realloc(resp, cap);
            if (!nb) { free(resp); InternetCloseHandle(uh); InternetCloseHandle(inet); return NULL; }
            resp = nb;
        }
        memcpy(resp + len, buf, got);
        len += (int)got;
    }
    InternetCloseHandle(uh);
    InternetCloseHandle(inet);
    resp[len] = 0;
    if (out_len) *out_len = len;
    return resp;
}

static int g_podcasts_cached = -1;

/* POST JSON vers le REST du moteur (8080) : retourne le corps (malloc) ou NULL */
static char* engine_http_post(const char* path, const char* body, int* out_len)
{
    wchar_t url[1200];
    char upath[1024];
    snprintf(upath, sizeof(upath), "http://127.0.0.1:8080%s", path);
    MultiByteToWideChar(CP_UTF8, 0, upath, -1, url, 1200);
    HINTERNET inet = InternetOpenW(L"MusicPlayer", INTERNET_OPEN_TYPE_DIRECT,
                                   NULL, NULL, 0);
    if (!inet) return NULL;
    DWORD to = 15000;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    const wchar_t* hdrs = L"Content-Type: application/json\r\n";
    HINTERNET uh = InternetOpenUrlW(inet, url, hdrs, -1,
                                    INTERNET_FLAG_RELOAD |
                                    INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!uh) { InternetCloseHandle(inet); return NULL; }
    DWORD wr = 0;
    InternetWriteFile(uh, body, (DWORD)strlen(body), &wr);
    char buf[4096];
    int cap = 8192, len = 0;
    char* resp = (char*)malloc(cap);
    if (!resp) { InternetCloseHandle(uh); InternetCloseHandle(inet); return NULL; }
    for (;;) {
        DWORD got = 0;
        if (!InternetReadFile(uh, buf, sizeof(buf), &got) || got == 0) break;
        if (len + (int)got + 1 > cap) {
            cap = (len + (int)got + 1) * 2;
            char* nb = (char*)realloc(resp, cap);
            if (!nb) { free(resp); InternetCloseHandle(uh); InternetCloseHandle(inet); return NULL; }
            resp = nb;
        }
        memcpy(resp + len, buf, got);
        len += (int)got;
    }
    InternetCloseHandle(uh);
    InternetCloseHandle(inet);
    resp[len] = 0;
    if (out_len) *out_len = len;
    return resp;
}

static int podcasts_available(void)
{
    if (g_podcasts_cached >= 0) return g_podcasts_cached;
    wchar_t core[MAX_PATH];
    GetModuleFileNameW(NULL, core, MAX_PATH);
    wchar_t* sl = wcsrchr(core, L'\\');
    if (sl) wcscpy(sl + 1, L"core_plugins\\podcasts.dll");
    if (GetFileAttributesW(core) == INVALID_FILE_ATTRIBUTES) {
        g_podcasts_cached = 0;
        return 0;
    }
    /* le service répond-il ? (plugin chargé et activé par le moteur) */
    int len = 0;
    char* r = podcast_http("GET", "/podcasts", NULL, &len);
    if (!r) { g_podcasts_cached = 0; return 0; }
    free(r);
    g_podcasts_cached = 1;
    return 1;
}
#include "config.h"
#include "cd.h"

#ifndef MP_VERSION
#define MP_VERSION "2026.08.014"
#endif
/* indirection : les arguments de ## ne sont pas expansés, d'où les 2 niveaux */
#define MP_WIDE2(x) L##x
#define MP_WIDE(x)  MP_WIDE2(x)
#define MP_VERSION_W MP_WIDE(MP_VERSION)

#define APP_TITLE L"MusicPlayer " MP_VERSION_W

/* IDs de commandes */
enum {
    IDM_OPEN = 101, IDM_OPEN_FOLDER = 103, IDM_EXIT = 102,
    IDM_OPEN_CD = 104,
    IDM_PLAYPAUSE = 201, IDM_STOP = 202, IDM_NEXT = 203,
    IDM_PREV = 204, IDM_SHUFFLE = 205,
    IDM_SPEED_BASE = 300,   /* +0 → 0.5x, +1 → 1.0x, +2 → 1.5x, +3 → 2.0x */
    IDM_VOL_UP = 401, IDM_VOL_DOWN = 402, IDM_VOL_SHOW = 403,
    IDM_PLUGIN_RELOAD = 501,
    IDM_PLUGIN_CFG = 502,   /* Settings ▸ Plugins… */
    IDM_PLUGIN_BASE = 600,  /* items plugins dynamiques */
    IDM_LANG_BASE = 700,    /* borne supérieure des items plugins */
    IDM_FULLSCREEN = 801,
    IDM_WEB_SERVER = 804,
    IDM_INTERFACE = 805,    /* Settings ▸ Interface… (skin + langue) */
    IDM_UPDATE_CFG = 806,   /* Settings ▸ Update… (mode de mise à jour) */
    IDM_DJ_MODE = 807,      /* Settings ▸ DJ Mixing (synchro web) */
    IDM_NETWORK = 809,      /* Settings ▸ Network… (services réseau) */
    IDM_REPO = 810,         /* Settings ▸ Plugin repository… */
    IDM_PODCASTS = 811,     /* File ▸ Podcasts… */
    IDM_ABOUT = 901,
    IDM_LOGS = 902        /* Help ▸ Logs… (niveau de journalisation) */
};

#define SPEED_COUNT 4
static const float SPEED_VALUES[SPEED_COUNT] = { 0.5f, 1.0f, 1.5f, 2.0f };

/* barre de contrôles (boutons + volume) */
#define CTRL_H 32
#define PROGRESS_H 16

static HWND g_hwnd = NULL;
static HWND g_status = NULL;
static wchar_t g_plugins_dir[MAX_PATH] = { 0 };
static wchar_t g_skins_dir[MAX_PATH] = { 0 };
static wchar_t g_lang_dir[MAX_PATH] = { 0 };

static int  g_fullscreen = 0;   /* mode plein écran (F11 / Échap) */
static RECT g_win_normal = { 0, 0, 640, 300 };
static int  g_fs_screens = 0;   /* nb d'écrans pour le plein écran (0 = 1) */
static int  g_fs_mode[4] = { 0, 0, 0, 0 }; /* contenu des écrans 1..4 :
                                            0 = visuel, 1 = playlist,
                                            2 = lyrics, 3 = jaquette */
static HWND g_fs_wins[4] = { NULL, NULL, NULL, NULL }; /* fenêtres annexes */
static HWND g_fs_win = NULL;    /* fenêtre principale en plein écran multi */
static int  g_fs_win_count = 0; /* nb de fenêtres annexes ouvertes */
static int  g_cd_mode = 0;        /* 1 = lecture CD audio (MCI) */
static int  g_cd_was_playing = 0; /* détection fin de piste */
static int  g_dj_mode = 0;        /* 1 = mode DJ Mixing (synchro web) */
static float g_djv_a = 1.0f, g_djv_b = 1.0f, g_djxf = 0.5f;  /* affichage DJ (valeurs côté moteur) */
static int  g_dj_track_a = -1;    /* piste choisie sur la platine A */
static int  g_dj_track_b = -1;    /* piste choisie sur la platine B */
static RECT g_dj_rc_a, g_dj_rc_b; /* platines de la console DJ locale */
static RECT g_dj_bplay_a, g_dj_bpause_a, g_dj_bstop_a; /* boutons A */
static RECT g_dj_bplay_b, g_dj_bpause_b, g_dj_bstop_b; /* boutons B */
static RECT g_dj_svol_a, g_dj_svol_b, g_dj_sxf, g_dj_spitch; /* sliders */
static int  g_dj_drag = -1;   /* slider en cours de glissement (1=A,2=B,3=xf,4=pitch) */
static RECT g_rc_play, g_rc_stop, g_rc_next, g_rc_shuffle, g_rc_plist, g_rc_fs, g_rc_vol;
static int  g_vol_drag = 0;   /* curseur de volume en cours de glissement */

static void status_update(void);        /* définie plus bas */
static void lang_pref_save(const wchar_t* code); /* idem */
static void on_command(int id, HMENU bar);       /* idem */
static void playlist_add(const wchar_t* path, int owned); /* idem */
static INT_PTR dlg_skin_color(HWND h, WPARAM w, LPARAM l); /* idem */
static void wide_to_utf8(const wchar_t* in, char* out, int out_chars); /* idem */
static void log_line(const char* s);    /* idem */
static void playlist_win_rebuild(void); /* fenêtre playlist */
static void playlist_win_highlight(void);

/* ------------------------------------------------------------------ */
/* Playlist : lecture d'un dossier (avec ses sous-dossiers)            */
/* ------------------------------------------------------------------ */
#define PLAYLIST_MAX 4096
wchar_t* g_plist[PLAYLIST_MAX];
int      g_plist_n = 0;
int      g_plist_idx = -1;
static wchar_t  g_plist_dir[MAX_PATH] = { 0 };   /* dossier de la playlist */

static void playlist_clear(void)
{
    g_cd_mode = 0;   /* la playliste fichier remplace le mode CD */
    for (int i = 0; i < g_plist_n; i++) free(g_plist[i]);
    g_plist_n = 0;
    g_plist_idx = -1;
    playlist_win_rebuild();
}

/* Ajoute une entrée à la playliste (owned = le buffer devient la propriété
 * de la playliste, sinon il est dupliqué). */
static void playlist_add(const wchar_t* path, int owned)
{
    if (g_plist_n >= PLAYLIST_MAX) return;
    g_plist[g_plist_n] = owned ? (wchar_t*)path : _wcsdup(path);
    if (g_plist[g_plist_n]) g_plist_n++;
}

static int playlist_play_index(int i)
{
    if (i < 0 || i >= g_plist_n) return -1;
    g_plist_idx = i;
    if (g_cd_mode) {
        /* CD audio : la piste i+1 via MCI (local au client) */
        cd_play(i + 1);
        status_update();
        return 0;
    }
    /* client/serveur : le moteur joue l'index de SA playlist.
     * purge du son local (effet immédiat) + état rafraîchi tout de suite */
    sp_flush();
    cc_cmd_val("playidx", i);
    cc_poll();
    playlist_win_highlight();
    return 0;
}

/* Ouvre un dossier en playlist : le moteur scanne (SA playlist), le
 * client synchronise la sienne via /api/plist. */
static int playlist_open_folder(const wchar_t* dir)
{
    wcscpy(g_plist_dir, dir);
    char utf8[MAX_PATH * 3];
    wide_to_utf8(dir, utf8, sizeof(utf8));
    cc_cmd_path("open", utf8);
    cc_plist_refresh();
    return 0;
}

/* Passe au morceau suivant ; en mode aléatoire : index au hasard ;
 * à la fin de la playlist : stop */
static int g_shuffle = 0;

void playlist_set_shuffle(int on)
{
    g_shuffle = on ? 1 : 0;
}

int playlist_get_shuffle(void)
{
    return g_shuffle;
}

/* Commande de transport : purge le son local pour un effet immédiat,
 * envoie la commande au moteur, puis rafraîchit l'état tout de suite
 * (sans attendre le timer de 250 ms, qui donnait une icône en retard).
 * flush = 0 pour les commandes qui ne doivent pas couper le son. */
static void client_transport(const char* cmd, int flush)
{
    if (flush) sp_flush();
    cc_cmd(cmd);
    cc_poll();          /* état à jour immédiatement */
    status_update();
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Lecture / pause déterministe : on décide d'après l'état connu du
 * moteur plutôt que d'envoyer une bascule. */
static void client_play_pause(void)
{
    if (cc_st() == MP_STATE_PLAYING) client_transport("pause", 1);
    else                             client_transport("play", 0);
}

static void playlist_next(void)
{
    if (g_plist_n == 0) return;
    if (g_shuffle && g_plist_n > 1) {
        int ni;
        do { ni = rand() % g_plist_n; } while (ni == g_plist_idx);
        if (playlist_play_index(ni) == 0) return;
    }
    if (g_plist_idx + 1 >= g_plist_n) {
        g_plist_idx = g_plist_n;       /* marque la fin de la playlist */
        client_transport("stop", 1);
    } else if (playlist_play_index(g_plist_idx + 1) != 0) {
        g_plist_idx++;                 /* fichier illisible : on saute */
        playlist_next();
    }
    status_update();
}

/* (l'enchaînement des morceaux est géré par le CORE — playlist_tick) */

/* Passe au morceau précédent (boucle sur la fin de la playlist) */
static void playlist_prev(void)
{
    if (g_plist_n == 0) return;
    int i = g_plist_idx - 1;
    if (i < 0) i = g_plist_n - 1;
    if (playlist_play_index(i) != 0) {
        g_plist_idx = i;
        playlist_prev();
    }
    status_update();
}

/* ------------------------------------------------------------------ */
/* Helpers UTF-8 <-> UTF-16                                            */
/* ------------------------------------------------------------------ */
static void utf8_to_wide(const char* in, wchar_t* out, int out_chars)
{
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, out_chars);
}

static void wide_to_utf8(const wchar_t* in, char* out, int out_bytes)
{
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, out_bytes, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* Journal (logs/musicplayer.log) : niveau 0 = rien, 1 = erreurs,      */
/* 2 = info, 3 = debug. Le niveau vient de la config (Help ▸ Logs…).   */
/* ------------------------------------------------------------------ */
static volatile LONG g_log_level = 2;

void mp_set_log_level(int lvl)
{
    if (lvl < 0) lvl = 0;
    if (lvl > 3) lvl = 3;
    InterlockedExchange(&g_log_level, lvl);
}

static void log_write(int level, const char* msg)
{
    if (g_log_level <= 0 || level > g_log_level) return;
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) wcscpy(slash + 1, L"logs");
    CreateDirectoryW(exe, NULL);
    if (slash) wcscpy(slash + 1, L"logs\\musicplayer.log");
    FILE* f = _wfopen(exe, L"a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02u:%02u:%02u] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
}

static void log_line(const char* msg) { log_write(2, msg); }

/* ------------------------------------------------------------------ */
/* Serveur web : configuration globale (dialog + config.yml)           */
/* ------------------------------------------------------------------ */
static int g_web_enabled = 0;
static int g_web_port = 8000;
static int g_web_audio = 0;      /* 0 = PC, 1 = téléphone, 2 = les deux */
static char g_web_ips_cfg[1024] = "";   /* IP écoutées ("ip1;ip2;..." ; vide = toutes) */

/* Premier port libre à partir de 8000 (port par défaut du serveur web) */
static int find_free_port(void)
{
    WSADATA wsa;
    int port = 8000;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 8000;
    for (int p = 8000; p < 65535; p++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) break;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((u_short)p);
        if (bind(s, (struct sockaddr*)&a, sizeof(a)) == 0) {
            port = p;
            closesocket(s);
            break;
        }
        closesocket(s);
    }
    WSACleanup();
    return port;
}

/* ------------------------------------------------------------------ */
/* API hôte exposée aux plugins                                        */
/* ------------------------------------------------------------------ */
static const char* host_file_name(void) { return cc_name(); }
static int         host_get_state(void) { return (int)cc_st(); }
static double      host_get_position(void) { return cc_pos(); }
static double      host_get_duration(void) { return cc_dur(); }
static float       host_get_volume(void) { return sp_get_volume(); }
static float       host_get_speed(void) { return cc_speed(); }
static void        host_play_pause(void) { client_play_pause(); }
static void        host_stop(void) { client_transport("stop", 1); }
static void        host_next(void) { playlist_next(); }
static void        host_set_volume(float v) { sp_set_volume(v); }
static void        host_set_speed(float s) { cc_cmd_val("speed", s); }
static void        host_set_audio_out(int m) { mp_set_audio_out(m); }
static int         host_get_audio_out(void) { return mp_get_audio_out(); }
static void        host_shuffle_toggle(void)
{ playlist_set_shuffle(!playlist_get_shuffle()); }
static int         host_get_shuffle(void) { return playlist_get_shuffle(); }

/* --- services réseau (Settings ▸ Network…) --- */
static int host_svc_port(const char* name)
{
    if (!name) return 0;
    if (!strcmp(name, "rest")) return g_cfg.svc_rest_port > 0 ? g_cfg.svc_rest_port : 8080;
    if (!strcmp(name, "upnp")) return g_cfg.svc_upnp_port > 0 ? g_cfg.svc_upnp_port : 8081;
    if (!strcmp(name, "rtp"))  return g_cfg.svc_rtp_port  > 0 ? g_cfg.svc_rtp_port  : 5004;
    if (!strcmp(name, "multiroom")) return g_cfg.svc_mr_port > 0 ? g_cfg.svc_mr_port : 5004;
    return 0;
}

static const char* host_svc_ips(const char* name)
{
    if (!name) return "";
    if (!strcmp(name, "rest")) return g_cfg.svc_rest_ips;
    if (!strcmp(name, "upnp")) return g_cfg.svc_upnp_ips;
    if (!strcmp(name, "rtp"))  return g_cfg.svc_rtp_ips;
    if (!strcmp(name, "multiroom")) return g_cfg.svc_mr_ips;
    return "";
}
static int         host_get_dj_mode(void) { return g_dj_mode; }
static void        host_dj_toggle(void)
{
    g_dj_mode = !g_dj_mode;
    if (g_dj_mode) {
        g_dj_track_a = g_plist_idx >= 0 ? g_plist_idx : 0;
        g_dj_track_b = (g_plist_idx + 1 < g_plist_n) ? g_plist_idx + 1 : 0;
        g_djv_a = 1.0f; g_djv_b = 1.0f; g_djxf = 0.5f;
        cc_cmd_val("dj_vol_a", 1.0f);
        cc_cmd_val("dj_vol_b", 1.0f);
        cc_cmd_val("dj_xf", 0.5f);
    } else {
        cc_cmd("dj_stop_b");
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
    status_update();
}
static int         host_plist_count(void) { return g_plist_n; }
static const wchar_t* host_plist_name(int i)
{
    if (i < 0 || i >= g_plist_n) return L"";
    const wchar_t* s = wcsrchr(g_plist[i], L'\\');
    return s ? s + 1 : g_plist[i];
}
static int         host_plist_index(void) { return g_plist_idx; }
static void        host_plist_play(int i) { playlist_play_index(i); }
static void*       host_main_window(void) { return g_hwnd; }
static int         host_web_enabled(void) { return g_web_enabled; }
static int         host_web_port(void) { return g_web_port; }
static int         host_web_audio(void) { return g_web_audio; }
static const char* host_web_ips(void) { return g_web_ips_cfg; }
static int         host_web_find_free_port(void) { return find_free_port(); }
static uint32_t    host_web_read(float* dst, uint32_t frames)
{ return sp_web_read(dst, frames); }

/* le client n'a pas de ring de diffusion multi-lecteurs : stubs */
static int      host_web_reader_open(void) { return 0; }
static void     host_web_reader_close(int id) { (void)id; }
static uint32_t host_web_read_n(int id, float* dst, uint32_t frames)
{ (void)id; return sp_web_read(dst, frames); }

/* ------------------------------------------------------------------ */
/* Skins : palette de couleurs (modifiée par les plugins SKIN)         */
/* ------------------------------------------------------------------ */
static mp_skin_colors g_skin = {
    RGB(255, 255, 255),  /* bg */
    RGB(30, 30, 30),     /* text */
    RGB(238, 240, 246),  /* ctrl_bar */
    RGB(210, 214, 224),  /* ctrl_sep */
    RGB(52, 120, 246),   /* accent */
    RGB(226, 66, 56),    /* accent2 */
    RGB(230, 126, 34),   /* accent3 */
    RGB(110, 118, 136),  /* neutral */
    RGB(205, 210, 222),  /* track */
    RGB(120, 126, 140),  /* mark */
    RGB(255, 255, 255),  /* knob */
    RGB(28, 30, 38),     /* prog_bg */
    RGB(92, 98, 116)     /* prog_border */
};

/* palette courante (lecture) : pour les fenêtres liées au thème */
static const mp_skin_colors* host_get_skin_colors(void) { return &g_skin; }

static ULONG_PTR g_gdiplus_token = 0;  /* GDI+ initialisé au démarrage */
static GpImage* g_skin_bg = NULL;      /* image de fond du skin */
static RECT g_skin_vis = { -1, -1, -1, -1 };  /* zone du visualiseur (skin) */
static int g_skin_menu_visible = 1;    /* barre de menus affichée ? */
static int g_skin_ctrl_top = 0;        /* contrôles en haut (skin) ? */
static int g_skin_status_visible = 1;  /* barre d'état affichée ? */
static int g_skin_win_w = 0;           /* taille client imposée par le skin */
static int g_skin_win_h = 0;
static int g_skin_win_fixed = 0;       /* fenêtre non redimensionnable ? */
static HMENU g_menu_bar = NULL;        /* barre de menus (cachable) */

/* ------------------------------------------------------------------ */
/* Barre de menus dessinée (owner-draw) avec la palette du skin        */
/* ------------------------------------------------------------------ */
static wchar_t g_bar_texts[8][64];
static int g_bar_text_n = 0;

static const wchar_t* bar_text_dup(const wchar_t* s)
{
    if (g_bar_text_n >= 8) return L"";
    wcsncpy(g_bar_texts[g_bar_text_n], s, 63);
    g_bar_texts[g_bar_text_n][63] = 0;
    return g_bar_texts[g_bar_text_n++];
}

static void append_bar_item(HMENU bar, HMENU popup, const wchar_t* text)
{
    AppendMenuW(bar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)popup,
                (LPCWSTR)bar_text_dup(text));
}

/* mesuré puis dessiné : WM_MEASUREITEM / WM_DRAWITEM (ODT_MENU) */
static void menu_measure(MEASUREITEMSTRUCT* mi)
{
    const wchar_t* t = (const wchar_t*)mi->itemData;
    HDC hdc = GetDC(g_hwnd);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(hdc, f);
    SIZE sz;
    GetTextExtentPoint32W(hdc, t, (int)wcslen(t), &sz);
    SelectObject(hdc, old);
    ReleaseDC(g_hwnd, hdc);
    mi->itemWidth = sz.cx + 28;
    mi->itemHeight = sz.cy + 10;
}

static void menu_draw(DRAWITEMSTRUCT* di)
{
    const wchar_t* t = (const wchar_t*)di->itemData;
    int hot = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH b = CreateSolidBrush(hot ? g_skin.accent : g_skin.ctrl_bar);
    FillRect(di->hDC, &di->rcItem, b);
    DeleteObject(b);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, hot ? RGB(255, 255, 255) : g_skin.text);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(di->hDC, f);
    RECT rc = di->rcItem;
    DrawTextW(di->hDC, t, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(di->hDC, old);
}

/* recrée les pinceaux de la progression (couleurs figées) */
static void skin_reset_brushes(void);
static void menu_bar_bg(void);  /* définie plus bas */

static void host_skin_set_colors(const mp_skin_colors* c)
{
    if (!c) return;
    g_skin = *c;
    skin_reset_brushes();
    if (g_hwnd) {
        InvalidateRect(g_hwnd, NULL, FALSE);
        menu_bar_bg();
        status_update();
    }
}

/* Image de fond du skin : chargée via GDI+, affichée en WM_ERASEBKGND */
static void host_skin_set_bg(const char* path_utf8)
{
    if (g_skin_bg) {
        GdipDisposeImage(g_skin_bg);
        g_skin_bg = NULL;
    }
    if (path_utf8 && path_utf8[0]) {
        wchar_t w[MAX_PATH * 3];
        MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, w, MAX_PATH * 3);
        GpStatus st = GdipLoadImageFromFile(w, &g_skin_bg);
        (void)st;
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Zone du visualiseur imposée par le skin (ex. haut-parleur de la radio) */
static void host_skin_set_visual_rect(int x, int y, int w, int h)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0) {
        /* réinitialisation : zone du visualiseur par défaut */
        g_skin_vis.left = g_skin_vis.top = -1;
        g_skin_vis.right = g_skin_vis.bottom = -1;
    } else {
        g_skin_vis.left = x;
        g_skin_vis.top = y;
        g_skin_vis.right = x + w;
        g_skin_vis.bottom = y + h;
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Barre de menus courante (fonctionne même quand elle est cachée) */
static HMENU menu_bar(void)
{
    return g_menu_bar ? g_menu_bar : GetMenu(g_hwnd);
}

/* Affiche ou cache la barre de menus selon la disposition du skin */
static void apply_menu_visibility(void)
{
    if (!g_hwnd) return;
    SetMenu(g_hwnd, g_skin_menu_visible ? g_menu_bar : NULL);
    DrawMenuBar(g_hwnd);
}

/* Disposition imposée par le skin : menu caché, contrôles en haut,
 * barre d'état masquée... */
static void host_skin_set_layout(int menu_visible, int ctrl_top,
                                 int status_visible)
{
    g_skin_menu_visible = menu_visible ? 1 : 0;
    g_skin_ctrl_top = ctrl_top ? 1 : 0;
    g_skin_status_visible = status_visible ? 1 : 0;
    if (g_hwnd) {
        apply_menu_visibility();
        if (g_status && !g_fullscreen)
            ShowWindow(g_status, g_skin_status_visible ? SW_SHOW : SW_HIDE);
        InvalidateRect(g_hwnd, NULL, FALSE);
        status_update();
    }
}

/* Taille de la zone client imposée par le skin. fixed = 1 : fenêtre non
 * redimensionnable (skin « fenêtre entière », rendu pixel-perfect). */
static void host_skin_set_window_size(int w, int h, int fixed)
{
    g_skin_win_w = w;
    g_skin_win_h = h;
    g_skin_win_fixed = (w > 0 && h > 0 && fixed) ? 1 : 0;
    if (!g_hwnd || g_fullscreen) return;

    LONG style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (g_skin_win_fixed) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    else                  style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowLongW(g_hwnd, GWL_STYLE, style);

    if (w > 0 && h > 0) {
        int ch = h;
        if (g_status && g_skin_status_visible) {
            RECT sr;
            GetWindowRect(g_status, &sr);
            ch += (sr.bottom - sr.top);
        }
        RECT wr = { 0, 0, w, ch };
        AdjustWindowRectEx(&wr, style, g_skin_menu_visible ? TRUE : FALSE,
                           GetWindowLongW(g_hwnd, GWL_EXSTYLE));
        SetWindowPos(g_hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Fond de la barre de menus avec la couleur du skin */
static void menu_bar_bg(void)
{
    HMENU m = menu_bar();
    if (!m) return;
    MENUINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = CreateSolidBrush(g_skin.ctrl_bar);
    SetMenuInfo(m, &mi);
    DrawMenuBar(g_hwnd);
}

/* ------------------------------------------------------------------ */
/* Métadonnées & jaquette (serveur web)                                */
/* ------------------------------------------------------------------ */
static const char* host_get_metadata(const char* path, const char* field)
{
    return mp_plugins_get_metadata(path, field);
}

static const wchar_t* host_plist_path(int i)
{
    if (i < 0 || i >= g_plist_n) return L"";
    return g_plist[i];
}

/* Jaquette d'un fichier : cover.* dans le dossier, sinon APIC du MP3.
 * Buffer statique : valable jusqu'au prochain appel. */
static unsigned char g_cover_buf[2 * 1024 * 1024];

static const unsigned char* host_get_cover(const char* path, size_t* len)
{
    *len = 0;
    if (!path || !path[0]) return NULL;

    /* 1) fichier d'image dans le même dossier */
    char dir[MAX_PATH * 3];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char* slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    static const char* names[] = { "cover.jpg", "folder.jpg", "cover.png", "front.jpg" };
    for (int i = 0; i < 4; i++) {
        char p[MAX_PATH * 3];
        _snprintf(p, sizeof(p), "%s%s", dir, names[i]);
        FILE* f = fopen(p, "rb");
        if (!f) continue;
        size_t n = fread(g_cover_buf, 1, sizeof(g_cover_buf), f);
        fclose(f);
        if (n > 0) { *len = n; return g_cover_buf; }
    }

    /* 2) jaquette intégrée (APIC) */
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[10];
    int found = 0;
    if (fread(h, 1, 10, f) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
        unsigned sz = ((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) |
                      ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
        long pos = 10, end = 10 + (long)sz;
        while (pos + 10 <= end) {
            unsigned char fr[10];
            if (fseek(f, pos, SEEK_SET) != 0) break;
            if (fread(fr, 1, 10, f) != 10) break;
            unsigned fsz = ((unsigned)fr[4] << 24) | ((unsigned)fr[5] << 16) |
                           ((unsigned)fr[6] << 8) | fr[7];
            if (fr[0] == 'A' && fr[1] == 'P' && fr[2] == 'I' && fr[3] == 'C') {
                if (fsz > 10 && fseek(f, pos + 10, SEEK_SET) == 0) {
                    unsigned char* buf = (unsigned char*)malloc((size_t)fsz + 1);
                    if (buf) {
                        if (fread(buf, 1, (size_t)fsz, f) == (size_t)fsz) {
                            const unsigned char* p2 = buf + 1;   /* encodage */
                            while (p2 < buf + fsz && *p2) p2++;  /* mime */
                            p2++;
                            if (p2 < buf + fsz) p2++;            /* type */
                            while (p2 < buf + fsz && *p2) p2++;  /* description */
                            p2++;
                            if (p2 < buf + fsz) {
                                long dl = (long)(buf + fsz - p2);
                                if (dl > 0 && (size_t)dl <= sizeof(g_cover_buf)) {
                                    memcpy(g_cover_buf, p2, (size_t)dl);
                                    *len = (size_t)dl;
                                    found = 1;
                                }
                            }
                        }
                        free(buf);
                    }
                }
                break;
            }
            if (fsz == 0) break;
            pos += 10 + (long)fsz;
        }
    }
    fclose(f);
    return found ? g_cover_buf : NULL;
}

static const mp_host_api g_host = {
    MP_PLUGIN_API_VERSION,
    log_line,
    host_get_state, host_get_position, host_get_duration,
    host_get_volume, host_get_speed, host_file_name,
    host_play_pause, host_stop, host_next,
    host_set_volume, host_set_speed,
    host_set_audio_out, host_get_audio_out,
    host_shuffle_toggle, host_get_shuffle,
    host_get_dj_mode, host_dj_toggle,
    host_plist_count, host_plist_name, host_plist_index, host_plist_play,
    host_main_window,
    host_web_enabled, host_web_port, host_web_audio, host_web_ips,
    host_web_find_free_port,
    host_svc_port, host_svc_ips,
    host_web_read,
    host_skin_set_colors,
    host_skin_set_bg,
    host_skin_set_visual_rect,
    host_skin_set_layout,
    host_get_metadata, host_get_cover, host_plist_path,
    host_get_skin_colors,
    host_skin_set_window_size,
    host_web_reader_open, host_web_reader_close, host_web_read_n
};

/* ------------------------------------------------------------------ */
/* Status bar                                                          */
/* ------------------------------------------------------------------ */
static DWORD WINAPI plugins_upd_thread(LPVOID arg)
{
    (void)arg;
    Sleep(4000);   /* laisser le programme démarrer tranquillement */
    int n = mp_plugins_check_updates();
    if (n > 0) {
        char msg[160];
        _snprintf(msg, sizeof(msg),
                  "%d plugin update(s) downloaded — restart to load them", n);
        log_line(msg);
    }
    return 0;
}

static void status_init(HWND hwnd)
{
    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                               WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                               0, 0, 0, 0, hwnd, NULL, GetModuleHandleW(NULL), NULL);
    int parts[4] = { 260, 130, 90, -1 };
    SendMessageW(g_status, SB_SETPARTS, 4, (LPARAM)parts);
}

static void fmt_time(wchar_t* out, size_t n, double sec)
{
    if (sec < 0) sec = 0;
    int t = (int)(sec + 0.5);
    swprintf(out, n, L"%02d:%02d", t / 60, t % 60);
}

static void status_update(void)
{
    if (!g_status) return;

    wchar_t s1[280], s2[32], s3[32], s4[48];
    const char* fn = cc_name();
    if (fn) {
        /* titre issu des métadonnées (plugin SERVICE) si disponible */
        const char* title = mp_plugins_get_title(fn);
        if (title && title[0]) {
            utf8_to_wide(title, s1, 280);
        } else {
            const char* base = strrchr(fn, '\\');
            base = base ? base + 1 : fn;
            utf8_to_wide(base, s1, 280);
        }
    } else {
        wcscpy(s1, lang_get("no_file"));
    }
    if (g_plist_n > 0 && g_plist_idx >= 0 && g_plist_idx < g_plist_n) {
        wchar_t tmp[280];
        wcscpy(tmp, s1);
        swprintf(s1, 280, L"[%d/%d] %ls", g_plist_idx + 1, g_plist_n, tmp);
    }

    double pos = cc_pos(), dur = cc_dur();
    wchar_t p[16], d[16];
    fmt_time(p, 16, pos);
    fmt_time(d, 16, dur);
    swprintf(s2, 32, L" %ls / %ls", p, d);

    swprintf(s3, 32, L" x%.1f", cc_speed());
    swprintf(s4, 48, lang_get("vol_show"), (int)(sp_get_volume() * 100.0f + 0.5f));

    SendMessageW(g_status, SB_SETTEXT, 0, (LPARAM)s1);
    SendMessageW(g_status, SB_SETTEXT, 1, (LPARAM)s2);
    SendMessageW(g_status, SB_SETTEXT, 2, (LPARAM)s3);
    SendMessageW(g_status, SB_SETTEXT, 3, (LPARAM)s4);

    /* titre + état dans la zone centrale */
    static const char* state_keys[] = { "state_stopped", "state_playing", "state_paused", "state_finished" };
    wchar_t title[320];
    swprintf(title, 320, L"%ls — %ls", APP_TITLE, lang_get(state_keys[cc_st()]));
    SetWindowTextW(g_hwnd, title);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */
/* ------------------------------------------------------------------ */
static HMENU build_speed_menu(void)
{
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < SPEED_COUNT; i++) {
        wchar_t label[16];
        swprintf(label, 16, L"x%.1f", SPEED_VALUES[i]);
        AppendMenuW(m, MF_STRING, IDM_SPEED_BASE + i, label);
    }
    return m;
}

static void refresh_speed_check(HMENU menu)
{
    float sp = cc_speed();
    int idx = 1; /* défaut x1.0 */
    for (int i = 0; i < SPEED_COUNT; i++)
        if (SPEED_VALUES[i] == sp) idx = i;
    CheckMenuRadioItem(menu, IDM_SPEED_BASE, IDM_SPEED_BASE + SPEED_COUNT - 1,
                       IDM_SPEED_BASE + idx, MF_BYCOMMAND);
}
/* Menu Plugins : sous-menus par type (un seul visuel/skin actif, radio).
 * Tous les plugins visibles (Settings ▸ Plugins…) y figurent ; la case ou
 * le point radio reflète l'état actif, et un clic ne retire rien du menu. */
static void rebuild_plugins_menu(HMENU parent)
{
    if (!parent) parent = menu_bar();
    if (!parent) return;
    HMENU mVis = CreatePopupMenu();   /* visuels : radio */
    HMENU mFX = CreatePopupMenu();    /* effets audio : cases */
    HMENU mSvc = CreatePopupMenu();   /* services : cases */

    int n = mp_plugins_count();
    int vis_active = -1;
    for (int i = 0; i < n; i++) {
        mp_plugin* p = mp_plugins_get(i);
        if (!p || !p->api || !p->visible) continue;   /* masqué : absent */
        if (p->api->type() & MP_PLUGIN_SKIN) continue; /* skins : via Interface… */
        wchar_t label[160], name_w[128], ver_w[32];
        utf8_to_wide(p->api->name(), name_w, 128);
        utf8_to_wide(p->api->version() ? p->api->version() : "?", ver_w, 32);
        swprintf(label, 160, L"%ls %ls", name_w, ver_w);

        unsigned t = p->api->type();
        HMENU target;
        if (t & MP_PLUGIN_VISUAL)   target = mVis;
        else if (t & MP_PLUGIN_AUDIO_EFFECT) target = mFX;
        else if (t & MP_PLUGIN_SERVICE)  target = mSvc;
        else                             continue;

        AppendMenuW(target, MF_STRING, IDM_PLUGIN_BASE + i, label);
        if (t & MP_PLUGIN_VISUAL) {
            if (p->enabled) vis_active = i;
        } else {
            CheckMenuItem(target, IDM_PLUGIN_BASE + i,
                          MF_BYCOMMAND | (p->enabled ? MF_CHECKED : MF_UNCHECKED));
        }
    }
    if (vis_active >= 0)
        CheckMenuRadioItem(mVis, IDM_PLUGIN_BASE, IDM_PLUGIN_BASE + n - 1,
                           IDM_PLUGIN_BASE + vis_active, MF_BYCOMMAND);

    /* reconstruit le menu Plugins (position 2) */
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_PLUGIN_RELOAD, lang_get("plugins_reload"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (n == 0) {
        AppendMenuW(m, MF_GRAYED | MF_STRING, 0, lang_get("plugins_none"));
    } else {
        if (GetMenuItemCount(mVis) > 0)
            AppendMenuW(m, MF_POPUP, (UINT_PTR)mVis, lang_get("plugins_visual"));
        else DestroyMenu(mVis);
        if (GetMenuItemCount(mFX) > 0)
            AppendMenuW(m, MF_POPUP, (UINT_PTR)mFX, lang_get("plugins_effects"));
        else DestroyMenu(mFX);
        if (GetMenuItemCount(mSvc) > 0)
            AppendMenuW(m, MF_POPUP, (UINT_PTR)mSvc, lang_get("plugins_services"));
        else DestroyMenu(mSvc);
    }
    RemoveMenu(parent, 2, MF_BYPOSITION);
    InsertMenuW(parent, 2, MF_BYPOSITION | MF_POPUP, (UINT_PTR)m, lang_get("menu_plugins"));
    DrawMenuBar(g_hwnd);
}

/* ------------------------------------------------------------------ */
static HMENU create_menus(void)
{
    HMENU bar = CreateMenu();

    HMENU mFile = CreatePopupMenu();
    AppendMenuW(mFile, MF_STRING, IDM_OPEN, lang_get("open"));
    AppendMenuW(mFile, MF_STRING, IDM_OPEN_FOLDER, lang_get("menu_open_folder"));
    AppendMenuW(mFile, MF_STRING, IDM_OPEN_CD, lang_get("menu_open_cd"));
    /* Podcasts : seulement si le plugin du moteur est présent et actif */
    if (podcasts_available())
        AppendMenuW(mFile, MF_STRING, IDM_PODCASTS, L"Podcasts…");
    AppendMenuW(mFile, MF_SEPARATOR, 0, NULL);
    /* commandes de lecture */
    AppendMenuW(mFile, MF_STRING, IDM_PLAYPAUSE, lang_get("menu_play"));
    AppendMenuW(mFile, MF_STRING, IDM_STOP, lang_get("menu_stop"));
    AppendMenuW(mFile, MF_STRING, IDM_NEXT, lang_get("menu_next"));
    AppendMenuW(mFile, MF_STRING, IDM_PREV, lang_get("menu_prev"));
    AppendMenuW(mFile, MF_STRING, IDM_SHUFFLE, lang_get("menu_shuffle"));
    AppendMenuW(mFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mFile, MF_STRING, IDM_EXIT, lang_get("quit"));
    append_bar_item(bar, mFile, lang_get("menu_file"));

    /* Paramètres : vitesse, plein écran, interface, mises à jour */
    HMENU mSettings = CreatePopupMenu();
    AppendMenuW(mSettings, MF_POPUP, (UINT_PTR)build_speed_menu(), lang_get("speed"));
    AppendMenuW(mSettings, MF_STRING, IDM_FULLSCREEN, lang_get("fullscreen"));
    AppendMenuW(mSettings, MF_STRING, IDM_DJ_MODE, lang_get("menu_dj"));
    AppendMenuW(mSettings, MF_STRING, IDM_NETWORK, lang_get("menu_net"));
    AppendMenuW(mSettings, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mSettings, MF_STRING, IDM_INTERFACE, lang_get("menu_interface"));
    AppendMenuW(mSettings, MF_STRING, IDM_UPDATE_CFG, lang_get("menu_update_cfg"));
    AppendMenuW(mSettings, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mSettings, MF_STRING, IDM_WEB_SERVER, lang_get("menu_web_server"));
    AppendMenuW(mSettings, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mSettings, MF_STRING, IDM_PLUGIN_CFG, lang_get("menu_plugins_cfg"));
    AppendMenuW(mSettings, MF_STRING, IDM_REPO, L"Plugin repository…");
    append_bar_item(bar, mSettings, lang_get("menu_settings"));

    append_bar_item(bar, (HMENU)CreatePopupMenu(), lang_get("menu_plugins"));

    HMENU mHelp = CreatePopupMenu();
    AppendMenuW(mHelp, MF_STRING, IDM_LOGS, L"Logs…");
    AppendMenuW(mHelp, MF_STRING, IDM_ABOUT, lang_get("about"));
    append_bar_item(bar, mHelp, lang_get("menu_help"));

    return bar;
}

/* Reconstruction complète de la barre de menus (changement de langue) */
static void rebuild_menus(void)
{
    HMENU old = g_menu_bar;
    g_menu_bar = create_menus();
    SetMenu(g_hwnd, g_skin_menu_visible ? g_menu_bar : NULL);
    HMENU mSettings = GetSubMenu(g_menu_bar, 1);
    refresh_speed_check(GetSubMenu(mSettings, 0));
    rebuild_plugins_menu(g_menu_bar);          /* position 2 dans la barre */
    DrawMenuBar(g_hwnd);
    if (old) DestroyMenu(old);
    mp_plugins_apply_skins(g_hwnd);
}

/* Menu contextuel (clic droit) — utilisé quand la barre est cachée */
static void show_context_menu(HWND hwnd)
{
    HMENU bar = g_menu_bar;
    if (!bar) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_POPUP, (UINT_PTR)GetSubMenu(bar, 0), lang_get("menu_file"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)GetSubMenu(bar, 1), lang_get("menu_settings"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)GetSubMenu(bar, 2), lang_get("menu_plugins"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)GetSubMenu(bar, 3), lang_get("menu_help"));
    POINT pt;
    GetCursorPos(&pt);
    int r = (int)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                pt.x, pt.y, 0, hwnd, NULL);
    /* détacher les sous-menus AVANT de détruire le popup : DestroyMenu
     * détruit aussi les sous-menus attachés, ce qui viderait la barre.
     * RemoveMenu détache sans détruire (DeleteMenu, lui, détruirait). */
    for (int i = GetMenuItemCount(m) - 1; i >= 0; i--)
        RemoveMenu(m, i, MF_BYPOSITION);
    DestroyMenu(m);
    if (r) on_command(r, menu_bar());
}

/* ------------------------------------------------------------------ */
/* Dialogues                                                           */
/* ------------------------------------------------------------------ */
/* Construction du filtre de fichiers (chaîne à double \0) */
static int add_filter(wchar_t* out, int cap, const wchar_t* s)
{
    int len = (int)wcslen(s);
    if (len + 2 > cap) return 0;
    memcpy(out, s, (size_t)len * sizeof(wchar_t));
    out[len] = 0;
    return len + 1;
}

static void build_open_filter(wchar_t* out, int out_chars)
{
    int n = 0;
    n += add_filter(out + n, out_chars - n, lang_get("filter_audio"));
    n += add_filter(out + n, out_chars - n, L"*.mp3;*.mp4");
    n += add_filter(out + n, out_chars - n, lang_get("filter_mp3"));
    n += add_filter(out + n, out_chars - n, L"*.mp3");
    n += add_filter(out + n, out_chars - n, lang_get("filter_mp4"));
    n += add_filter(out + n, out_chars - n, L"*.mp4");
    n += add_filter(out + n, out_chars - n, lang_get("filter_all"));
    n += add_filter(out + n, out_chars - n, L"*.*");
    if (n + 1 < out_chars) out[n] = 0;   /* double \0 final */
}

static void do_open_dialog(void)
{
    wchar_t path_w[MAX_PATH] = { 0 };
    wchar_t filter[512] = { 0 };
    build_open_filter(filter, 512);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path_w;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = lang_get("open_title");

    if (GetOpenFileNameW(&ofn)) {
        char path_utf8[MAX_PATH * 3];
        wide_to_utf8(path_w, path_utf8, sizeof(path_utf8));
        if (cc_open(path_utf8) != 0) {
            wchar_t msg[600];
            swprintf(msg, 600, lang_get("err_open"), path_w);
            MessageBoxW(g_hwnd, msg, APP_TITLE, MB_ICONERROR);
        }
    }
}

/* Ouvre un CD audio : les pistes remplacent la playliste (mode CD). */
static void do_open_cd(void)
{
    if (!cd_open()) {
        log_line("CD: no CD drive available");
        return;
    }
    int n = cd_track_count();
    if (n <= 0) {
        log_line("CD: no audio disc in the drive");
        return;
    }
    playlist_clear();
    g_cd_mode = 1;
    for (int i = 0; i < n; i++) {
        wchar_t name[32];
        wsprintfW(name, L"CD Track %d", i + 1);
        wchar_t* copy = (wchar_t*)malloc((wcslen(name) + 1) * sizeof(wchar_t));
        if (!copy) break;
        wcscpy(copy, name);
        playlist_add(copy, 1);
    }
    g_plist_idx = 0;
    cd_play(1);
    playlist_win_rebuild();
    status_update();
    log_line("CD: audio disc opened");
}

static void do_open_folder_dialog(void)
{
    /* Sélecteur de dossier classique (le plus stable : fonctionne sous
     * Windows 11 et Wine). COM initialisé pour la stabilité du shell. */
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = lang_get("open_folder_title");
    bi.ulFlags = BIF_RETURNONLYFSDIRS;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t dir[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, dir)) {
            if (playlist_open_folder(dir) != 0) {
                wchar_t msg[600];
                swprintf(msg, 600, lang_get("err_folder"), dir);
                MessageBoxW(g_hwnd, msg, APP_TITLE, MB_ICONERROR);
            }
        }
        CoTaskMemFree(pidl);
    }
    if (hr == S_OK || hr == S_FALSE) CoUninitialize();
}

/* ------------------------------------------------------------------ */
/* Serveur web : configuration (activé, port, sortie audio)            */
/* ------------------------------------------------------------------ */
#define IDD_WEB         104
#define IDD_PLUGINS     105
#define IDD_UPDATE      106
#define IDD_ABOUT       107
#define IDD_INTERFACE   108
#define IDD_UPDATE_CFG  109
#define IDC_WEB_CHK     2001
#define IDC_WEB_EDIT    2002
#define IDC_WEB_COMBO   2003
#define IDC_WEB_LIST    2004
#define IDC_WEB_PORT_LBL 1001
#define IDC_WEB_AUD_LBL 1002
#define IDC_WEB_LIST_LBL 1003
#define IDC_PLG_LBL     1004
#define IDC_UPD_LBL     1005
#define IDC_ABT_L1      1006
#define IDC_ABT_L2      1007
#define IDC_PLG_LIST    2005
static int g_engine_plugins_start = 0;   /* 1ère ligne "engine" du dialog Plugins */
static char g_engine_files[64][128];    /* noms de fichiers des plugins moteur */
#define IDC_IF_SKIN     1013   /* dialog Interface : combo skin */
#define IDC_IF_LANG     1014   /* dialog Interface : combo langue */
#define IDC_IF_LBL_S    1015
#define IDC_IF_LBL_L    1016
#define IDC_UPD_GRP     1017   /* dialog Update : groupe mode */
#define IDC_UPD_AUTO    1018
#define IDC_UPD_MAN     1019
#define IDC_UPD_OFF     1020
#define IDC_UPD_CHECK   1021

/* liste des interfaces réseau (pour le dialog) */
typedef struct {
    char ip[64];
    wchar_t name[128];
} web_ip_t;
static web_ip_t g_web_ip_list[32];
static int g_web_ip_count = 0;

/* Énumère les adresses IPv4 de la machine (+ loopback) */
static void web_enum_ips(void)
{
    g_web_ip_count = 0;
    ULONG buflen = 0;
    if (GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &buflen) != ERROR_BUFFER_OVERFLOW || buflen == 0)
        buflen = 16384;
    IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)malloc(buflen);
    if (!aa) return;
    if (GetAdaptersAddresses(AF_INET, 0, NULL, aa, &buflen) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = aa; a && g_web_ip_count < 31; a = a->Next) {
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress;
                 u && g_web_ip_count < 31; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                struct sockaddr_in* si = (struct sockaddr_in*)u->Address.lpSockaddr;
                unsigned long v = ntohl(si->sin_addr.s_addr);
                char ip[64];
                _snprintf(ip, sizeof(ip), "%lu.%lu.%lu.%lu",
                          (v >> 24) & 0xff, (v >> 16) & 0xff,
                          (v >> 8) & 0xff, v & 0xff);
                int dup = 0;
                for (int i = 0; i < g_web_ip_count; i++)
                    if (!strcmp(g_web_ip_list[i].ip, ip)) { dup = 1; break; }
                if (dup) continue;
                _snprintf(g_web_ip_list[g_web_ip_count].ip, 64, "%s", ip);
                if (a->FriendlyName)
                    wcsncpy(g_web_ip_list[g_web_ip_count].name, a->FriendlyName, 127);
                else
                    g_web_ip_list[g_web_ip_count].name[0] = 0;
                g_web_ip_list[g_web_ip_count].name[127] = 0;
                g_web_ip_count++;
            }
        }
    }
    free(aa);
    /* boucle locale */
    if (g_web_ip_count < 32) {
        strcpy(g_web_ip_list[g_web_ip_count].ip, "127.0.0.1");
        wcscpy(g_web_ip_list[g_web_ip_count].name, L"Loopback");
        g_web_ip_count++;
    }
}

/* Une IP de la config est-elle cochée ? (défaut : tout coché) */
static int web_ip_checked(const char* ip)
{
    if (g_web_ips_cfg[0] == 0) return 1;
    char tmp[1024];
    strncpy(tmp, g_web_ips_cfg, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char* tok = strtok(tmp, ";");
    while (tok) {
        if (!strcmp(tok, ip)) return 1;
        tok = strtok(NULL, ";");
    }
    return 0;
}

static void web_load_config(void)
{
    g_web_enabled = g_cfg.web_enabled;
    g_web_port = g_cfg.web_port > 0 ? g_cfg.web_port : find_free_port();
    g_web_audio = g_cfg.web_audio;
    strncpy(g_web_ips_cfg, g_cfg.web_ips, sizeof(g_web_ips_cfg) - 1);
    g_web_ips_cfg[sizeof(g_web_ips_cfg) - 1] = 0;
}

static void web_save_config(void)
{
    g_cfg.web_enabled = g_web_enabled;
    g_cfg.web_port = g_web_port;
    g_cfg.web_audio = g_web_audio;
    strncpy(g_cfg.web_ips, g_web_ips_cfg, sizeof(g_cfg.web_ips) - 1);
    g_cfg.web_ips[sizeof(g_cfg.web_ips) - 1] = 0;
    config_save();
}

static void web_apply(void)
{
    mp_set_audio_out(g_web_audio);
    /* le serveur web est un plugin SERVICE : reconfiguration */
    mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL);
    cc_push_web_config(g_cfg.web_enabled, g_cfg.web_port, g_cfg.web_ips);
}

static INT_PTR CALLBACK web_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        CheckDlgButton(h, IDC_WEB_CHK, g_web_enabled ? BST_CHECKED : BST_UNCHECKED);
        wchar_t ptxt[16];
        swprintf(ptxt, 16, L"%d", g_web_port);
        SetDlgItemTextW(h, IDC_WEB_EDIT, ptxt);
        HWND cb = GetDlgItem(h, IDC_WEB_COMBO);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)lang_get("web_pc"));
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)lang_get("web_phone"));
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)lang_get("web_both"));
        SendMessageW(cb, CB_SETCURSEL, g_web_audio, 0);
        /* liste des interfaces : sur quelle IP écouter */
        HWND lv = GetDlgItem(h, IDC_WEB_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMNW c0, c1;
        memset(&c0, 0, sizeof(c0));
        c0.mask = LVCF_TEXT | LVCF_WIDTH;
        c0.cx = 118; c0.pszText = L"IP";
        ListView_InsertColumn(lv, 0, &c0);
        memset(&c1, 0, sizeof(c1));
        c1.mask = LVCF_TEXT | LVCF_WIDTH;
        c1.cx = 142; c1.pszText = L"Interface";
        ListView_InsertColumn(lv, 1, &c1);
        web_enum_ips();
        for (int i = 0; i < g_web_ip_count; i++) {
            LVITEMW it;
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_TEXT;
            it.iItem = i;
            wchar_t ipw[64];
            MultiByteToWideChar(CP_UTF8, 0, g_web_ip_list[i].ip, -1, ipw, 64);
            it.pszText = ipw;
            ListView_InsertItem(lv, &it);
            ListView_SetItemText(lv, i, 1, g_web_ip_list[i].name);
            ListView_SetCheckState(lv, i, web_ip_checked(g_web_ip_list[i].ip));
        }
        SetDlgItemTextW(h, IDC_WEB_CHK, lang_get("web_enable"));
        SetDlgItemTextW(h, IDC_WEB_PORT_LBL, lang_get("web_port"));
        SetDlgItemTextW(h, IDC_WEB_AUD_LBL, lang_get("web_audio_out"));
        SetDlgItemTextW(h, IDC_WEB_LIST_LBL, lang_get("web_listen"));
        SetDlgItemTextW(h, IDOK, lang_get("web_ok"));
        SetDlgItemTextW(h, IDCANCEL, lang_get("web_cancel"));
        SetWindowTextW(h, lang_get("web_dlg_title"));
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            int on = IsDlgButtonChecked(h, IDC_WEB_CHK) == BST_CHECKED;
            wchar_t ptxt[32];
            GetDlgItemTextW(h, IDC_WEB_EDIT, ptxt, 32);
            int port = _wtoi(ptxt);
            int audio = (int)SendMessageW(GetDlgItem(h, IDC_WEB_COMBO),
                                          CB_GETCURSEL, 0, 0);
            if (audio < 0) audio = 0;
            /* IP cochées : "ip1;ip2;..." (vide = toutes) */
            char ips[1024] = "";
            HWND lv = GetDlgItem(h, IDC_WEB_LIST);
            for (int i = 0; i < g_web_ip_count; i++) {
                if (ListView_GetCheckState(lv, i)) {
                    if (ips[0]) strcat(ips, ";");
                    strcat(ips, g_web_ip_list[i].ip);
                }
            }
            g_web_enabled = on;
            g_web_port = (port >= 1 && port <= 65535) ? port : 8000;
            g_web_audio = audio;
            strncpy(g_web_ips_cfg, ips, sizeof(g_web_ips_cfg) - 1);
            g_web_ips_cfg[sizeof(g_web_ips_cfg) - 1] = 0;
            web_save_config();
            web_apply();
            EndDialog(h, 1);
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

static void do_web_dialog(void)
{
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_WEB),
                    g_hwnd, web_dlg_proc, 0);
}

/* ------------------------------------------------------------------ */
/* Dialog Plugins (Settings ▸ Plugins…) : visibilité dans le menu      */
/* (les skins se choisissent uniquement dans le menu Plugins ▸ Skins)  */
/* ------------------------------------------------------------------ */
static HBRUSH g_dlg_brush = NULL;

/* fond de dialog et textes avec la palette du skin */
static INT_PTR dlg_skin_color(HWND h, WPARAM w, LPARAM l)
{
    (void)h; (void)l;
    HDC hdc = (HDC)w;
    if (!g_dlg_brush) g_dlg_brush = CreateSolidBrush(g_skin.bg);
    SetBkColor(hdc, g_skin.bg);
    SetTextColor(hdc, g_skin.text);
    return (INT_PTR)g_dlg_brush;
}

/* Recharge la liste : plugins locaux puis plugins du moteur (engine) */
static void plugins_refresh(HWND h)
{
    HWND lv = GetDlgItem(h, IDC_PLG_LIST);
    ListView_DeleteAllItems(lv);
    int n = mp_plugins_count();
    int row = 0;
    for (int i = 0; i < n; i++) {
        mp_plugin* p = mp_plugins_get(i);
        if (!p || !p->api) continue;
        if (p->api->type() & MP_PLUGIN_SKIN) continue;  /* skins : menu uniquement */
        wchar_t name_w[160], desc_w[240];
        utf8_to_wide(p->api->name() ? p->api->name() : "?", name_w, 160);
        utf8_to_wide(p->api->description() ? p->api->description() : "",
                     desc_w, 240);
        const wchar_t* type_w = L"";
        unsigned t = p->api->type();
        if (t & MP_PLUGIN_VISUAL) type_w = L"Visual";
        else if (t & MP_PLUGIN_AUDIO_EFFECT) type_w = L"Audio effect";
        else if (t & MP_PLUGIN_SERVICE) type_w = L"Service";
        LVITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = name_w;
        ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, row, 1, (wchar_t*)type_w);
        ListView_SetItemText(lv, row, 2, desc_w);
        wchar_t ver_w[48];
        utf8_to_wide(p->api->version() ? p->api->version() : "", ver_w, 48);
        ListView_SetItemText(lv, row, 3, ver_w);
        ListView_SetCheckState(lv, row, p->visible ? TRUE : FALSE);
        row++;
    }
    /* plugins du MOTEUR (core_plugins/) : GET /api/plugins, affichés
     * avec la mention (engine) — case en lecture seule */
    g_engine_plugins_start = row;
    {
        int elen = 0;
        char* eresp = engine_http_get("/api/plugins", &elen);
        if (eresp) {
            int cnt = 0;
            const char* pp = eresp;
            while ((pp = strstr(pp, "\"name\":")) != NULL) { cnt++; pp += 7; }
            pp = eresp;
            for (int i = 0; i < cnt; i++) {
                const char* obj = strstr(pp, "{\"name\":");
                if (!obj) break;
                const char* end = strchr(obj, '}');
                if (!end) break;
                int olen = (int)(end - obj) + 1;
                char tmp[2048];
                if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
                memcpy(tmp, obj, olen);
                tmp[olen] = 0;
                char nm[160], ty[64], ds[240], en[8], fl[128], ve[48];
                pod_json_str(tmp, "name", nm, sizeof(nm));
                pod_json_str(tmp, "type", ty, sizeof(ty));
                pod_json_str(tmp, "desc", ds, sizeof(ds));
                pod_json_str(tmp, "enabled", en, sizeof(en));
                pod_json_str(tmp, "file", fl, sizeof(fl));
                pod_json_str(tmp, "version", ve, sizeof(ve));
                pod_json_unescape(nm);
                pod_json_unescape(ds);
                wchar_t name_w[200], desc_w[280], type_w[80];
                utf8_to_wide(nm, name_w, 200);
                swprintf(desc_w, 280, L"%hs (engine)", ds);
                utf8_to_wide(ty, type_w, 80);
                LVITEMW it;
                memset(&it, 0, sizeof(it));
                it.mask = LVIF_TEXT;
                it.iItem = row;
                it.pszText = name_w;
                ListView_InsertItem(lv, &it);
                ListView_SetItemText(lv, row, 1, type_w);
                ListView_SetItemText(lv, row, 2, desc_w);
                wchar_t ver_w[48];
                utf8_to_wide(ve, ver_w, 48);
                ListView_SetItemText(lv, row, 3, ver_w);
                ListView_SetCheckState(lv, row, atoi(en) ? TRUE : FALSE);
                if (row - g_engine_plugins_start < 64)
                    _snprintf(g_engine_files[row - g_engine_plugins_start],
                              sizeof(g_engine_files[0]), "%s", fl);
                row++;
                pp = end + 1;
            }
            free(eresp);
        }
    }
}

static INT_PTR CALLBACK plugins_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        HWND lv = GetDlgItem(h, IDC_PLG_LIST);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMNW c0, c1, c2, c3;
        memset(&c0, 0, sizeof(c0));
        c0.mask = LVCF_TEXT | LVCF_WIDTH;
        c0.cx = 150; c0.pszText = L"Plugin";
        ListView_InsertColumn(lv, 0, &c0);
        memset(&c1, 0, sizeof(c1));
        c1.mask = LVCF_TEXT | LVCF_WIDTH;
        c1.cx = 90; c1.pszText = L"Type";
        ListView_InsertColumn(lv, 1, &c1);
        memset(&c2, 0, sizeof(c2));
        c2.mask = LVCF_TEXT | LVCF_WIDTH;
        c2.cx = 80; c2.pszText = L"Description";
        ListView_InsertColumn(lv, 2, &c2);
        memset(&c3, 0, sizeof(c3));
        c3.mask = LVCF_TEXT | LVCF_WIDTH;
        c3.cx = 60; c3.pszText = L"Version";
        ListView_InsertColumn(lv, 3, &c3);
        plugins_refresh(h);
        SetDlgItemTextW(h, IDC_PLG_LBL, lang_get("plugins_dlg_lbl"));
        SetWindowTextW(h, lang_get("plugins_dlg_title"));
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)l;
        if (nm->idFrom == IDC_PLG_LIST && nm->code == LVN_ITEMCHANGING) {
            NMLISTVIEW* nv = (NMLISTVIEW*)l;
            /* seule la case à cocher des plugins moteur est verrouillée :
             * la SÉLECTION (et donc le bouton Delete) reste possible */
            if (nv->iItem >= g_engine_plugins_start &&
                (nv->uNewState & LVIS_STATEIMAGEMASK)) {
                SetWindowLongPtrW(h, DWLP_MSGRESULT, TRUE);
                return TRUE;
            }
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 2006) {
            /* Delete selected... : plugin local ou moteur */
            HWND lv = GetDlgItem(h, IDC_PLG_LIST);
            int sel = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1,
                                        LVNI_SELECTED);
            if (sel < 0) break;
            if (sel < g_engine_plugins_start) {
                mp_plugin* p = mp_plugins_get(sel);
                if (!p) break;
                /* le skin actif ne peut pas être supprimé */
                if ((p->api->type() & MP_PLUGIN_SKIN) && p->enabled) {
                    MessageBoxW(h, L"This skin is active and cannot be deleted.",
                                L"Plugins", MB_ICONWARNING);
                    break;
                }
                wchar_t name_w[160];
                utf8_to_wide(p->api->name() ? p->api->name() : "?", name_w, 160);
                wchar_t msg[320];
                swprintf(msg, 320,
                         L"Delete plugin \"%ls\"?\nThe DLL file will be removed.",
                         name_w);
                if (MessageBoxW(h, msg, L"Plugins",
                                MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    wchar_t dll[MAX_PATH];
                    wcscpy(dll, p->path);
                    mp_plugins_unload(sel);
                    if (!DeleteFileW(dll)) {
                        MessageBoxW(h, L"File is locked — it will be removed "
                                        L"on next startup.",
                                    L"Plugins", MB_ICONWARNING);
                    }
                    plugins_refresh(h);
                    rebuild_menus();
                }
            } else {
                /* plugin du moteur : POST /api/plugins/del */
                int eidx = sel - g_engine_plugins_start;
                if (eidx < 0 || eidx >= 64 || !g_engine_files[eidx][0]) break;
                wchar_t msg[320];
                swprintf(msg, 320,
                         L"Delete engine plugin \"%hs\"?\nThe DLL file will be "
                         L"removed (core_plugins).", g_engine_files[eidx]);
                if (MessageBoxW(h, msg, L"Plugins",
                                MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    char body[192];
                    snprintf(body, sizeof(body),
                             "{\"file\":\"%s\"}", g_engine_files[eidx]);
                    int elen = 0;
                    char* r = engine_http_post("/api/plugins/del", body, &elen);
                    if (r) {
                        if (strstr(r, "\"error\":\"protected\""))
                            MessageBoxW(h,
                                L"This plugin is required and cannot be deleted.",
                                L"Plugins", MB_ICONWARNING);
                        else if (strstr(r, "\"ok\":1"))
                            plugins_refresh(h);
                        free(r);
                    }
                    g_podcasts_cached = -1;
                    rebuild_menus();
                }
            }
        } else if (LOWORD(w) == IDOK) {
            HWND lv = GetDlgItem(h, IDC_PLG_LIST);
            int n = mp_plugins_count();
            int row = 0;
            for (int i = 0; i < n; i++) {
                mp_plugin* p = mp_plugins_get(i);
                if (!p || !p->api) continue;
                if (p->api->type() & MP_PLUGIN_SKIN) continue;
                mp_plugins_set_visible(i, ListView_GetCheckState(lv, row) ? 1 : 0);
                row++;
            }
            /* reconfigurer les services (serveur web…) */
            mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL);
    cc_push_web_config(g_cfg.web_enabled, g_cfg.web_port, g_cfg.web_ips);
            rebuild_menus();
            EndDialog(h, 1);
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    }
    return FALSE;
}

static void do_plugins_dialog(void)
{
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_PLUGINS),
                    g_hwnd, plugins_dlg_proc, 0);
}

/* ------------------------------------------------------------------ */
/* Dialog Network services (Settings ▸ Network…) : port + IPs          */
/* ------------------------------------------------------------------ */
#define IDC_NET_COMBO 1051
#define IDC_NET_PORT  1053
#define IDC_NET_LIST  1055

static const wchar_t* net_svc_names[] = {
    L"REST API", L"DLNA/UPnP", L"RTP/AES67", L"Multiroom"
};
static int* net_svc_ports[] = {
    &g_cfg.svc_rest_port, &g_cfg.svc_upnp_port,
    &g_cfg.svc_rtp_port, &g_cfg.svc_mr_port
};
static char* net_svc_ipss[] = {
    g_cfg.svc_rest_ips, g_cfg.svc_upnp_ips,
    g_cfg.svc_rtp_ips, g_cfg.svc_mr_ips
};
static const int net_svc_defports[] = { 8080, 8081, 5004, 5004 };
static int g_net_cur = 0;

/* une IP est-elle dans la liste "ip1;ip2;..." ? (vide = toutes) */
static int web_ip_in(const char* ips, const char* ip)
{
    if (!ips || !ips[0]) return 1;
    char tmp[1024];
    _snprintf(tmp, sizeof(tmp), "%s", ips);
    char* tok = strtok(tmp, ";");
    while (tok) {
        if (!strcmp(tok, ip)) return 1;
        tok = strtok(NULL, ";");
    }
    return 0;
}

static void net_load(int idx, HWND h)
{
    int port = *net_svc_ports[idx];
    if (port <= 0) port = net_svc_defports[idx];
    wchar_t ptxt[16];
    wsprintfW(ptxt, L"%d", port);
    SetDlgItemTextW(h, IDC_NET_PORT, ptxt);
    HWND lv = GetDlgItem(h, IDC_NET_LIST);
    for (int i = 0; i < g_web_ip_count; i++)
        ListView_SetCheckState(lv, i,
            web_ip_in(net_svc_ipss[idx], g_web_ip_list[i].ip));
}

static INT_PTR CALLBACK net_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        HWND cb = GetDlgItem(h, IDC_NET_COMBO);
        for (int i = 0; i < 4; i++)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)net_svc_names[i]);
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
        HWND lv = GetDlgItem(h, IDC_NET_LIST);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMNW c0, c1;
        memset(&c0, 0, sizeof(c0));
        c0.mask = LVCF_TEXT | LVCF_WIDTH;
        c0.cx = 118; c0.pszText = L"IP";
        ListView_InsertColumn(lv, 0, &c0);
        memset(&c1, 0, sizeof(c1));
        c1.mask = LVCF_TEXT | LVCF_WIDTH;
        c1.cx = 142; c1.pszText = L"Interface";
        ListView_InsertColumn(lv, 1, &c1);
        web_enum_ips();
        for (int i = 0; i < g_web_ip_count; i++) {
            LVITEMW it;
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_TEXT;
            it.iItem = i;
            wchar_t ipw[64];
            MultiByteToWideChar(CP_UTF8, 0, g_web_ip_list[i].ip, -1, ipw, 64);
            it.pszText = ipw;
            ListView_InsertItem(lv, &it);
            ListView_SetItemText(lv, i, 1, g_web_ip_list[i].name);
        }
        g_net_cur = 0;
        net_load(0, h);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDC_NET_COMBO && HIWORD(w) == CBN_SELCHANGE) {
            g_net_cur = (int)SendMessageW(GetDlgItem(h, IDC_NET_COMBO),
                                          CB_GETCURSEL, 0, 0);
            if (g_net_cur < 0) g_net_cur = 0;
            net_load(g_net_cur, h);
            return TRUE;
        }
        if (LOWORD(w) == IDOK) {
            wchar_t ptxt[32];
            GetDlgItemTextW(h, IDC_NET_PORT, ptxt, 32);
            int port = _wtoi(ptxt);
            char ips[1024] = "";
            HWND lv = GetDlgItem(h, IDC_NET_LIST);
            int n = 0;
            for (int i = 0; i < g_web_ip_count; i++) {
                if (ListView_GetCheckState(lv, i))
                    n += _snprintf(ips + n, sizeof(ips) - n, "%s%s",
                                   n ? ";" : "", g_web_ip_list[i].ip);
            }
            *net_svc_ports[g_net_cur] = port;
            _snprintf(net_svc_ipss[g_net_cur], 1024, "%s", ips);
            config_save();
            /* reconfigurer les services réseau */
            mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL);
    cc_push_web_config(g_cfg.web_enabled, g_cfg.web_port, g_cfg.web_ips);
            EndDialog(h, 1);
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

static void do_net_dialog(void)
{
    DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(111), g_hwnd,
               net_dlg_proc);
}

/* ------------------------------------------------------------------ */
/* Dialog Interface (Settings ▸ Interface…) : skin + langue            */
/* ------------------------------------------------------------------ */

/* Boutons uniques : le texte reflète l'état courant (pas de double
 * bouton Enable/Disable — un seul, dont le libellé change). */
static void refresh_svc_buttons(HWND h)
{
    SetDlgItemTextW(h, 1044, svc_installed() == 1 ? L"Disable autostart"
                                                  : L"Enable autostart");
    SetDlgItemTextW(h, 1046, svc_running() == 1 ? L"Stop engine"
                                                : L"Start engine now");
}

static INT_PTR CALLBACK interface_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        SetDlgItemTextW(h, IDC_IF_LBL_S, lang_get("interface_skin"));
        SetDlgItemTextW(h, IDC_IF_LBL_L, lang_get("interface_lang"));
        HWND cs = GetDlgItem(h, IDC_IF_SKIN);
        SendMessageW(cs, CB_ADDSTRING, 0, (LPARAM)L"Default");
        int sel = 0;
        int n = mp_plugins_count();
        for (int i = 0; i < n; i++) {
            mp_plugin* p = mp_plugins_get(i);
            if (!p || !p->api || !(p->api->type() & MP_PLUGIN_SKIN)) continue;
            wchar_t name_w[128];
            utf8_to_wide(p->api->name(), name_w, 128);
            int idx = (int)SendMessageW(cs, CB_ADDSTRING, 0, (LPARAM)name_w);
            if (p->enabled) sel = idx;
        }
        SendMessageW(cs, CB_SETCURSEL, sel, 0);
        HWND cl = GetDlgItem(h, IDC_IF_LANG);
        int nlang = 0;
        const lang_info* li = lang_list(&nlang);
        int lsel = 0;
        for (int i = 0; i < nlang; i++) {
            int idx = (int)SendMessageW(cl, CB_ADDSTRING, 0, (LPARAM)li[i].name);
            if (wcscmp(li[i].code, lang_code()) == 0) lsel = idx;
        }
        SendMessageW(cl, CB_SETCURSEL, lsel, 0);

        /* plein écran multi-écrans */
        {
            int nmons = GetSystemMetrics(SM_CMONITORS);
            if (nmons > 4) nmons = 4;
            wchar_t lbl[160];
            wsprintfW(lbl, L"(%d screen%s detected)", nmons,
                      nmons > 1 ? L"s" : L"");
            SetDlgItemTextW(h, 1037, lbl);
            HWND fsc = GetDlgItem(h, 1040);
            static const wchar_t* fsopts[] = {
                L"Off", L"1", L"2", L"3", L"4"
            };
            for (int i = 0; i < 5; i++)
                SendMessageW(fsc, CB_ADDSTRING, 0, (LPARAM)fsopts[i]);
            int fs = g_fs_screens;
            if (fs < 0) fs = 0;
            if (fs > 4) fs = 4;
            SendMessageW(fsc, CB_SETCURSEL, fs, 0);
            static const wchar_t* fmodes[] = {
                L"Visual effect", L"Playlist", L"Lyrics", L"Cover"
            };
            for (int i = 0; i < 3; i++) {
                HWND lbl2 = GetDlgItem(h, 1034 + i);
                HWND m = GetDlgItem(h, 1041 + i);
                if (i < nmons) {
                    wchar_t t[64];
                    wsprintfW(t, L"Screen %d shows :", i + 1);
                    SetWindowTextW(lbl2, t);
                    ShowWindow(lbl2, SW_SHOW);
                    ShowWindow(m, SW_SHOW);
                    for (int j = 0; j < 4; j++)
                        SendMessageW(m, CB_ADDSTRING, 0, (LPARAM)fmodes[j]);
                    SendMessageW(m, CB_SETCURSEL, g_fs_mode[i], 0);
                } else {
                    ShowWindow(lbl2, SW_HIDE);
                    ShowWindow(m, SW_HIDE);
                }
            }
        }
        /* boutons uniques (texte selon l'état) */
        refresh_svc_buttons(h);
        /* le groupe Full screen s'adapte au nombre d'écrans : pas
         * d'espace vide sous les rangées masquées */
        {
            int nmons = GetSystemMetrics(SM_CMONITORS);
            if (nmons > 4) nmons = 4;
            if (nmons < 1) nmons = 1;
            int extra = (4 - nmons) * 26;
            if (extra > 0) {
                RECT r;
                HWND fs = GetDlgItem(h, 1032);
                GetWindowRect(fs, &r);
                MapWindowPoints(NULL, h, (LPPOINT)&r, 2);
                MoveWindow(fs, r.left, r.top, r.right - r.left,
                           (r.bottom - r.top) - extra, TRUE);
                HWND ctrls[] = { GetDlgItem(h, 1050), GetDlgItem(h, 1044),
                                 GetDlgItem(h, 1046), GetDlgItem(h, IDOK),
                                 GetDlgItem(h, IDCANCEL) };
                for (int i = 0; i < 5; i++) {
                    GetWindowRect(ctrls[i], &r);
                    MapWindowPoints(NULL, h, (LPPOINT)&r, 2);
                    MoveWindow(ctrls[i], r.left, r.top - extra,
                               r.right - r.left, r.bottom - r.top, TRUE);
                }
                RECT dr;
                GetWindowRect(h, &dr);
                SetWindowPos(h, NULL, 0, 0, dr.right - dr.left,
                             dr.bottom - dr.top - extra,
                             SWP_NOMOVE | SWP_NOZORDER);
            }
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            /* skin choisi (0 = palette par défaut) */
            int sel = (int)SendMessageW(GetDlgItem(h, IDC_IF_SKIN),
                                        CB_GETCURSEL, 0, 0);
            int skin_i = -1;
            int k = 0;
            int n = mp_plugins_count();
            for (int i = 0; i < n; i++) {
                mp_plugin* p = mp_plugins_get(i);
                if (!p || !p->api || !(p->api->type() & MP_PLUGIN_SKIN)) continue;
                k++;
                if (k == sel) { skin_i = i; break; }
            }
            for (int j = 0; j < n; j++) {
                mp_plugin* q = mp_plugins_get(j);
                if (q && q->api && (q->api->type() & MP_PLUGIN_SKIN))
                    mp_plugins_set_enabled(j, j == skin_i);
            }
            mp_plugins_apply_skins(g_hwnd);
            /* langue choisie */
            int lsel = (int)SendMessageW(GetDlgItem(h, IDC_IF_LANG),
                                         CB_GETCURSEL, 0, 0);
            int nlang = 0;
            const lang_info* li = lang_list(&nlang);
            if (lsel >= 0 && lsel < nlang) {
                if (lang_set(li[lsel].code) == 0) {
                    lang_pref_save(li[lsel].code);
                    rebuild_menus();
                }
            }
            /* plein écran multi-écrans */
            g_fs_screens = (int)SendMessageW(GetDlgItem(h, 1040),
                                             CB_GETCURSEL, 0, 0);
            for (int i = 0; i < 3; i++)
                g_fs_mode[i] = (int)SendMessageW(GetDlgItem(h, 1041 + i),
                                                 CB_GETCURSEL, 0, 0);
            g_cfg.fs_screens = g_fs_screens;
            g_cfg.fs_mode1 = g_fs_mode[0];
            g_cfg.fs_mode2 = g_fs_mode[1];
            g_cfg.fs_mode3 = g_fs_mode[2];
            config_save();
            EndDialog(h, 1);
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        } else if (LOWORD(w) == 1044) {   /* Enable/Disable autostart (un seul bouton) */
            if (svc_installed() == 1) {
                int rc = svc_uninstall();
                if (rc == 0)      MessageBoxW(h, L"Autostart disabled.", L"Start with Windows", MB_OK);
                else if (rc == 1) MessageBoxW(h, L"Autostart was not enabled.", L"Start with Windows", MB_OK);
                else              MessageBoxW(h, L"Disable failed.", L"Start with Windows", MB_ICONERROR);
            } else {
                int rc = svc_install();
                if (rc == 0)      MessageBoxW(h, L"Autostart enabled: the engine will start at your next login.", L"Start with Windows", MB_OK);
                else if (rc == 1) MessageBoxW(h, L"Autostart already enabled.", L"Start with Windows", MB_OK);
                else              MessageBoxW(h, L"Enable failed.", L"Start with Windows", MB_ICONERROR);
            }
            refresh_svc_buttons(h);
        } else if (LOWORD(w) == 1046) {   /* Start/Stop engine (un seul bouton) */
            if (svc_running() == 1) {
                int rc = svc_stop();
                if (rc == 0)      MessageBoxW(h, L"Engine stopped.", L"Start with Windows", MB_OK);
                else if (rc == 1) MessageBoxW(h, L"Engine not running.", L"Start with Windows", MB_OK);
                else              MessageBoxW(h, L"Stop failed.", L"Start with Windows", MB_ICONERROR);
            } else {
                int rc = svc_start();
                if (rc == 0)      MessageBoxW(h, L"Engine started (icon in the notification area).", L"Start with Windows", MB_OK);
                else if (rc == 1) MessageBoxW(h, L"Engine already running.", L"Start with Windows", MB_OK);
                else              MessageBoxW(h, L"Start failed.", L"Start with Windows", MB_ICONERROR);
            }
            refresh_svc_buttons(h);
        }
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Dialog Plugin repository (Settings ▸ Plugin repository…)           */
/* ------------------------------------------------------------------ */
#define IDD_REPO        112
#define IDC_REPO_FETCH  2002
#define IDC_REPO_SEARCH 2003
#define IDC_REPO_TYPE   2004
#define IDC_REPO_LIST   2005
#define IDC_REPO_DL     2006

static repo_plugin* g_repo_list = NULL;
static int g_repo_n = 0;
static wchar_t g_repos[REPO_MAX_URLS][512];
static int g_repo_count = 0;
static wchar_t g_repo_base[512] = REPO_DEFAULT_BASE;   /* repo courant (téléchargements) */

static void repo_fill_list(HWND h)
{
    HWND lv = GetDlgItem(h, IDC_REPO_LIST);
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    wchar_t search[64] = L"";
    GetDlgItemTextW(h, IDC_REPO_SEARCH, search, 64);
    int type_sel = (int)SendMessageW(GetDlgItem(h, IDC_REPO_TYPE),
                                     CB_GETCURSEL, 0, 0);
    static const char* types[] = { "", "skin", "visual", "effect", "service" };
    const char* typef = types[type_sel < 0 ? 0 : type_sel];

    for (int i = 0; i < g_repo_n; i++) {
        repo_plugin* p = &g_repo_list[i];
        if (typef[0] && strcmp(p->type, typef)) continue;
        if (search[0]) {
            wchar_t name[64];
            MultiByteToWideChar(CP_UTF8, 0, p->name, -1, name, 64);
            if (!wcsstr(name, search)) continue;
        }
        wchar_t name[64], type[16], ver[32], desc[256];
        MultiByteToWideChar(CP_UTF8, 0, p->name, -1, name, 64);
        MultiByteToWideChar(CP_UTF8, 0, p->type, -1, type, 16);
        MultiByteToWideChar(CP_UTF8, 0, p->version, -1, ver, 32);
        MultiByteToWideChar(CP_UTF8, 0, p->desc, -1, desc, 256);
        LVITEMW li;
        memset(&li, 0, sizeof(li));
        li.mask = LVIF_TEXT | LVIF_PARAM;
        li.iItem = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
        li.lParam = i;
        li.pszText = name;
        int idx = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&li);
        li.iItem = idx;
        li.mask = LVIF_TEXT;
        li.iSubItem = 1; li.pszText = type; SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&li);
        li.iSubItem = 2; li.pszText = ver;  SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&li);
        li.iSubItem = 3; li.pszText = desc; SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&li);
    }
}

/* Remplit la liste des repositories et sélectionne le premier. */
static void repo_refresh_listbox(HWND h)
{
    HWND lb = GetDlgItem(h, 2007);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_repo_count; i++)
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)g_repos[i]);
    SendMessageW(lb, LB_SETCURSEL, 0, 0);
}

/* Charge l'index du repository sélectionné dans la liste. */
static void repo_fetch_current(HWND h)
{
    int sel = (int)SendMessageW(GetDlgItem(h, 2007), LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_repo_count) return;
    wcscpy(g_repo_base, g_repos[sel]);
    HCURSOR cur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    wchar_t json_url[1024];
    swprintf(json_url, 1024, L"%ls/plugins.json", g_repo_base);
    repo_free(g_repo_list, g_repo_n);
    g_repo_list = NULL;
    g_repo_n = 0;
    int rc = repo_fetch(json_url, &g_repo_list, &g_repo_n);
    SetCursor(cur);
    if (rc != 0)
        MessageBoxW(h, rc == -1 ? L"Network error (check the URL)."
                                : L"Invalid repository index.",
                    L"Plugin repository", MB_ICONERROR);
    repo_fill_list(h);
}

/* Dialog Add repository (ID 113) : saisie d'une URL. */
static INT_PTR CALLBACK repo_add_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            wchar_t url[512];
            GetDlgItemTextW(h, 2001, url, 512);
            if (url[0]) {
                if (g_repo_count < REPO_MAX_URLS) {
                    wcscpy(g_repos[g_repo_count++], url);
                    repo_list_save(g_repos, g_repo_count);
                    EndDialog(h, 1);
                } else {
                    MessageBoxW(h, L"Too many repositories.", L"Add repository",
                                MB_ICONERROR);
                }
            }
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK repo_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        HWND lv = GetDlgItem(h, IDC_REPO_LIST);
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 130; col.pszText = L"Name";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.cx = 70; col.pszText = L"Type";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        col.cx = 90; col.pszText = L"Version";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
        col.cx = 140; col.pszText = L"Description";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
        HWND cb = GetDlgItem(h, IDC_REPO_TYPE);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"All types");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Skin");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Visual");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Effect");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Service");
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
        /* liste des repositories (persistée) + fetch du premier */
        g_repo_count = repo_list_load(g_repos, REPO_MAX_URLS);
        repo_refresh_listbox(h);
        repo_fetch_current(h);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 2008) {          /* Add… */
            if (DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(113),
                           h, repo_add_proc) == 1) {
                repo_refresh_listbox(h);
                SendMessageW(GetDlgItem(h, 2007), LB_SETCURSEL,
                             g_repo_count - 1, 0);
                repo_fetch_current(h);
            }
        } else if (LOWORD(w) == 2009) {   /* Remove */
            int sel = (int)SendMessageW(GetDlgItem(h, 2007), LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_repo_count) {
                for (int i = sel; i < g_repo_count - 1; i++)
                    wcscpy(g_repos[i], g_repos[i + 1]);
                g_repo_count--;
                repo_list_save(g_repos, g_repo_count);
                repo_refresh_listbox(h);
                if (g_repo_count > 0) repo_fetch_current(h);
                else {
                    repo_free(g_repo_list, g_repo_n);
                    g_repo_list = NULL;
                    g_repo_n = 0;
                    repo_fill_list(h);
                }
            }
        } else if (LOWORD(w) == 2007) {   /* sélection d'un repository */
            if (HIWORD(w) == LBN_SELCHANGE) repo_fetch_current(h);
        } else if (LOWORD(w) == IDC_REPO_FETCH) {
            repo_fetch_current(h);
        } else if (LOWORD(w) == IDC_REPO_DL) {
            HWND lv = GetDlgItem(h, IDC_REPO_LIST);
            int sel = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1,
                                        LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxW(h, L"Select a plugin first.", L"Plugin repository", MB_OK);
                break;
            }
            LVITEMW li;
            memset(&li, 0, sizeof(li));
            li.mask = LVIF_PARAM;
            li.iItem = sel;
            SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&li);
            int pi = (int)li.lParam;
            if (pi < 0 || pi >= g_repo_n) break;
            HCURSOR cur = SetCursor(LoadCursor(NULL, IDC_WAIT));
            int rc = repo_download(g_repo_base, &g_repo_list[pi]);
            SetCursor(cur);
            if (rc == 0) {
                wchar_t msg[600];
                swprintf(msg, 600,
                         L"%hs downloaded.\nRestart MusicPlayer to load it.",
                         g_repo_list[pi].name);
                MessageBoxW(h, msg, L"Plugin repository", MB_OK);
            } else {
                /* le fichier est probablement verrouillé par le moteur
                 * (plugin chargé ou DLL FFmpeg en cours d'utilisation) :
                 * on propose de l'arrêter, retenter, et le relancer */
                const char* ty = g_repo_list[pi].type;
                /* catégories chargées comme DLL (service/engine, runtime,
                 * skin, visual, effect) : le fichier peut être verrouillé */
                int locked = ty && (ty[0] == 'e' || ty[0] == 'r' ||
                                    ty[0] == 's' || ty[0] == 'v');
                if (locked) {
                    int r = MessageBoxW(h,
                        L"Download failed: the file is probably locked by "
                        L"the engine (currently loaded).\n"
                        L"Stop the engine, retry the download and restart "
                        L"it? Playback will be interrupted briefly.",
                        L"Plugin repository",
                        MB_YESNO | MB_ICONQUESTION);
                    if (r == IDYES) {
                        cc_stop();
                        Sleep(800);
                        rc = repo_download(g_repo_base, &g_repo_list[pi]);
                        cc_start();
                        if (rc == 0)
                            MessageBoxW(h,
                                L"Downloaded and engine restarted.\n"
                                L"The new version is now loaded.",
                                L"Plugin repository", MB_OK);
                        else
                            MessageBoxW(h,
                                L"Download failed again.\nCheck your "
                                L"connection and retry.",
                                L"Plugin repository", MB_ICONERROR);
                    }
                } else {
                    MessageBoxW(h, L"Download failed.", L"Plugin repository",
                                MB_ICONERROR);
                }
            }
        } else if (LOWORD(w) == IDC_REPO_SEARCH) {
            if (HIWORD(w) == EN_CHANGE) repo_fill_list(h);
        } else if (LOWORD(w) == IDC_REPO_TYPE) {
            if (HIWORD(w) == CBN_SELCHANGE) repo_fill_list(h);
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

static void do_repo_dialog(void)
{
    DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_REPO), g_hwnd,
               repo_dlg_proc);
    repo_free(g_repo_list, g_repo_n);
    g_repo_list = NULL;
    g_repo_n = 0;
}

/* ------------------------------------------------------------------ */
/* Dialog Podcasts (File ▸ Podcasts…) — pilote le plugin podcasts du  */
/* moteur (port 8082)                                                  */
/* ------------------------------------------------------------------ */
#define IDD_PODCASTS    114
#define IDD_PODCAST_ADD 115
#define IDC_POD_SUBS    2001
#define IDC_POD_ADD     2002
#define IDC_POD_DEL     2003
#define IDC_POD_REFRESH 2004
#define IDC_POD_EPS     2005
#define IDC_POD_PLAY    2006
#define IDC_POD_MARK    2007
#define IDC_POD_DL      2008
#define IDC_POD_SEARCH  2009

#define PODCAST_PORT 8082

/* requête HTTP vers le plugin podcasts du moteur (127.0.0.1:8082).
 * Retourne le corps de réponse (malloc) ou NULL. */
static char* podcast_http(const char* method, const char* path,
                          const char* body, int* out_len)
{
    (void)method;   /* GET ou POST selon la présence du corps */
    wchar_t url[1200];
    char upath[1024];
    snprintf(upath, sizeof(upath), "http://127.0.0.1:%d%s", PODCAST_PORT, path);
    MultiByteToWideChar(CP_UTF8, 0, upath, -1, url, 1200);
    HINTERNET inet = InternetOpenW(L"MusicPlayer", INTERNET_OPEN_TYPE_DIRECT,
                                   NULL, NULL, 0);
    if (!inet) return NULL;
    DWORD to = 15000;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    const wchar_t* hdrs = body ? L"Content-Type: application/json\r\n" : NULL;
    HINTERNET uh = InternetOpenUrlW(inet, url, hdrs, -1,
                                    INTERNET_FLAG_RELOAD |
                                    INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!uh) { InternetCloseHandle(inet); return NULL; }
    if (body) {
        DWORD wr = 0;
        InternetWriteFile(uh, body, (DWORD)strlen(body), &wr);
    }
    char buf[4096];
    int cap = 8192, len = 0;
    char* resp = (char*)malloc(cap);
    if (!resp) { InternetCloseHandle(uh); InternetCloseHandle(inet); return NULL; }
    for (;;) {
        DWORD got = 0;
        if (!InternetReadFile(uh, buf, sizeof(buf), &got) || got == 0) break;
        if (len + (int)got + 1 > cap) {
            cap = (len + (int)got + 1) * 2;
            char* nb = (char*)realloc(resp, cap);
            if (!nb) { free(resp); InternetCloseHandle(uh); InternetCloseHandle(inet); return NULL; }
            resp = nb;
        }
        memcpy(resp + len, buf, got);
        len += (int)got;
    }
    InternetCloseHandle(uh);
    InternetCloseHandle(inet);
    resp[len] = 0;
    if (out_len) *out_len = len;
    return resp;
}

/* extrait les chaînes du JSON de réponse (même style que le moteur) */
static void pod_json_str(const char* body, const char* key, char* out, int outsz)
{
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < outsz - 1) out[n++] = *p++;
    out[n] = 0;
}

static void pod_json_unescape(char* s)
{
    /* \u00e9 → é (les titres RSS passent en JSON) */
    char* r = s;
    char* w = s;
    while (*r) {
        if (r[0] == '\\' && r[1] == 'u' && r[2] && r[3] && r[4] && r[5]) {
            char h[5] = { r[2], r[3], r[4], r[5], 0 };
            unsigned cp = (unsigned)strtoul(h, NULL, 16);
            if (cp < 0x80) {
                *w++ = (char)cp;
            } else if (cp < 0x800) {
                *w++ = (char)(0xC0 | (cp >> 6));
                *w++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *w++ = (char)(0xE0 | (cp >> 12));
                *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *w++ = (char)(0x80 | (cp & 0x3F));
            }
            r += 6;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static void pod_fill_subs(HWND h)
{
    HWND lv = GetDlgItem(h, IDC_POD_SUBS);
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    int len = 0;
    char* resp = podcast_http("GET", "/podcasts", NULL, &len);
    if (!resp) return;
    /* compte les objets */
    int n = 0;
    const char* p = resp;
    while ((p = strstr(p, "\"url\":")) != NULL) { n++; p += 5; }
    /* chaque objet : {"url":"..","title":"..","unread":N} */
    p = resp;
    for (int i = 0; i < n; i++) {
        const char* obj = strstr(p, "{\"url\":");
        if (!obj) break;
        const char* end = strchr(obj, '}');
        if (!end) break;
        int olen = (int)(end - obj) + 1;
        char tmp[4096];
        if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
        memcpy(tmp, obj, olen);
        tmp[olen] = 0;
        char url[512], title[512], unread[16];
        pod_json_str(tmp, "url", url, sizeof(url));
        pod_json_str(tmp, "title", title, sizeof(title));
        pod_json_str(tmp, "unread", unread, sizeof(unread));
        pod_json_unescape(title);
        wchar_t wtitle[512], wurl[512];
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 512);
        MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 512);
        wchar_t display[600];
        swprintf(display, 600, L"%ls [%hs non lus]", wtitle, unread);
        LVITEMW li;
        memset(&li, 0, sizeof(li));
        li.mask = LVIF_TEXT | LVIF_PARAM;
        li.iItem = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
        li.lParam = i;
        li.pszText = display;
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&li);
        p = end + 1;
    }
    free(resp);
}

/* URL-encode une chaîne pour la query string */
static void pod_urlencode(const char* in, char* out, int outsz)
{
    int o = 0;
    for (int i = 0; in[i] && o < outsz - 4; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == ':' || c == '/' || c == '?' || c == '&' ||
            c == '=' || c == '%') {
            out[o++] = (char)c;
        } else {
            o += snprintf(out + o, outsz - o, "%%%02X", c);
        }
    }
    out[o] = 0;
}

static void pod_fill_eps(HWND h)
{
    HWND lv = GetDlgItem(h, IDC_POD_EPS);
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    HWND subs = GetDlgItem(h, IDC_POD_SUBS);
    int sel = (int)SendMessageW(subs, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel < 0) return;
    LVITEMW li;
    memset(&li, 0, sizeof(li));
    li.mask = LVIF_PARAM;
    li.iItem = sel;
    SendMessageW(subs, LVM_GETITEMW, 0, (LPARAM)&li);
    /* l'URL de l'abonnement : on la retrouve dans la réponse /podcasts */
    int len = 0;
    char* resp = podcast_http("GET", "/podcasts", NULL, &len);
    if (!resp) return;
    char feed[512] = "";
    {
        const char* p = resp;
        for (int i = 0; i <= (int)li.lParam; i++) {
            const char* obj = strstr(p, "{\"url\":");
            if (!obj) break;
            const char* end = strchr(obj, '}');
            if (!end) break;
            if (i == (int)li.lParam) {
                char tmp[4096];
                int olen = (int)(end - obj) + 1;
                if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
                memcpy(tmp, obj, olen);
                tmp[olen] = 0;
                pod_json_str(tmp, "url", feed, sizeof(feed));
            }
            p = end + 1;
        }
    }
    free(resp);
    if (!feed[0]) return;
    char q[1200], path[1800];
    pod_urlencode(feed, q, sizeof(q));
    snprintf(path, sizeof(path), "/podcasts/episodes?feed=%s", q);
    resp = podcast_http("GET", path, NULL, &len);
    if (!resp) return;
    int n = 0;
    const char* p = resp;
    while ((p = strstr(p, "\"url\":")) != NULL) { n++; p += 5; }
    p = resp;
    for (int i = 0; i < n; i++) {
        const char* obj = strstr(p, "{\"url\":");
        if (!obj) break;
        const char* end = strchr(obj, '}');
        if (!end) break;
        int olen = (int)(end - obj) + 1;
        char tmp[8192];
        if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
        memcpy(tmp, obj, olen);
        tmp[olen] = 0;
        char url[512], title[512], date[64], dur[16], played[8], pos[16];
        pod_json_str(tmp, "url", url, sizeof(url));
        pod_json_str(tmp, "title", title, sizeof(title));
        pod_json_str(tmp, "date", date, sizeof(date));
        pod_json_str(tmp, "dur", dur, sizeof(dur));
        pod_json_str(tmp, "played", played, sizeof(played));
        pod_json_str(tmp, "pos", pos, sizeof(pos));
        pod_json_unescape(title);
        wchar_t wtitle[512];
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 512);
        wchar_t wdur[32];
        if (atoi(dur) > 0)
            swprintf(wdur, 32, L"%d:%02d", atoi(dur) / 60, atoi(dur) % 60);
        else
            wcscpy(wdur, L"?");
        wchar_t wstate[16];
        wcscpy(wstate, atoi(played) ? L"lu" : L"nouveau");
        LVITEMW li2;
        memset(&li2, 0, sizeof(li2));
        li2.mask = LVIF_TEXT | LVIF_PARAM;
        li2.iItem = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
        li2.lParam = i;
        li2.pszText = wtitle;
        int idx = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&li2);
        li2.iItem = idx;
        li2.mask = LVIF_TEXT;
        li2.iSubItem = 1;
        {
            wchar_t wdate[64];
            MultiByteToWideChar(CP_UTF8, 0, date, -1, wdate, 64);
            li2.pszText = wdate;
        }
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&li2);
        li2.iSubItem = 2; li2.pszText = wdur;
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&li2);
        li2.iSubItem = 3; li2.pszText = wstate;
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&li2);
        p = end + 1;
    }
    free(resp);
}

static void pod_refresh(HWND h)
{
    pod_fill_subs(h);
    pod_fill_eps(h);
}

/* URL de l'épisode sélectionné (dans la liste) */
static void pod_selected_episode(HWND h, char* url_out, int url_sz,
                                 char* played_out, int played_sz)
{
    url_out[0] = 0;
    played_out[0] = 0;
    HWND lv = GetDlgItem(h, IDC_POD_EPS);
    int sel = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel < 0) return;
    LVITEMW li;
    memset(&li, 0, sizeof(li));
    li.mask = LVIF_PARAM;
    li.iItem = sel;
    SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&li);
    /* re-fetch les épisodes du flux sélectionné et prend l'index */
    HWND subs = GetDlgItem(h, IDC_POD_SUBS);
    int ssel = (int)SendMessageW(subs, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (ssel < 0) return;
    LVITEMW ls;
    memset(&ls, 0, sizeof(ls));
    ls.mask = LVIF_PARAM;
    ls.iItem = ssel;
    SendMessageW(subs, LVM_GETITEMW, 0, (LPARAM)&ls);
    int len = 0;
    char* resp = podcast_http("GET", "/podcasts", NULL, &len);
    if (!resp) return;
    char feed[512] = "";
    {
        const char* p = resp;
        for (int i = 0; i <= (int)ls.lParam; i++) {
            const char* obj = strstr(p, "{\"url\":");
            if (!obj) break;
            const char* end = strchr(obj, '}');
            if (!end) break;
            if (i == (int)ls.lParam) {
                char tmp[4096];
                int olen = (int)(end - obj) + 1;
                if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
                memcpy(tmp, obj, olen);
                tmp[olen] = 0;
                pod_json_str(tmp, "url", feed, sizeof(feed));
            }
            p = end + 1;
        }
    }
    free(resp);
    if (!feed[0]) return;
    char q[1200], path[1800];
    pod_urlencode(feed, q, sizeof(q));
    snprintf(path, sizeof(path), "/podcasts/episodes?feed=%s", q);
    resp = podcast_http("GET", path, NULL, &len);
    if (!resp) return;
    {
        const char* p = resp;
        for (int i = 0; i <= (int)li.lParam; i++) {
            const char* obj = strstr(p, "{\"url\":");
            if (!obj) break;
            const char* end = strchr(obj, '}');
            if (!end) break;
            if (i == (int)li.lParam) {
                char tmp[8192];
                int olen = (int)(end - obj) + 1;
                if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
                memcpy(tmp, obj, olen);
                tmp[olen] = 0;
                pod_json_str(tmp, "url", url_out, url_sz);
                pod_json_str(tmp, "played", played_out, played_sz);
            }
            p = end + 1;
        }
    }
    free(resp);
}

static INT_PTR CALLBACK podcast_add_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            wchar_t url[600];
            GetDlgItemTextW(h, 2001, url, 600);
            if (url[0]) {
                char u8[900];
                WideCharToMultiByte(CP_UTF8, 0, url, -1, u8, 900, NULL, NULL);
                char body[1000];
                snprintf(body, sizeof(body), "{\"url\":\"%s\"}", u8);
                char* resp = podcast_http("POST", "/podcasts", body, NULL);
                if (resp) {
                    char title[512];
                    pod_json_str(resp, "title", title, sizeof(title));
                    pod_json_unescape(title);
                    if (!title[0]) {
                        char err[32];
                        pod_json_str(resp, "error", err, sizeof(err));
                        if (!strcmp(err, "network"))
                            MessageBoxW(h,
                                L"Network error while fetching the feed.\n"
                                L"Check your connection and try again.",
                                L"Podcasts", MB_ICONERROR);
                        else
                            MessageBoxW(h,
                                L"Invalid feed URL (no RSS podcast feed "
                                L"found at this address).\n"
                                L"If this keeps happening, update the "
                                L"Podcasts plugin: Settings \u25b8 Plugin "
                                L"repository\u2026 \u25b8 Download selected.",
                                L"Podcasts", MB_ICONERROR);
                    } else {
                        wchar_t wtitle[512], msg[600];
                        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 512);
                        swprintf(msg, 600, L"Subscribed: %ls", wtitle);
                        MessageBoxW(h, msg, L"Podcasts", MB_OK);
                        free(resp);
                        EndDialog(h, 1);
                    }
                } else {
                    MessageBoxW(h, L"Podcast service unavailable (engine not running?).",
                                L"Podcasts", MB_ICONERROR);
                }
                if (resp) free(resp);
            }
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK podcast_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        HWND lv = GetDlgItem(h, IDC_POD_SUBS);
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 470; col.pszText = L"Subscription";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        HWND le = GetDlgItem(h, IDC_POD_EPS);
        col.cx = 260; col.pszText = L"Episode";
        SendMessageW(le, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.cx = 90; col.pszText = L"Date";
        SendMessageW(le, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        col.cx = 60; col.pszText = L"Dur";
        SendMessageW(le, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
        col.cx = 70; col.pszText = L"State";
        SendMessageW(le, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
        pod_fill_subs(h);
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)l;
        if (nm->idFrom == IDC_POD_SUBS && nm->code == NM_CLICK)
            pod_fill_eps(h);
        if (nm->idFrom == IDC_POD_EPS && nm->code == NM_DBLCLK) {
            /* double-clic : jouer l'épisode */
            char url[512], played[8];
            pod_selected_episode(h, url, sizeof(url), played, sizeof(played));
            if (url[0]) {
                cc_cmd_path("open", url);
                char body[1200];
                snprintf(body, sizeof(body),
                         "{\"url\":\"%s\",\"played\":1}", url);
                podcast_http("POST", "/episodes", body, NULL);
                pod_refresh(h);
            }
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDC_POD_ADD) {
            if (DialogBoxW(GetModuleHandleW(NULL),
                           MAKEINTRESOURCEW(IDD_PODCAST_ADD), h,
                           podcast_add_proc) == 1)
                pod_refresh(h);
        } else if (LOWORD(w) == IDC_POD_SEARCH) {
            /* Search... : recherche dans les annuaires (sources) */
            if (DialogBoxW(GetModuleHandleW(NULL),
                           MAKEINTRESOURCEW(117), h,
                           podcast_search_dlg_proc) == 1)
                pod_refresh(h);
        } else if (LOWORD(w) == IDC_POD_SEARCH) {
            /* Search... : recherche dans les annuaires (sources) */
            if (DialogBoxW(GetModuleHandleW(NULL),
                           MAKEINTRESOURCEW(117), h,
                           podcast_search_dlg_proc) == 1)
                pod_refresh(h);
        } else if (LOWORD(w) == IDC_POD_DEL) {
            HWND subs = GetDlgItem(h, IDC_POD_SUBS);
            int sel = (int)SendMessageW(subs, LVM_GETNEXTITEM, (WPARAM)-1,
                                        LVNI_SELECTED);
            if (sel < 0) break;
            LVITEMW li;
            memset(&li, 0, sizeof(li));
            li.mask = LVIF_PARAM;
            li.iItem = sel;
            SendMessageW(subs, LVM_GETITEMW, 0, (LPARAM)&li);
            int len = 0;
            char* resp = podcast_http("GET", "/podcasts", NULL, &len);
            if (!resp) break;
            char url[512] = "";
            {
                const char* p = resp;
                for (int i = 0; i <= (int)li.lParam; i++) {
                    const char* obj = strstr(p, "{\"url\":");
                    if (!obj) break;
                    const char* end = strchr(obj, '}');
                    if (!end) break;
                    if (i == (int)li.lParam) {
                        char tmp[4096];
                        int olen = (int)(end - obj) + 1;
                        if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
                        memcpy(tmp, obj, olen);
                        tmp[olen] = 0;
                        pod_json_str(tmp, "url", url, sizeof(url));
                    }
                    p = end + 1;
                }
            }
            free(resp);
            if (url[0]) {
                char body[600];
                snprintf(body, sizeof(body), "{\"url\":\"%s\"}", url);
                podcast_http("POST", "/podcasts/del", body, NULL);
                pod_refresh(h);
            }
        } else if (LOWORD(w) == IDC_POD_REFRESH) {
            podcast_http("POST", "/refresh", NULL, NULL);
            pod_refresh(h);
        } else if (LOWORD(w) == IDC_POD_PLAY) {
            char url[512], played[8];
            pod_selected_episode(h, url, sizeof(url), played, sizeof(played));
            if (url[0]) {
                cc_cmd_path("open", url);
                char body[1200];
                snprintf(body, sizeof(body),
                         "{\"url\":\"%s\",\"played\":1}", url);
                podcast_http("POST", "/episodes", body, NULL);
                pod_refresh(h);
            }
        } else if (LOWORD(w) == IDC_POD_MARK) {
            char url[512], played[8];
            pod_selected_episode(h, url, sizeof(url), played, sizeof(played));
            if (url[0]) {
                char body[1200];
                snprintf(body, sizeof(body),
                         "{\"url\":\"%s\",\"played\":%d}",
                         url, atoi(played) ? 0 : 1);
                podcast_http("POST", "/episodes", body, NULL);
                pod_refresh(h);
            }
        } else if (LOWORD(w) == IDC_POD_DL) {
            char url[512], played[8];
            pod_selected_episode(h, url, sizeof(url), played, sizeof(played));
            if (url[0]) {
                char body[600];
                snprintf(body, sizeof(body), "{\"url\":\"%s\"}", url);
                char* resp = podcast_http("POST", "/download", body, NULL);
                if (resp && strstr(resp, "\"ok\":1"))
                    MessageBoxW(h, L"Episode downloaded (AppData\\MusicPlayer\\podcasts).",
                                L"Podcasts", MB_OK);
                else
                    MessageBoxW(h, L"Download failed.", L"Podcasts",
                                MB_ICONERROR);
                if (resp) free(resp);
            }
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Dialog Search podcasts (117) + Add source (118)                      */
/* ------------------------------------------------------------------ */
#define IDC_SRC_COMBO   2001
#define IDC_SRC_TERMS   2002
#define IDC_SRC_SEARCH  2003
#define IDC_SRC_RESULTS 2004
#define IDC_SRC_SUB     2005
#define IDC_SRC_ADD     2006

/* dernier path de recherche (pour retrouver le feed du résultat) */
static char g_last_search_path[1900] = "";

/* liste des sources affichées (sélection du combo) */
static int g_src_count = 0;
static char g_src_urls[16][600];
static char g_src_types[16][16];

static void src_fill_combo(HWND h)
{
    HWND cb = GetDlgItem(h, IDC_SRC_COMBO);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    g_src_count = 0;
    int len = 0;
    char* resp = podcast_http("GET", "/podcasts/sources", NULL, &len);
    if (!resp) return;
    int cnt = 0;
    const char* p = resp;
    while ((p = strstr(p, "\"type\":")) != NULL) { cnt++; p += 7; }
    p = resp;
    for (int i = 0; i < cnt; i++) {
        const char* obj = strstr(p, "{\"type\":");
        if (!obj) break;
        const char* end = strchr(obj, '}');
        if (!end) break;
        int olen = (int)(end - obj) + 1;
        char tmp[2048];
        if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
        memcpy(tmp, obj, olen);
        tmp[olen] = 0;
        char ty[16], nm[128], ur[600];
        pod_json_str(tmp, "type", ty, sizeof(ty));
        pod_json_str(tmp, "name", nm, sizeof(nm));
        pod_json_str(tmp, "url", ur, sizeof(ur));
        pod_json_unescape(nm);
        pod_json_unescape(ur);
        wchar_t wnm[160];
        utf8_to_wide(nm, wnm, 160);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)wnm);
        if (g_src_count < 16) {
            _snprintf(g_src_types[g_src_count], 16, "%s", ty);
            _snprintf(g_src_urls[g_src_count], 600, "%s", ur);
        }
        g_src_count++;
        p = end + 1;
    }
    free(resp);
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
}

static INT_PTR CALLBACK src_add_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        HWND cb = GetDlgItem(h, 2001);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Search directory");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"RSS feed (direct)");
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            int ty = (int)SendMessageW(GetDlgItem(h, 2001), CB_GETCURSEL, 0, 0);
            wchar_t wname[160], wurl[600];
            GetDlgItemTextW(h, 2002, wname, 160);
            GetDlgItemTextW(h, 2003, wurl, 600);
            if (!wname[0] || !wurl[0]) {
                MessageBoxW(h, L"Name and URL are required.", L"Add source",
                            MB_ICONWARNING);
                break;
            }
            char name[240], url[800];
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, 240, NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, wurl, -1, url, 800, NULL, NULL);
            char body[1100];
            snprintf(body, sizeof(body),
                     "{\"type\":\"%s\",\"name\":\"%s\",\"url\":\"%s\"}",
                     ty == 1 ? "rss" : "search", name, url);
            char* resp = podcast_http("POST", "/podcasts/sources", body, NULL);
            if (resp) {
                if (strstr(resp, "\"ok\":1")) {
                    free(resp);
                    EndDialog(h, 1);
                } else {
                    free(resp);
                    MessageBoxW(h, L"Could not add the source.",
                                L"Add source", MB_ICONERROR);
                }
            }
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK podcast_search_dlg_proc(HWND h, UINT m, WPARAM w,
                                                LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        HWND lv = GetDlgItem(h, IDC_SRC_RESULTS);
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 300; col.pszText = L"Podcast";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.cx = 150; col.pszText = L"Author";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        src_fill_combo(h);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDC_SRC_ADD) {
            if (DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(118),
                           h, src_add_proc) == 1)
                src_fill_combo(h);
        } else if (LOWORD(w) == IDC_SRC_SEARCH) {
            int sel = (int)SendMessageW(GetDlgItem(h, IDC_SRC_COMBO),
                                        CB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= g_src_count) break;
            wchar_t wterm[300];
            GetDlgItemTextW(h, IDC_SRC_TERMS, wterm, 300);
            if (!wterm[0]) break;
            char term[400];
            WideCharToMultiByte(CP_UTF8, 0, wterm, -1, term, 400, NULL, NULL);
            char eq[1200], path[1900];
            pod_urlencode(term, eq, sizeof(eq));
            snprintf(path, sizeof(path), "/podcasts/search?query=%s", eq);
            char sq[1500];
            pod_urlencode(g_src_urls[sel], sq, sizeof(sq));
            strncat(path, "&source=", sizeof(path) - strlen(path) - 1);
            strncat(path, sq, sizeof(path) - strlen(path) - 1);
            _snprintf(g_last_search_path, sizeof(g_last_search_path), "%s",
                      path);
            HWND lv = GetDlgItem(h, IDC_SRC_RESULTS);
            SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
            int len = 0;
            char* resp = podcast_http("GET", path, NULL, &len);
            if (!resp) {
                MessageBoxW(h, L"Search failed (service unavailable).",
                            L"Search", MB_ICONERROR);
                break;
            }
            if (strstr(resp, "\"error\"")) {
                MessageBoxW(h, L"Search failed. Check the source URL and "
                                L"your connection.",
                            L"Search", MB_ICONERROR);
                free(resp);
                break;
            }
            int cnt = 0;
            const char* p = resp;
            while ((p = strstr(p, "\"feed\":")) != NULL) { cnt++; p += 7; }
            p = resp;
            for (int i = 0; i < cnt; i++) {
                const char* obj = strstr(p, "{\"title\":");
                if (!obj) break;
                const char* end = strchr(obj, '}');
                if (!end) break;
                int olen = (int)(end - obj) + 1;
                char tmp[4096];
                if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
                memcpy(tmp, obj, olen);
                tmp[olen] = 0;
                char ttl[256], aut[256], feed[600];
                pod_json_str(tmp, "title", ttl, sizeof(ttl));
                pod_json_str(tmp, "author", aut, sizeof(aut));
                pod_json_str(tmp, "feed", feed, sizeof(feed));
                pod_json_unescape(ttl);
                pod_json_unescape(aut);
                pod_json_unescape(feed);
                wchar_t wttl[300], waut[300];
                utf8_to_wide(ttl, wttl, 300);
                utf8_to_wide(aut, waut, 300);
                LVITEMW it;
                memset(&it, 0, sizeof(it));
                it.mask = LVIF_TEXT | LVIF_PARAM;
                it.iItem = i;
                it.pszText = wttl;
                it.lParam = i;
                int idx = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
                it.iItem = idx;
                it.mask = LVIF_TEXT;
                it.iSubItem = 1;
                it.pszText = waut;
                SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&it);
                p = end + 1;
            }
            free(resp);
        } else if (LOWORD(w) == IDC_SRC_SUB) {
            HWND lv = GetDlgItem(h, IDC_SRC_RESULTS);
            int sel = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1,
                                        LVNI_SELECTED);
            if (sel < 0) {
                /* source RSS directe : s'abonner à l'URL de la source */
                int ssel = (int)SendMessageW(GetDlgItem(h, IDC_SRC_COMBO),
                                             CB_GETCURSEL, 0, 0);
                if (ssel >= 0 && ssel < g_src_count &&
                    !strcmp(g_src_types[ssel], "rss")) {
                    char body[900];
                    snprintf(body, sizeof(body),
                             "{\"url\":\"%s\"}", g_src_urls[ssel]);
                    char* resp = podcast_http("POST", "/podcasts", body, NULL);
                    if (resp && strstr(resp, "\"ok\":")) {
                        free(resp);
                        EndDialog(h, 1);
                        return TRUE;
                    }
                    if (resp) free(resp);
                    MessageBoxW(h, L"Could not subscribe to this feed.",
                                L"Search", MB_ICONERROR);
                }
                break;
            }
            /* re-fetch les résultats pour retrouver le feed sélectionné */
            int len = 0;
            char* resp = g_last_search_path[0]
                ? podcast_http("GET", g_last_search_path, NULL, &len) : NULL;
            if (!resp) break;
            const char* p = resp;
            char feed[600] = "";
            for (int i = 0; i <= sel; i++) {
                const char* obj = strstr(p, "{\"title\":");
                if (!obj) break;
                const char* end = strchr(obj, '}');
                if (!end) break;
                if (i == sel) {
                    char tmp[4096];
                    int olen = (int)(end - obj) + 1;
                    if (olen > (int)sizeof(tmp) - 1) olen = (int)sizeof(tmp) - 1;
                    memcpy(tmp, obj, olen);
                    tmp[olen] = 0;
                    pod_json_str(tmp, "feed", feed, sizeof(feed));
                    pod_json_unescape(feed);
                }
                p = end + 1;
            }
            free(resp);
            if (feed[0]) {
                char body[900];
                snprintf(body, sizeof(body), "{\"url\":\"%s\"}", feed);
                char* r2 = podcast_http("POST", "/podcasts", body, NULL);
                if (r2 && strstr(r2, "\"ok\":")) {
                    free(r2);
                    EndDialog(h, 1);
                    return TRUE;
                }
                if (r2) free(r2);
                MessageBoxW(h, L"Could not subscribe to this podcast.",
                            L"Search", MB_ICONERROR);
            }
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

static void do_podcast_dialog(void)
{
    DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_PODCASTS),
               g_hwnd, podcast_dlg_proc);
}

/* ------------------------------------------------------------------ */
/* Dialog Logs (Help ▸ Logs…) : niveau de journalisation + dossier     */
/* ------------------------------------------------------------------ */
#define IDD_LOGS        116
#define IDC_LOG_LEVEL   2001
#define IDC_LOG_OPEN    2002

void mp_set_log_level(int lvl);

static INT_PTR CALLBACK logs_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        HWND cb = GetDlgItem(h, IDC_LOG_LEVEL);
        static const wchar_t* lvls[] = {
            L"Nothing", L"Errors only", L"Info", L"Debug"
        };
        for (int i = 0; i < 4; i++)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)lvls[i]);
        SendMessageW(cb, CB_SETCURSEL, g_cfg.log_level < 0 ? 0 :
                      (g_cfg.log_level > 3 ? 3 : g_cfg.log_level), 0);
        wchar_t msg[400];
        GetModuleFileNameW(NULL, msg, MAX_PATH);
        wchar_t* sl = wcsrchr(msg, L'\\');
        if (sl) wcscpy(sl + 1, L"logs");
        wchar_t full[440];
        swprintf(full, 440, L"Logs are written to:\r\n%ls", msg);
        SetDlgItemTextW(h, 1002, full);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            int sel = (int)SendMessageW(GetDlgItem(h, IDC_LOG_LEVEL),
                                        CB_GETCURSEL, 0, 0);
            if (sel < 0) sel = 2;
            g_cfg.log_level = sel;
            config_save();
            mp_set_log_level(sel);
            /* le moteur suit le niveau (POST /api/config) */
            cc_push_log_level(sel);
            EndDialog(h, 1);
        } else if (LOWORD(w) == IDC_LOG_OPEN) {
            wchar_t msg[400];
            GetModuleFileNameW(NULL, msg, MAX_PATH);
            wchar_t* sl = wcsrchr(msg, L'\\');
            if (sl) wcscpy(sl + 1, L"logs");
            CreateDirectoryW(msg, NULL);
            ShellExecuteW(NULL, L"open", msg, NULL, NULL, SW_SHOWNORMAL);
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

static void do_logs_dialog(void)
{
    DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_LOGS),
               g_hwnd, logs_dlg_proc);
}

static void do_interface_dialog(void)
{
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_INTERFACE),
                    g_hwnd, interface_dlg_proc, 0);
}

/* ------------------------------------------------------------------ */
/* Dialog Update (Settings ▸ Update…) : mode + vérification            */
/* ------------------------------------------------------------------ */
static INT_PTR CALLBACK updcfg_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        int mode = mp_update_get_mode();
        CheckRadioButton(h, 1018, 1022,
                         mode == 0 ? 1020 : mode == 2 ? 1019 :
                         mode == 3 ? 1022 : 1018);
        CheckRadioButton(h, 1024, 1025,
                         mp_update_get_type() ? 1025 : 1024);
        int lag = mp_update_get_lag();
        CheckRadioButton(h, 1027, 1030,
                         lag >= 30 ? 1030 : lag >= 7 ? 1029 :
                         lag >= 1 ? 1028 : 1027);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            int mode = 1;
            if (IsDlgButtonChecked(h, 1019)) mode = 2;
            else if (IsDlgButtonChecked(h, 1020)) mode = 0;
            else if (IsDlgButtonChecked(h, 1022)) mode = 3;
            mp_update_set_mode(mode);
            mp_update_set_type(IsDlgButtonChecked(h, 1025) ? 1 : 0);
            int lag = IsDlgButtonChecked(h, 1028) ? 1 :
                      IsDlgButtonChecked(h, 1029) ? 7 :
                      IsDlgButtonChecked(h, 1030) ? 30 : 0;
            mp_update_set_lag(lag);
            EndDialog(h, 1);
        } else if (LOWORD(w) == IDC_UPD_CHECK) {
            EndDialog(h, 2);   /* vérifier maintenant */
        } else if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
        }
        return TRUE;
    }
    return FALSE;
}

static void do_update_cfg_dialog(void)
{
    INT_PTR r = DialogBoxParamW(GetModuleHandleW(NULL),
                                MAKEINTRESOURCEW(IDD_UPDATE_CFG),
                                g_hwnd, updcfg_dlg_proc, 0);
    if (r == 2) mp_update_check_async(g_hwnd, 1);
}

static INT_PTR CALLBACK upd_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        wchar_t msg[512];
        swprintf(msg, 512, lang_get("upd_new"), mp_update_latest(), MP_VERSION);
        SetDlgItemTextW(h, IDC_UPD_LBL, msg);
        SetDlgItemTextW(h, 1, lang_get("upd_now"));
        SetDlgItemTextW(h, 2, lang_get("upd_later"));
        SetDlgItemTextW(h, 3, lang_get("upd_skip"));
        SetWindowTextW(h, lang_get("upd_title"));
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 1)                    EndDialog(h, 1);
        else if (LOWORD(w) == 2 || LOWORD(w) == IDCANCEL) EndDialog(h, 2);
        else if (LOWORD(w) == 3)               EndDialog(h, 3);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Reprise / sauvegarde de la session (config.yml)                     */
/* ------------------------------------------------------------------ */
static void resume_last_session(void)
{
    if (g_cfg.last_path[0] == 0) return;
    wchar_t path_w[MAX_PATH * 3];
    utf8_to_wide(g_cfg.last_path, path_w, MAX_PATH * 3);
    if (GetFileAttributesW(path_w) == INVALID_FILE_ATTRIBUTES) return;
    if (GetFileAttributesW(path_w) & FILE_ATTRIBUTE_DIRECTORY) {
        /* playlist : rescan complet du dossier — les nouveaux fichiers sont
         * ajoutés, ceux qui n'existent plus sont retirés */
        if (playlist_open_folder(path_w) == 0 && g_cfg.last_file[0]) {
            wchar_t file_w[MAX_PATH * 3];
            utf8_to_wide(g_cfg.last_file, file_w, MAX_PATH * 3);
            for (int i = 0; i < g_plist_n; i++) {
                if (!_wcsicmp(g_plist[i], file_w)) {
                    playlist_play_index(i);
                    break;
                }
            }
        }
    } else {
        cc_open(g_cfg.last_path);
    }
}

static void save_state(void)
{
    g_cfg.volume = (int)(sp_get_volume() * 100.0f + 0.5f);
    g_cfg.speed = cc_speed();
    g_cfg.shuffle = g_shuffle;
    g_cfg.web_enabled = g_web_enabled;
    g_cfg.web_port = g_web_port;
    g_cfg.web_audio = g_web_audio;
    strncpy(g_cfg.web_ips, g_web_ips_cfg, sizeof(g_cfg.web_ips) - 1);
    g_cfg.web_ips[sizeof(g_cfg.web_ips) - 1] = 0;
    if (g_plist_n > 0 && g_plist_dir[0]) {
        wide_to_utf8(g_plist_dir, g_cfg.last_path, sizeof(g_cfg.last_path));
        if (g_plist_idx >= 0 && g_plist_idx < g_plist_n)
            wide_to_utf8(g_plist[g_plist_idx], g_cfg.last_file,
                         sizeof(g_cfg.last_file));
        else
            g_cfg.last_file[0] = 0;
    } else if (cc_name()) {
        strncpy(g_cfg.last_path, cc_name(), sizeof(g_cfg.last_path) - 1);
        g_cfg.last_path[sizeof(g_cfg.last_path) - 1] = 0;
        g_cfg.last_file[0] = 0;
    }
    config_save();
}

/* ------------------------------------------------------------------ */
/* Interface playlist pour le serveur web (server.c)                   */
/* ------------------------------------------------------------------ */
int web_plist_count(void)
{
    return g_plist_n;
}

const wchar_t* web_plist_name(int i)
{
    if (i < 0 || i >= g_plist_n) return L"";
    const wchar_t* p = g_plist[i];
    const wchar_t* s = wcsrchr(p, L'\\');
    return s ? s + 1 : p;
}

int web_plist_index(void)
{
    return g_plist_idx;
}

void web_plist_next(void)
{
    playlist_next();
}

/* Joue directement le morceau n° i (clic sur la playlist web) */
void web_playlist_play(int i)
{
    playlist_play_index(i);
}

/* Bascule le mode aléatoire */
void web_shuffle_toggle(void)
{
    playlist_set_shuffle(!playlist_get_shuffle());
}

int web_plist_shuffle(void)
{
    return g_shuffle;
}

/* Changement de sortie audio depuis le serveur web (PC / téléphone / les 2) */
void web_set_audio_out(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_web_audio = mode;
    mp_set_audio_out(mode);
    web_save_config();
}

static INT_PTR CALLBACK about_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return dlg_skin_color(h, w, l);
    case WM_INITDIALOG: {
        wchar_t l1[160], l2[320];
        swprintf(l1, 160, L"MusicPlayer %hs", MP_VERSION);
        swprintf(l2, 320, L"FFmpeg %hs · %d plugins",
                 av_version_info(), mp_plugins_count());
        SetDlgItemTextW(h, IDC_ABT_L1, l1);
        SetDlgItemTextW(h, IDC_ABT_L2, l2);
        SetWindowTextW(h, lang_get("about_title"));
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 1 || LOWORD(w) == IDCANCEL)
            EndDialog(h, 0);
        else if (LOWORD(w) == 2)
            ShellExecuteW(h, L"open",
                          L"https://github.com/LostInTheBugs/MusicPlayer",
                          NULL, NULL, SW_SHOWNORMAL);
        return TRUE;
    }
    return FALSE;
}

static void do_about(void)
{
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_ABOUT),
                    g_hwnd, about_dlg_proc, 0);
}

/* ------------------------------------------------------------------ */
/* Barre de contrôles : boutons lecture/pause, stop, plein écran,      */
/* curseur de volume. Layout + dessin + hit-testing.                    */
/* ------------------------------------------------------------------ */
static void layout_controls(const RECT* rc)
{
    int y = rc->bottom - CTRL_H;      /* en bas par défaut */
    if (g_skin_ctrl_top) y = rc->top; /* le skin met les contrôles en haut */
    int x = rc->left + 8;
    int bs = CTRL_H - 8;                 /* taille des boutons carrés */
    g_rc_play.left = x; g_rc_play.top = y + 4;
    g_rc_play.right = x + bs; g_rc_play.bottom = y + 4 + bs;
    x += bs + 6;
    g_rc_stop.left = x; g_rc_stop.top = y + 4;
    g_rc_stop.right = x + bs; g_rc_stop.bottom = y + 4 + bs;
    x += bs + 6;
    /* bouton suivant */
    g_rc_next.left = x; g_rc_next.top = y + 4;
    g_rc_next.right = x + bs; g_rc_next.bottom = y + 4 + bs;
    x += bs + 6;
    /* bouton aléatoire (shuffle) */
    g_rc_shuffle.left = x; g_rc_shuffle.top = y + 4;
    g_rc_shuffle.right = x + bs; g_rc_shuffle.bottom = y + 4 + bs;
    x += bs + 6;
    /* bouton playlist */
    g_rc_plist.left = x; g_rc_plist.top = y + 4;
    g_rc_plist.right = x + bs; g_rc_plist.bottom = y + 4 + bs;
    x += bs + 6;
    /* curseur de volume */
    g_rc_vol.left = x;
    g_rc_vol.top = y + (CTRL_H - 16) / 2;
    g_rc_vol.right = rc->right - 56;
    g_rc_vol.bottom = g_rc_vol.top + 16;
    /* bouton plein écran (coin droit) */
    g_rc_fs.left = rc->right - 44;
    g_rc_fs.top = y + 4;
    g_rc_fs.right = rc->right - 8;
    g_rc_fs.bottom = y + 4 + bs;
}

static void draw_glyph_play(HDC hdc, RECT* r, int paused)
{
    /* triangle (lecture) ou deux barres (pause) */
    HBRUSH wh = (HBRUSH)GetStockObject(WHITE_BRUSH);
    if (paused) {
        int bw = (r->right - r->left) / 5;
        int gap = bw / 2;
        RECT b1 = { r->left + gap, r->top + 3, r->left + gap + bw, r->bottom - 3 };
        RECT b2 = { r->right - gap - bw, r->top + 3, r->right - gap, r->bottom - 3 };
        FillRect(hdc, &b1, wh);
        FillRect(hdc, &b2, wh);
    } else {
        POINT pts[3] = {
            { r->left + 2, r->top },
            { r->left + 2, r->bottom },
            { r->right - 1, (r->top + r->bottom) / 2 }
        };
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HBRUSH oldb = (HBRUSH)SelectObject(hdc, wh);
        HPEN oldp = (HPEN)SelectObject(hdc, pen);
        Polygon(hdc, pts, 3);
        SelectObject(hdc, oldb);
        SelectObject(hdc, oldp);
        DeleteObject(pen);
    }
}

static void draw_glyph_stop(HDC hdc, RECT* r)
{
    HBRUSH wh = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RECT s = { r->left + 3, r->top + 3, r->right - 3, r->bottom - 3 };
    FillRect(hdc, &s, wh);
}

static void draw_glyph_next(HDC hdc, RECT* r)
{
    /* ⏭ : deux triangles pointant à droite */
    HBRUSH wh = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, wh);
    HPEN oldp = (HPEN)SelectObject(hdc, pen);
    int w = (r->right - r->left) / 2;
    POINT t1[3] = {
        { r->left + 1, r->top },
        { r->left + 1, r->bottom },
        { r->left + w - 2, (r->top + r->bottom) / 2 }
    };
    POINT t2[3] = {
        { r->left + w + 1, r->top },
        { r->left + w + 1, r->bottom },
        { r->right - 1, (r->top + r->bottom) / 2 }
    };
    Polygon(hdc, t1, 3);
    Polygon(hdc, t2, 3);
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

static void draw_glyph_shuffle(HDC hdc, RECT* r)
{
    /* deux flèches horizontales croisées */
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HPEN old = (HPEN)SelectObject(hdc, pen);
    int x0 = r->left + 1, x1 = r->right - 1;
    int cy = (r->top + r->bottom) / 2;
    MoveToEx(hdc, x0, cy - 4, NULL); LineTo(hdc, x1, cy - 4);
    MoveToEx(hdc, x0, cy + 4, NULL); LineTo(hdc, x1, cy + 4);
    /* pointes : flèche haute → droite, flèche basse → gauche */
    MoveToEx(hdc, x1 - 4, cy - 7, NULL); LineTo(hdc, x1, cy - 4); LineTo(hdc, x1 - 4, cy - 1);
    MoveToEx(hdc, x0 + 4, cy + 1, NULL); LineTo(hdc, x0, cy + 4); LineTo(hdc, x0 + 4, cy + 7);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void draw_glyph_playlist(HDC hdc, RECT* r)
{
    /* liste : trois lignes avec une puce carrée à gauche */
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HPEN old = (HPEN)SelectObject(hdc, pen);
    HBRUSH wh = (HBRUSH)GetStockObject(WHITE_BRUSH);
    int x0 = r->left + 2, x1 = r->right - 2;
    int cy = (r->top + r->bottom) / 2;
    for (int i = -1; i <= 1; i++) {
        int yy = cy + i * 4;
        HBRUSH oldb = (HBRUSH)SelectObject(hdc, wh);
        Rectangle(hdc, x0, yy - 2, x0 + 4, yy + 2);
        SelectObject(hdc, oldb);
        MoveToEx(hdc, x0 + 7, yy, NULL); LineTo(hdc, x1, yy);
    }
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void draw_glyph_fullscreen(HDC hdc, RECT* r)
{
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HPEN old = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    int m = 5;
    Rectangle(hdc, r->left + m, r->top + m, r->right - m, r->bottom - m);
    SelectObject(hdc, oldb);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void draw_glyph_volume(HDC hdc, int x, int y)
{
    /* haut-parleur stylisé */
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(90, 98, 116));
    HPEN old = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    POINT body[4] = { { x, y + 5 }, { x + 5, y + 5 }, { x + 9, y + 1 }, { x + 9, y + 15 } };
    Polygon(hdc, body, 4);
    /* onde : deux petits arcs */
    Arc(hdc, x + 8, y, x + 20, y + 16, x + 8, y, x + 20, y + 16);
    SelectObject(hdc, oldb);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void paint_controls(HDC hdc, const RECT* rc)
{
    if (g_fullscreen) return;
    layout_controls(rc);

    /* fond de la barre : à l'endroit des contrôles (haut si le skin
     * les y met, sinon bas) */
    int cy = g_skin_ctrl_top ? rc->top : rc->bottom - CTRL_H;
    RECT bg = { rc->left, cy, rc->right, cy + CTRL_H };
    if (g_skin_bg) {
        /* voile semi-transparent : l'artwork reste lisible dessous */
        GpGraphics* g = NULL;
        if (GdipCreateFromHDC(hdc, &g) == Ok) {
            GpSolidFill* br = NULL;
            GdipCreateSolidFill(0xA0000000, &br);   /* noir 63 % */
            if (br) {
                GdipFillRectangleI(g, (GpBrush*)br, bg.left, bg.top,
                                   bg.right - bg.left, bg.bottom - bg.top);
                GdipDeleteBrush((GpBrush*)br);
            }
            GdipDeleteGraphics(g);
        }
    } else {
        HBRUSH bbg = CreateSolidBrush(g_skin.ctrl_bar);
        FillRect(hdc, &bg, bbg);
        DeleteObject(bbg);
    }
    HPEN sep = CreatePen(PS_SOLID, 1, g_skin.ctrl_sep);
    HPEN oldp = (HPEN)SelectObject(hdc, sep);
    int sy = g_skin_ctrl_top ? bg.bottom - 1 : bg.top;
    MoveToEx(hdc, bg.left, sy, NULL);
    LineTo(hdc, bg.right, sy);
    SelectObject(hdc, oldp);
    DeleteObject(sep);

    /* bouton lecture / pause */
    HBRUSH bplay = CreateSolidBrush(g_skin.accent);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, bplay);
    RoundRect(hdc, g_rc_play.left, g_rc_play.top, g_rc_play.right, g_rc_play.bottom, 8, 8);
    SelectObject(hdc, oldb);
    DeleteObject(bplay);
    RECT gp = g_rc_play;
    gp.left += 3; gp.right -= 3; gp.top += 3; gp.bottom -= 3;
    draw_glyph_play(hdc, &gp, cc_st() == MP_STATE_PLAYING);

    /* bouton stop */
    HBRUSH bstop = CreateSolidBrush(g_skin.accent2);
    oldb = (HBRUSH)SelectObject(hdc, bstop);
    RoundRect(hdc, g_rc_stop.left, g_rc_stop.top, g_rc_stop.right, g_rc_stop.bottom, 8, 8);
    SelectObject(hdc, oldb);
    DeleteObject(bstop);
    RECT gs = g_rc_stop;
    gs.left += 2; gs.right -= 2; gs.top += 2; gs.bottom -= 2;
    draw_glyph_stop(hdc, &gs);

    /* bouton suivant */
    HBRUSH bnext = CreateSolidBrush(g_skin.accent);
    oldb = (HBRUSH)SelectObject(hdc, bnext);
    RoundRect(hdc, g_rc_next.left, g_rc_next.top, g_rc_next.right, g_rc_next.bottom, 8, 8);
    SelectObject(hdc, oldb);
    DeleteObject(bnext);
    RECT gn = g_rc_next;
    gn.left += 3; gn.right -= 3; gn.top += 3; gn.bottom -= 3;
    draw_glyph_next(hdc, &gn);

    /* bouton aléatoire (shuffle) : accent3 si actif */
    HBRUSH bsh = CreateSolidBrush(g_shuffle ? g_skin.accent3 : g_skin.neutral);
    oldb = (HBRUSH)SelectObject(hdc, bsh);
    RoundRect(hdc, g_rc_shuffle.left, g_rc_shuffle.top,
              g_rc_shuffle.right, g_rc_shuffle.bottom, 8, 8);
    SelectObject(hdc, oldb);
    DeleteObject(bsh);
    RECT gsh = g_rc_shuffle;
    gsh.left += 2; gsh.right -= 2; gsh.top += 2; gsh.bottom -= 2;
    draw_glyph_shuffle(hdc, &gsh);

    /* bouton playlist */
    HBRUSH bpl = CreateSolidBrush(g_skin.neutral);
    oldb = (HBRUSH)SelectObject(hdc, bpl);
    RoundRect(hdc, g_rc_plist.left, g_rc_plist.top,
              g_rc_plist.right, g_rc_plist.bottom, 8, 8);
    SelectObject(hdc, oldb);
    DeleteObject(bpl);
    RECT gp2 = g_rc_plist;
    gp2.left += 2; gp2.right -= 2; gp2.top += 2; gp2.bottom -= 2;
    draw_glyph_playlist(hdc, &gp2);

    /* bouton plein écran */
    HBRUSH bfs = CreateSolidBrush(g_skin.neutral);
    oldb = (HBRUSH)SelectObject(hdc, bfs);
    RoundRect(hdc, g_rc_fs.left, g_rc_fs.top, g_rc_fs.right, g_rc_fs.bottom, 8, 8);
    SelectObject(hdc, oldb);
    DeleteObject(bfs);
    RECT gf = g_rc_fs;
    gf.left += 2; gf.right -= 2; gf.top += 2; gf.bottom -= 2;
    draw_glyph_fullscreen(hdc, &gf);

    /* curseur de volume : 0..100% (bleu) puis boost 100..200% (orange) */
    draw_glyph_volume(hdc, g_rc_vol.left - 14, g_rc_vol.top);
    int vw = g_rc_vol.right - g_rc_vol.left;
    float vol = sp_get_volume();
    int fill = (int)(vol * 0.5f * vw);          /* position du curseur */
    int mid = vw / 2;                            /* marque 100 % */
    HBRUSH track = CreateSolidBrush(g_skin.track);
    RECT tr = { g_rc_vol.left, g_rc_vol.top + 6, g_rc_vol.right, g_rc_vol.top + 10 };
    FillRect(hdc, &tr, track);
    DeleteObject(track);
    /* 0..100 % : accent */
    if (fill > 0) {
        int fw = fill < mid ? fill : mid;
        if (fw > 0) {
            HBRUSH bfill = CreateSolidBrush(g_skin.accent);
            RECT fr = { g_rc_vol.left, g_rc_vol.top + 6, g_rc_vol.left + fw, g_rc_vol.top + 10 };
            FillRect(hdc, &fr, bfill);
            DeleteObject(bfill);
        }
    }
    /* 100..200 % : accent3 (booster) */
    if (fill > mid) {
        HBRUSH bboost = CreateSolidBrush(g_skin.accent3);
        RECT fr = { g_rc_vol.left + mid, g_rc_vol.top + 6, g_rc_vol.left + fill, g_rc_vol.top + 10 };
        FillRect(hdc, &fr, bboost);
        DeleteObject(bboost);
    }
    /* marque des 100 % */
    HBRUSH bmark = CreateSolidBrush(g_skin.mark);
    RECT mk = { g_rc_vol.left + mid - 1, g_rc_vol.top + 5, g_rc_vol.left + mid + 1, g_rc_vol.top + 11 };
    FillRect(hdc, &mk, bmark);
    DeleteObject(bmark);
    int knob = g_rc_vol.left + fill;
    HBRUSH bknob = CreateSolidBrush(g_skin.knob);
    oldb = (HBRUSH)SelectObject(hdc, bknob);
    Ellipse(hdc, knob - 6, g_rc_vol.top, knob + 6, g_rc_vol.bottom);
    SelectObject(hdc, oldb);
    DeleteObject(bknob);
}

static void vol_from_mouse(int x)
{
    int w = g_rc_vol.right - g_rc_vol.left;
    if (w <= 0) return;
    float v = (float)(x - g_rc_vol.left) / w * 2.0f;   /* 0..200 % */
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    sp_set_volume(v);
    status_update();
}

/* ------------------------------------------------------------------ */
/* Zone centrale (WM_PAINT)                                            */
/* ------------------------------------------------------------------ */
/* Zone de contenu : toute la zone client, moins la barre d'état si
 * elle est visible. C'est la surface de l'image de fond, de la barre de
 * progression et des contrôles. */
static void get_content_rect(HWND hwnd, RECT* rc)
{
    GetClientRect(hwnd, rc);
    if (g_status && IsWindowVisible(g_status)) {
        RECT sr;
        GetWindowRect(g_status, &sr);
        rc->bottom -= (sr.bottom - sr.top);
    }
}

/* (La zone du visualiseur est calculée dans paint_center, en
 * coordonnées locales du DC mémoire — voir le bloc g_skin_vis.) */

#define PROGRESS_H 16   /* hauteur de la barre de progression */

/* mélange deux couleurs (t 0..1) */
static COLORREF blend_color(COLORREF a, COLORREF b, float t)
{
    return RGB((BYTE)(GetRValue(a) * (1.0f - t) + GetRValue(b) * t),
               (BYTE)(GetGValue(a) * (1.0f - t) + GetGValue(b) * t),
               (BYTE)(GetBValue(a) * (1.0f - t) + GetBValue(b) * t));
}

/* Barre de progression (dégradé accent -> accent2) */
static HBRUSH g_pg_fill[40] = { 0 };
static HBRUSH g_pg_bg = NULL;
static HBRUSH g_pg_border = NULL;

static void skin_reset_brushes(void)
{
    for (int i = 0; i < 40; i++) {
        if (g_pg_fill[i]) { DeleteObject(g_pg_fill[i]); g_pg_fill[i] = NULL; }
    }
    if (g_pg_bg) { DeleteObject(g_pg_bg); g_pg_bg = NULL; }
    if (g_pg_border) { DeleteObject(g_pg_border); g_pg_border = NULL; }
}

static void draw_progress_bar(HDC hdc, const RECT* rc)
{
    if (!g_pg_bg) {
        for (int i = 0; i < 40; i++) {
            g_pg_fill[i] = CreateSolidBrush(
                blend_color(g_skin.accent, g_skin.accent2, (float)i / 39.0f));
        }
        g_pg_bg = CreateSolidBrush(g_skin.prog_bg);
        g_pg_border = CreateSolidBrush(g_skin.prog_border);
    }

    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;
    if (w <= 0 || h <= 0) return;

    /* fond : voile semi-transparent si le skin a une image de fond
     * (l'artwork reste lisible), sinon aplat habituel */
    RECT bg = { rc->left, rc->top, rc->right, rc->bottom };
    if (g_skin_bg) {
        GpGraphics* g = NULL;
        if (GdipCreateFromHDC(hdc, &g) == Ok) {
            GpSolidFill* br = NULL;
            GdipCreateSolidFill(0x80000000, &br);   /* noir 50 % */
            if (br) {
                GdipFillRectangleI(g, (GpBrush*)br, bg.left, bg.top,
                                   bg.right - bg.left, bg.bottom - bg.top);
                GdipDeleteBrush((GpBrush*)br);
            }
            GdipDeleteGraphics(g);
        }
    } else {
        FillRect(hdc, &bg, g_pg_bg);
    }

    /* remplissage */
    double dur = cc_dur();
    double pos = cc_pos();
    if (dur > 0.0 && pos > 0.0) {
        double ratio = pos / dur;
        if (ratio > 1.0) ratio = 1.0;
        int fw = (int)((double)(w - 2) * ratio);
        if (fw > 0) {
            for (int x = 0; x < fw; x += 6) {
                int seg = fw - x;
                if (seg > 6) seg = 6;
                int idx = (int)((float)x / (float)(w - 2) * 39.0f);
                if (idx > 39) idx = 39;
                RECT sr = { rc->left + 1 + x, rc->top + 1, rc->left + 1 + x + seg, rc->bottom - 1 };
                FillRect(hdc, &sr, g_pg_fill[idx]);
            }
        }
    }

    /* bordure */
    FrameRect(hdc, &bg, g_pg_border);
}

/* ------------------------------------------------------------------ */
/* Console DJ locale (mode DJ Mixing)                                  */
/* ------------------------------------------------------------------ */
static void draw_dj_slider(HDC hdc, const RECT* rc, float val, COLORREF fill)
{
    HBRUSH t = CreateSolidBrush(RGB(20, 24, 30));
    FillRect(hdc, rc, t);
    DeleteObject(t);
    RECT f = *rc;
    f.right = f.left + (LONG)((f.right - f.left) * val);
    if (f.right > f.left) {
        HBRUSH fb = CreateSolidBrush(fill);
        FillRect(hdc, &f, fb);
        DeleteObject(fb);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(92, 108, 128));
    HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HPEN op = (HPEN)SelectObject(hdc, pen);
    Rectangle(hdc, rc->left, rc->top, rc->right, rc->bottom);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen);
}

static void draw_dj_button(HDC hdc, const RECT* rc, const wchar_t* label,
                           COLORREF bg)
{
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(hdc, rc, b);
    DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(20, 24, 30));
    HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HPEN op = (HPEN)SelectObject(hdc, pen);
    Rectangle(hdc, rc->left, rc->top, rc->right, rc->bottom);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT lr = *rc;
    DrawTextW(hdc, label, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint_dj_console(HDC hdc, const RECT* rc)
{
    RECT r = *rc;
    /* fond sombre de la console */
    HBRUSH bb = CreateSolidBrush(RGB(20, 24, 30));
    FillRect(hdc, &r, bb);
    DeleteObject(bb);

    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w < 320 || h < 90) return;
    int dw = (w - 60) / 2;
    if (dw > 320) dw = 320;

    g_dj_rc_a.left = r.left + 10;
    g_dj_rc_a.top = r.top + 8;
    g_dj_rc_a.right = g_dj_rc_a.left + dw;
    g_dj_rc_a.bottom = r.bottom - 58;
    g_dj_rc_b.left = r.right - 10 - dw;
    g_dj_rc_b.top = r.top + 8;
    g_dj_rc_b.right = r.right - 10;
    g_dj_rc_b.bottom = r.bottom - 58;

    /* platines */
    HBRUSH deck = CreateSolidBrush(RGB(38, 44, 54));
    FillRect(hdc, &g_dj_rc_a, deck);
    FillRect(hdc, &g_dj_rc_b, deck);
    DeleteObject(deck);

    /* bordures */
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(92, 108, 128));
    HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HPEN op = (HPEN)SelectObject(hdc, pen);
    Rectangle(hdc, g_dj_rc_a.left, g_dj_rc_a.top, g_dj_rc_a.right, g_dj_rc_a.bottom);
    Rectangle(hdc, g_dj_rc_b.left, g_dj_rc_b.top, g_dj_rc_b.right, g_dj_rc_b.bottom);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT of = (HFONT)SelectObject(hdc, f);

    const wchar_t* ta = (g_dj_track_a >= 0 && g_dj_track_a < g_plist_n)
                            ? g_plist[g_dj_track_a] : L"Choisir une piste…";
    const wchar_t* tb = (g_dj_track_b >= 0 && g_dj_track_b < g_plist_n)
                            ? g_plist[g_dj_track_b] : L"Choisir une piste…";

    RECT ta_rc = g_dj_rc_a;
    ta_rc.top += 6;
    ta_rc.bottom = ta_rc.top + 20;
    SetTextColor(hdc, RGB(122, 162, 247));
    DrawTextW(hdc, L"DECK A", -1, &ta_rc, DT_CENTER | DT_TOP);
    ta_rc.top += 18;
    ta_rc.bottom = ta_rc.top + 18;
    SetTextColor(hdc, RGB(232, 238, 244));
    DrawTextW(hdc, ta, -1, &ta_rc, DT_CENTER | DT_TOP | DT_END_ELLIPSIS);

    RECT tb_rc = g_dj_rc_b;
    tb_rc.top += 6;
    tb_rc.bottom = tb_rc.top + 20;
    SetTextColor(hdc, RGB(122, 162, 247));
    DrawTextW(hdc, L"DECK B", -1, &tb_rc, DT_CENTER | DT_TOP);
    tb_rc.top += 18;
    tb_rc.bottom = tb_rc.top + 18;
    SetTextColor(hdc, RGB(232, 238, 244));
    DrawTextW(hdc, tb, -1, &tb_rc, DT_CENTER | DT_TOP | DT_END_ELLIPSIS);

    /* sliders volume */
    g_dj_svol_a.left = g_dj_rc_a.left + 8;
    g_dj_svol_a.right = g_dj_rc_a.right - 8;
    g_dj_svol_a.top = g_dj_rc_a.top + 44;
    g_dj_svol_a.bottom = g_dj_svol_a.top + 10;
    draw_dj_slider(hdc, &g_dj_svol_a, g_djv_a, RGB(47, 111, 228));
    g_dj_svol_b.left = g_dj_rc_b.left + 8;
    g_dj_svol_b.right = g_dj_rc_b.right - 8;
    g_dj_svol_b.top = g_dj_rc_b.top + 44;
    g_dj_svol_b.bottom = g_dj_svol_b.top + 10;
    draw_dj_slider(hdc, &g_dj_svol_b, g_djv_b, RGB(47, 111, 228));

    /* boutons ▶ ⏸ ■ */
    int bw = (g_dj_rc_a.right - g_dj_rc_a.left - 28) / 3;
    if (bw < 20) bw = 20;
    int by = g_dj_rc_a.bottom - 36;
    g_dj_bplay_a.left = g_dj_rc_a.left + 8;
    g_dj_bplay_a.top = by;
    g_dj_bplay_a.right = g_dj_bplay_a.left + bw;
    g_dj_bplay_a.bottom = by + 26;
    g_dj_bpause_a.left = g_dj_bplay_a.right + 6;
    g_dj_bpause_a.top = by;
    g_dj_bpause_a.right = g_dj_bpause_a.left + bw;
    g_dj_bpause_a.bottom = by + 26;
    g_dj_bstop_a.left = g_dj_bpause_a.right + 6;
    g_dj_bstop_a.top = by;
    g_dj_bstop_a.right = g_dj_bstop_a.left + bw;
    g_dj_bstop_a.bottom = by + 26;
    draw_dj_button(hdc, &g_dj_bplay_a, L"▶", RGB(47, 111, 228));
    draw_dj_button(hdc, &g_dj_bpause_a, L"⏸", RGB(150, 120, 60));
    draw_dj_button(hdc, &g_dj_bstop_a, L"■", RGB(180, 64, 58));

    g_dj_bplay_b.left = g_dj_rc_b.left + 8;
    g_dj_bplay_b.top = by;
    g_dj_bplay_b.right = g_dj_bplay_b.left + bw;
    g_dj_bplay_b.bottom = by + 26;
    g_dj_bpause_b.left = g_dj_bplay_b.right + 6;
    g_dj_bpause_b.top = by;
    g_dj_bpause_b.right = g_dj_bpause_b.left + bw;
    g_dj_bpause_b.bottom = by + 26;
    g_dj_bstop_b.left = g_dj_bpause_b.right + 6;
    g_dj_bstop_b.top = by;
    g_dj_bstop_b.right = g_dj_bstop_b.left + bw;
    g_dj_bstop_b.bottom = by + 26;
    draw_dj_button(hdc, &g_dj_bplay_b, L"▶", RGB(47, 111, 228));
    draw_dj_button(hdc, &g_dj_bpause_b, L"⏸", RGB(150, 120, 60));
    draw_dj_button(hdc, &g_dj_bstop_b, L"■", RGB(180, 64, 58));

    /* crossfader */
    g_dj_sxf.left = r.left + 100;
    g_dj_sxf.right = r.right - 100;
    g_dj_sxf.top = r.bottom - 42;
    g_dj_sxf.bottom = g_dj_sxf.top + 10;
    draw_dj_slider(hdc, &g_dj_sxf, g_djxf, RGB(230, 126, 34));
    SetTextColor(hdc, RGB(232, 238, 244));
    RECT xa = { r.left + 8, g_dj_sxf.top - 3, r.left + 96, g_dj_sxf.bottom + 3 };
    DrawTextW(hdc, L"A", -1, &xa, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT xb = { r.right - 96, g_dj_sxf.top - 3, r.right - 8, g_dj_sxf.bottom + 3 };
    DrawTextW(hdc, L"B", -1, &xb, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* pitch (vitesse globale) */
    if (h > 130) {
        g_dj_spitch.left = r.left + 100;
        g_dj_spitch.right = r.right - 100;
        g_dj_spitch.top = r.bottom - 22;
        g_dj_spitch.bottom = g_dj_spitch.top + 10;
        float sp = (cc_speed() - 0.5f) / 1.5f;
        if (sp < 0.0f) sp = 0.0f; else if (sp > 1.0f) sp = 1.0f;
        draw_dj_slider(hdc, &g_dj_spitch, sp, RGB(122, 162, 247));
        SetTextColor(hdc, RGB(232, 238, 244));
        RECT xp = { r.left + 8, g_dj_spitch.top - 3, r.right - 8, g_dj_spitch.bottom + 3 };
        DrawTextW(hdc, L"Pitch", -1, &xp, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, of);
}

/* Applique une valeur à un slider DJ selon sa position */
static void dj_drag_set(int which, POINT pt)
{
    RECT* rc = which == 1 ? &g_dj_svol_a : which == 2 ? &g_dj_svol_b
              : which == 3 ? &g_dj_sxf : &g_dj_spitch;
    int span = rc->right - rc->left;
    if (span <= 0) return;
    float v = (float)(pt.x - rc->left) / (float)span;
    if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
    if (which == 1)      { g_djv_a = v; cc_cmd_val("dj_vol_a", v); }
    else if (which == 2) { g_djv_b = v; cc_cmd_val("dj_vol_b", v); }
    else if (which == 3) { g_djxf = v;  cc_cmd_val("dj_xf", v); }
    else                 cc_cmd_val("speed", 0.5f + v * 1.5f);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Sélecteur de piste d'une platine (menu popup façon "select") */
static void dj_pick_track(HWND hwnd, int deck)
{
    HMENU m = CreatePopupMenu();
    int n = g_plist_n;
    if (n > 300) n = 300;
    for (int i = 0; i < n; i++) {
        const wchar_t* s = g_plist[i];
        const wchar_t* base = wcsrchr(s, L'\\');
        base = base ? base + 1 : s;
        wchar_t label[64];
        wcsncpy(label, base, 62);
        label[62] = 0;
        AppendMenuW(m, MF_STRING, 1000 + i, label);
    }
    POINT pt;
    GetCursorPos(&pt);
    int r = (int)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (r >= 1000 && r < 1000 + g_plist_n) {
        int idx = r - 1000;
        if (deck == 0) g_dj_track_a = idx;
        else           g_dj_track_b = idx;
        InvalidateRect(hwnd, NULL, TRUE);
        status_update();
    }
}

static BOOL CALLBACK fsview_enum(HMONITOR mon, HDC hdc, LPRECT rc, LPARAM lp);
static void toggle_fullscreen(HWND hwnd);
static void fsview_paint_playlist(HDC hdc, int w, int h);
static void fsview_paint_lyrics(HDC hdc, int w, int h);
static void fsview_paint_cover(HDC hdc, int w, int h);

static void paint_center(HDC hdc, RECT* rc)
{
    RECT vis_rc, bar_rc, ctrl_rc;
    if (g_fullscreen) {
        /* plein écran : la zone visuelle occupe tout */
        vis_rc = *rc;
        bar_rc.left = bar_rc.right = ctrl_rc.left = ctrl_rc.right = 0;
        bar_rc.top = bar_rc.bottom = ctrl_rc.top = ctrl_rc.bottom = 0;
    } else if (g_skin_ctrl_top) {
        /* contrôles en haut (à la place du menu), progression dessous */
        ctrl_rc = *rc;
        ctrl_rc.bottom = rc->top + CTRL_H;
        bar_rc = *rc;
        bar_rc.top = ctrl_rc.bottom;
        bar_rc.bottom = ctrl_rc.bottom + PROGRESS_H;
        vis_rc = *rc;
        vis_rc.top = bar_rc.bottom;
    } else {
        /* contrôles en bas, progression au-dessus, visuel au-dessus */
        ctrl_rc = *rc;
        ctrl_rc.top = rc->bottom - CTRL_H;
        bar_rc = *rc;
        bar_rc.top = ctrl_rc.top - PROGRESS_H;
        bar_rc.bottom = ctrl_rc.top;
        vis_rc = *rc;
        vis_rc.bottom = bar_rc.top - 2;
    }

    /* zone du visualiseur : imposée par le skin (coordonnées client
     * converties en coordonnées locales du DC mémoire), sinon zone de
     * contenu */
    if (g_skin_vis.left >= 0) {
        RECT cont;
        get_content_rect(g_hwnd, &cont);
        vis_rc.left   = g_skin_vis.left   - cont.left;
        vis_rc.top    = g_skin_vis.top    - cont.top;
        vis_rc.right  = g_skin_vis.right  - cont.left;
        vis_rc.bottom = g_skin_vis.bottom - cont.top;
    }

    /* mode DJ : console de mixage locale (synchro avec la page web) —
     * la console occupe toute la zone centrale (le skin n'impose pas
     * sa zone de visualiseur en mode DJ) */
    if (g_dj_mode) {
        RECT dj_rc = *rc;
        if (!g_skin_ctrl_top)
            dj_rc.bottom -= (CTRL_H + PROGRESS_H);
        paint_dj_console(hdc, &dj_rc);
        return;
    }

    /* plein écran : le 1er écran peut afficher autre chose que le visuel */
    if (g_fullscreen && g_fs_mode[0] != 0) {
        int w = vis_rc.right - vis_rc.left;
        int h = vis_rc.bottom - vis_rc.top;
        if (g_fs_mode[0] == 1) {
            fsview_paint_playlist(hdc, w, h);
            return;
        }
        if (g_fs_mode[0] == 2) {
            fsview_paint_lyrics(hdc, w, h);
            return;
        }
        if (g_fs_mode[0] == 3) {
            HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
            FillRect(hdc, &vis_rc, bg);
            DeleteObject(bg);
            fsview_paint_cover(hdc, w, h);
            return;
        }
    }

    /* un plugin visuel actif remplace le texte par son rendu */
    if (mp_plugins_has_visual()) {
        mp_plugins_visual_render(hdc, vis_rc.right - vis_rc.left, vis_rc.bottom - vis_rc.top);
    } else {
        SetBkMode(hdc, TRANSPARENT);
        const char* fn = cc_name();
        static const char* state_keys[] = { "center_stopped", "center_playing", "center_paused", "center_finished" };
        const wchar_t* st = lang_get(state_keys[cc_st()]);

        HFONT big = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT small = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        if (fn) {
            const char* base = strrchr(fn, '\\');
            base = base ? base + 1 : fn;
            wchar_t base_w[280];
            utf8_to_wide(base, base_w, 280);
            HFONT old = (HFONT)SelectObject(hdc, big);
            SetTextColor(hdc, g_skin.text);
            RECT r = vis_rc;
            DrawTextW(hdc, base_w, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, old);
        }
        {
            HFONT old = (HFONT)SelectObject(hdc, small);
            SetTextColor(hdc, g_skin.text);
            RECT r = vis_rc;
            r.top = vis_rc.top + 38;
            DrawTextW(hdc, st, -1, &r, DT_CENTER | DT_TOP | DT_SINGLELINE);
            SelectObject(hdc, old);
        }

        DeleteObject(big);
        DeleteObject(small);
    }

    if (!g_fullscreen) {
        draw_progress_bar(hdc, &bar_rc);
        paint_controls(hdc, rc);
    }
}

/* ------------------------------------------------------------------ */
/* Fenêtres plein écran par écran (visuel, playlist, lyrics, jaquette) */
/* ------------------------------------------------------------------ */
static void fsview_paint_cover(HDC hdc, int w, int h)
{
    const char* name = cc_name();
    if (!name) return;
    size_t len = 0;
    const unsigned char* img = host_get_cover(name, &len);
    if (!img || !len) return;
    GpImage* gp = NULL;
    IStream* st = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &st) != S_OK) return;
    ULONG written = 0;
    st->lpVtbl->Write(st, img, (ULONG)len, &written);
    if (GdipCreateBitmapFromStream(st, &gp) != Ok) {
        st->lpVtbl->Release(st);
        return;
    }
    st->lpVtbl->Release(st);
    GpGraphics* g = NULL;
    if (GdipCreateFromHDC(hdc, &g) == Ok) {
        UINT iw = 0, ih = 0;
        GdipGetImageWidth(gp, &iw);
        GdipGetImageHeight(gp, &ih);
        if (iw > 0 && ih > 0) {
            int maxw = w - 80, maxh = h - 80;
            int dw = (int)iw, dh = (int)ih;
            if (dw > maxw) { dh = dh * maxw / dw; dw = maxw; }
            if (dh > maxh) { dw = dw * maxh / dh; dh = maxh; }
            int x = (w - dw) / 2, y = (h - dh) / 2;
            GdipDrawImageRectI(g, gp, x, y, dw, dh);
        }
        GdipDeleteGraphics(g);
    }
    GdipDisposeImage(gp);
}

static void fsview_paint_playlist(HDC hdc, int w, int h)
{
    HBRUSH bg = CreateSolidBrush(RGB(16, 20, 26));
    RECT r = { 0, 0, w, h };
    FillRect(hdc, &r, bg);
    DeleteObject(bg);
    HFONT f = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    int idx = g_plist_idx;
    int y = 40;
    int start = idx - 12;
    if (start < 0) start = 0;
    int end = start + 26;
    if (end > g_plist_n) end = g_plist_n;
    for (int i = start; i < end; i++) {
        RECT lr = { 60, y, w - 40, y + 30 };
        if (i == idx) {
            HBRUSH sel = CreateSolidBrush(RGB(38, 48, 64));
            RECT sr = { 40, y, w - 40, y + 30 };
            FillRect(hdc, &sr, sel);
            DeleteObject(sel);
            SetTextColor(hdc, RGB(122, 162, 247));
        } else {
            SetTextColor(hdc, RGB(210, 216, 224));
        }
        wchar_t num[16];
        wsprintfW(num, L"%d.", i + 1);
        DrawTextW(hdc, num, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        lr.left = 100;
        DrawTextW(hdc, g_plist[i], -1, &lr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += 30;
    }
    SelectObject(hdc, old);
    DeleteObject(f);
}

static void fsview_paint_lyrics(HDC hdc, int w, int h)
{
    HBRUSH bg = CreateSolidBrush(RGB(16, 20, 26));
    RECT r = { 0, 0, w, h };
    FillRect(hdc, &r, bg);
    DeleteObject(bg);
    const char* fn = cc_name();
    if (!fn) return;
    char lrc[MAX_PATH];
    _snprintf(lrc, sizeof(lrc), "%s", fn);
    char* dot = strrchr(lrc, '.');
    if (dot) _snprintf(dot, sizeof(lrc) - (size_t)(dot - lrc), ".lrc");
    FILE* f = fopen(lrc, "r");
    if (!f) return;
    wchar_t lines[64][128];
    int n = 0;
    char buf[256];
    while (n < 64 && fgets(buf, sizeof(buf), f)) {
        /* retire le timestamp [mm:ss.xx] */
        char* p = strchr(buf, ']');
        char* txt = p ? p + 1 : buf;
        while (*txt == ' ' || *txt == '\t') txt++;
        size_t l = strlen(txt);
        while (l > 0 && (txt[l - 1] == '\n' || txt[l - 1] == '\r'))
            txt[--l] = 0;
        if (!txt[0]) continue;
        utf8_to_wide(txt, lines[n], 128);
        n++;
    }
    fclose(f);
    HFONT f2 = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, f2);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(210, 216, 224));
    int y = (h - n * 34) / 2;
    if (y < 30) y = 30;
    for (int i = 0; i < n; i++) {
        RECT lr = { 80, y, w - 80, y + 34 };
        DrawTextW(hdc, lines[i], -1, &lr,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += 34;
    }
    SelectObject(hdc, old);
    DeleteObject(f2);
}

static LRESULT CALLBACK fsview_proc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_TIMER:
        InvalidateRect(hw, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        int mode = (int)GetWindowLongPtrW(hw, GWLP_USERDATA);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (mode == 1) {
            fsview_paint_playlist(hdc, w, h);
        } else if (mode == 2) {
            fsview_paint_lyrics(hdc, w, h);
        } else if (mode == 3) {
            HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            fsview_paint_cover(hdc, w, h);
        } else {
            HBRUSH bg = CreateSolidBrush(RGB(8, 10, 14));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            if (mp_plugins_has_visual())
                mp_plugins_visual_render(hdc, w, h);
        }
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            toggle_fullscreen(g_hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        toggle_fullscreen(g_hwnd);
        return 0;
    case WM_DESTROY:
        for (int i = 0; i < 4; i++)
            if (g_fs_wins[i] == hw) g_fs_wins[i] = NULL;
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

static void fsview_close_all(void)
{
    for (int i = 0; i < 4; i++) {
        if (g_fs_wins[i]) {
            DestroyWindow(g_fs_wins[i]);
            g_fs_wins[i] = NULL;
        }
    }
    g_fs_win_count = 0;
}

static void fsview_open_all(int nscreens)
{
    /* la fenêtre principale occupe le moniteur où elle se trouve ;
     * les fenêtres annexes couvrent les autres moniteurs, chacune avec
     * le contenu configuré pour son écran (g_fs_mode[1..n]) */
    (void)nscreens;
    WNDCLASSW wc;
    if (!GetClassInfoW(GetModuleHandleW(NULL), L"MusicPlayerFsView", &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = fsview_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"MusicPlayerFsView";
        RegisterClassW(&wc);
    }
    g_fs_win_count = 0;
    EnumDisplayMonitors(NULL, NULL, fsview_enum, 0);
}

static BOOL CALLBACK fsview_enum(HMONITOR mon, HDC hdc, LPRECT rc, LPARAM lp)
{
    (void)hdc;
    (void)rc;
    (void)lp;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    /* pas de fenêtre annexe sur le moniteur de la fenêtre principale */
    HMONITOR main_mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    if (mon == main_mon) return TRUE;
    if (g_fs_win_count >= g_fs_screens - 1) return FALSE;
    int mode = g_fs_mode[1 + g_fs_win_count];
    HWND w = CreateWindowExW(WS_EX_TOPMOST, L"MusicPlayerFsView", L"",
                             WS_POPUP,
                             mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (w) {
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)mode);
        ShowWindow(w, SW_SHOW);
        SetFocus(w);
        SetTimer(w, 1, 33, NULL);
        g_fs_wins[g_fs_win_count] = w;
        g_fs_win_count++;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Plein écran                                                         */
/* ------------------------------------------------------------------ */
static void toggle_fullscreen(HWND hwnd)
{
    if (!g_fullscreen) {
        GetWindowRect(hwnd, &g_win_normal);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP);
        SetWindowLongW(hwnd, GWL_EXSTYLE,
                       GetWindowLongW(hwnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
        /* le vrai plein écran : le moniteur sous le curseur */
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        int sw = mi.rcMonitor.right - mi.rcMonitor.left;
        int sh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowPos(hwnd, HWND_TOPMOST,
                     mi.rcMonitor.left, mi.rcMonitor.top, sw, sh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 1;
        ShowWindow(g_status, SW_HIDE);
        /* écrans annexes */
        if (g_fs_screens >= 2) fsview_open_all(g_fs_screens);
    } else {
        fsview_close_all();
        g_fs_win = NULL;
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        SetWindowLongW(hwnd, GWL_EXSTYLE,
                       GetWindowLongW(hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
        SetWindowPos(hwnd, HWND_NOTOPMOST,
                     g_win_normal.left, g_win_normal.top,
                     g_win_normal.right - g_win_normal.left,
                     g_win_normal.bottom - g_win_normal.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 0;
        ShowWindow(g_status, SW_SHOW);
    }
    status_update();
}

/* ------------------------------------------------------------------ */
/* Fenêtre Playlist : liste des morceaux + sélection au clic           */
/* ------------------------------------------------------------------ */
static HWND g_plist_win = NULL;
static HWND g_plist_lv = NULL;

static LRESULT CALLBACK plist_wnd_proc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_DESTROY:
        g_plist_win = NULL;
        g_plist_lv = NULL;
        return 0;
    case WM_CLOSE:
        DestroyWindow(hw);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 1) {   /* bouton Fermer */
            DestroyWindow(hw);
            return 0;
        }
        break;
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (g_plist_lv)
            SetWindowPos(g_plist_lv, NULL, 6, 6, w - 12, h - 44, SWP_NOZORDER);
        HWND b = GetDlgItem(hw, 1);
        if (b) SetWindowPos(b, NULL, 6, h - 34, 90, 26, SWP_NOZORDER);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->hwndFrom == g_plist_lv && nm->code == NM_DBLCLK) {
            int i = ListView_GetNextItem(g_plist_lv, -1, LVNI_SELECTED);
            if (i >= 0 && i < g_plist_n) playlist_play_index(i);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            int i = ListView_GetNextItem(g_plist_lv, -1, LVNI_SELECTED);
            if (i >= 0 && i < g_plist_n) playlist_play_index(i);
            return 0;
        }
        if (wp == VK_ESCAPE) { DestroyWindow(hw); return 0; }
        break;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

static void playlist_win_rebuild(void)
{
    if (!g_plist_win || !g_plist_lv) return;
    ListView_DeleteAllItems(g_plist_lv);
    for (int i = 0; i < g_plist_n; i++) {
        wchar_t num[16];
        swprintf(num, 16, L"%d", i + 1);
        LVITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = num;
        ListView_InsertItem(g_plist_lv, &it);
        ListView_SetItemText(g_plist_lv, i, 1, (wchar_t*)host_plist_name(i));
    }
    playlist_win_highlight();
}

static void playlist_win_highlight(void)
{
    if (!g_plist_win || !g_plist_lv) return;
    if (g_plist_idx >= 0 && g_plist_idx < g_plist_n) {
        ListView_SetItemState(g_plist_lv, g_plist_idx,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_plist_lv, g_plist_idx, FALSE);
    }
}

static void toggle_playlist_win(void)
{
    if (g_plist_win) { DestroyWindow(g_plist_win); return; }
    HINSTANCE inst = GetModuleHandleW(NULL);
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = plist_wnd_proc;
        wc.hInstance = inst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MPPlaylist";
        RegisterClassW(&wc);
        reg = 1;
    }
    g_plist_win = CreateWindowExW(0, L"MPPlaylist", L"Playlist",
                                  WS_OVERLAPPEDWINDOW, 700, 120, 320, 420,
                                  NULL, NULL, inst, NULL);
    if (!g_plist_win) return;
    g_plist_lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                 6, 6, 300, 370, g_plist_win, NULL, inst, NULL);
    ListView_SetExtendedListViewStyle(g_plist_lv,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW c0, c1;
    memset(&c0, 0, sizeof(c0));
    c0.mask = LVCF_TEXT | LVCF_WIDTH;
    c0.cx = 34; c0.pszText = L"#";
    ListView_InsertColumn(g_plist_lv, 0, &c0);
    memset(&c1, 0, sizeof(c1));
    c1.mask = LVCF_TEXT | LVCF_WIDTH;
    c1.cx = 240; c1.pszText = L"Title";
    ListView_InsertColumn(g_plist_lv, 1, &c1);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  6, 380, 90, 26, g_plist_win, (HMENU)1, inst, NULL);
    playlist_win_rebuild();
    ShowWindow(g_plist_win, SW_SHOW);
}

/* ------------------------------------------------------------------ */
/* Gestion souris : boutons + curseur de volume                        */
/* ------------------------------------------------------------------ */
static void mouse_down(HWND hwnd, int x, int y)
{
    if (g_fullscreen) return;
    RECT rc;
    get_content_rect(hwnd, &rc);
    layout_controls(&rc);

    /* mode DJ : boutons des platines, sliders, sélection des pistes */
    if (g_dj_mode) {
        POINT pt = { x, y };
        if (PtInRect(&g_dj_bplay_a, pt)) {
            if (g_dj_track_a >= 0 && g_dj_track_a < g_plist_n)
                playlist_play_index(g_dj_track_a);
        } else if (PtInRect(&g_dj_bpause_a, pt)) {
            client_play_pause();
        } else if (PtInRect(&g_dj_bstop_a, pt)) {
            client_transport("stop", 1);
        } else if (PtInRect(&g_dj_bplay_b, pt)) {
            if (g_dj_track_b >= 0 && g_dj_track_b < g_plist_n) {
                char utf8[MAX_PATH * 3];
                wide_to_utf8(g_plist[g_dj_track_b], utf8, sizeof(utf8));
                cc_cmd_path("dj_open_b", utf8);
            }
        } else if (PtInRect(&g_dj_bpause_b, pt)) {
            cc_cmd("dj_play_b");
        } else if (PtInRect(&g_dj_bstop_b, pt)) {
            cc_cmd("dj_stop_b");
        } else if (PtInRect(&g_dj_svol_a, pt)) {
            g_dj_drag = 1;
            dj_drag_set(1, pt);
        } else if (PtInRect(&g_dj_svol_b, pt)) {
            g_dj_drag = 2;
            dj_drag_set(2, pt);
        } else if (PtInRect(&g_dj_sxf, pt)) {
            g_dj_drag = 3;
            dj_drag_set(3, pt);
        } else if (PtInRect(&g_dj_spitch, pt)) {
            g_dj_drag = 4;
            dj_drag_set(4, pt);
        } else if (PtInRect(&g_dj_rc_a, pt)) {
            dj_pick_track(hwnd, 0);
        } else if (PtInRect(&g_dj_rc_b, pt)) {
            dj_pick_track(hwnd, 1);
        }
        return;
    }

    /* clic sur la barre de progression : aller directement au moment
     * choisi (la barre est juste au-dessus de la rangée de boutons) */
    if (y >= rc.bottom - CTRL_H - PROGRESS_H && y < rc.bottom - CTRL_H) {
        double dur = cc_dur();
        int w = rc.right - rc.left;
        if (dur > 0.0 && w > 0) {
            double ratio = (double)(x - rc.left) / (double)w;
            if (ratio < 0.0) ratio = 0.0;
            if (ratio > 1.0) ratio = 1.0;
            sp_flush();
            cc_cmd_val("seek", ratio * dur);
            cc_poll();
            status_update();
        }
        return;
    }

    if (x >= g_rc_play.left && x <= g_rc_play.right &&
        y >= g_rc_play.top && y <= g_rc_play.bottom) {
        client_play_pause();
        status_update();
    } else if (x >= g_rc_stop.left && x <= g_rc_stop.right &&
               y >= g_rc_stop.top && y <= g_rc_stop.bottom) {
        client_transport("stop", 1);
        status_update();
    } else if (x >= g_rc_next.left && x <= g_rc_next.right &&
               y >= g_rc_next.top && y <= g_rc_next.bottom) {
        playlist_next();
    } else if (x >= g_rc_shuffle.left && x <= g_rc_shuffle.right &&
               y >= g_rc_shuffle.top && y <= g_rc_shuffle.bottom) {
        playlist_set_shuffle(!playlist_get_shuffle());
        InvalidateRect(hwnd, NULL, TRUE);
    } else if (x >= g_rc_plist.left && x <= g_rc_plist.right &&
               y >= g_rc_plist.top && y <= g_rc_plist.bottom) {
        toggle_playlist_win();
    } else if (x >= g_rc_fs.left && x <= g_rc_fs.right &&
               y >= g_rc_fs.top && y <= g_rc_fs.bottom) {
        toggle_fullscreen(hwnd);
    } else if (x >= g_rc_vol.left - 14 && x <= g_rc_vol.right &&
               y >= g_rc_vol.top - 4 && y <= g_rc_vol.bottom + 4) {
        g_vol_drag = 1;
        SetCapture(hwnd);
        vol_from_mouse(x);
    }
}

static void mouse_move(int x)
{
    if (g_vol_drag) vol_from_mouse(x);
}

static void mouse_up(void)
{
    if (g_vol_drag) {
        g_vol_drag = 0;
        ReleaseCapture();
    }
}

/* ------------------------------------------------------------------ */
/* Préférence de langue (persistée dans %APPDATA%\MusicPlayer)         */
/* ------------------------------------------------------------------ */
static void lang_pref_path(wchar_t* out, int out_chars)
{
    wchar_t appdata[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
        swprintf(out, out_chars, L"%ls\\MusicPlayer", appdata);
        CreateDirectoryW(out, NULL);
        wcscat(out, L"\\lang.txt");
    } else {
        out[0] = 0;
    }
}

static void lang_pref_save(const wchar_t* code)
{
    wchar_t path[MAX_PATH];
    lang_pref_path(path, MAX_PATH);
    if (!path[0]) return;
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        char buf[16] = { 0 };
        WideCharToMultiByte(CP_UTF8, 0, code, -1, buf, 16, NULL, NULL);
        fwrite(buf, 1, strlen(buf), f);
        fclose(f);
    }
}

static void lang_pref_load(void)
{
    wchar_t path[MAX_PATH];
    lang_pref_path(path, MAX_PATH);
    if (!path[0]) return;
    FILE* f = _wfopen(path, L"rb");
    if (f) {
        char buf[16] = { 0 };
        fread(buf, 1, 15, f);
        fclose(f);
        wchar_t code[8] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, code, 8);
        if (code[0]) lang_set(code);
    }
}

/* ------------------------------------------------------------------ */
/* Gestion des commandes                                               */
/* ------------------------------------------------------------------ */
static void on_command(int id, HMENU bar)
{
    (void)bar;
    switch (id) {
    case IDM_OPEN:      do_open_dialog(); break;
    case IDM_OPEN_FOLDER: do_open_folder_dialog(); break;
    case IDM_OPEN_CD:   do_open_cd(); break;
    case IDM_EXIT:      SendMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    case IDM_PLAYPAUSE:
        if (g_cd_mode) {
            if (cd_playing()) cd_pause();
            else if (cd_paused()) cd_resume();
            else if (g_plist_idx >= 0) cd_play(g_plist_idx + 1);
            status_update();
        } else {
            client_play_pause();
        }
        break;
    case IDM_STOP:
        if (g_cd_mode) cd_stop();
        else client_transport("stop", 1);
        status_update();
        break;
    case IDM_NEXT:      playlist_next(); break;
    case IDM_PREV:      playlist_prev(); break;
    case IDM_SHUFFLE:
        playlist_set_shuffle(!playlist_get_shuffle());
        CheckMenuItem(GetSubMenu(menu_bar(), 0), IDM_SHUFFLE,
                      MF_BYCOMMAND | (g_shuffle ? MF_CHECKED : MF_UNCHECKED));
        status_update();
        break;
    case IDM_FULLSCREEN: toggle_fullscreen(g_hwnd); break;
    case IDM_DJ_MODE:
        g_dj_mode = !g_dj_mode;
        if (g_dj_mode) {
            g_dj_track_a = g_plist_idx >= 0 ? g_plist_idx : 0;
            g_dj_track_b = (g_plist_idx + 1 < g_plist_n) ? g_plist_idx + 1 : 0;
            g_djv_a = 1.0f; g_djv_b = 1.0f; g_djxf = 0.5f;
            cc_cmd_val("dj_vol_a", 1.0f);
            cc_cmd_val("dj_vol_b", 1.0f);
            cc_cmd_val("dj_xf", 0.5f);
        } else {
            cc_cmd("dj_stop_b");
        }
        InvalidateRect(g_hwnd, NULL, FALSE);
        status_update();
        break;
    case IDM_INTERFACE:
        do_interface_dialog();
        break;
    case IDM_UPDATE_CFG:
        do_update_cfg_dialog();
        break;
    case IDM_NETWORK:
        do_net_dialog();
        break;
    case IDM_REPO:
        do_repo_dialog();
        break;
    case IDM_PODCASTS:
        do_podcast_dialog();
        break;
    case IDM_WEB_SERVER:
        do_web_dialog();
        break;
    case IDM_ABOUT:     do_about(); break;
    case IDM_LOGS:      do_logs_dialog(); break;

    case IDM_VOL_UP: {
        float v = sp_get_volume() + 0.05f;
        if (v > 2.0f) v = 2.0f;
        sp_set_volume(v);
        status_update();
        break;
    }
    case IDM_VOL_DOWN: {
        float v = sp_get_volume() - 0.05f;
        if (v < 0.0f) v = 0.0f;
        sp_set_volume(v);
        status_update();
        break;
    }
    case IDM_PLUGIN_RELOAD:
        mp_plugins_scan(g_plugins_dir, g_skins_dir, &g_host, 0);
        mp_plugins_apply_skins(g_hwnd);
        rebuild_plugins_menu(menu_bar());
        break;

    default:
        if (id >= IDM_SPEED_BASE && id < IDM_SPEED_BASE + SPEED_COUNT) {
            cc_cmd_val("speed", SPEED_VALUES[id - IDM_SPEED_BASE]);
            refresh_speed_check(GetSubMenu(GetSubMenu(menu_bar(), 1), 0));
        } else if (id == IDM_PLUGIN_CFG) {
            do_plugins_dialog();
        } else if (id >= IDM_PLUGIN_BASE && id < IDM_LANG_BASE) {
            int i = id - IDM_PLUGIN_BASE;
            mp_plugin* p = mp_plugins_get(i);
            if (p) {
                unsigned t = p->api->type();
                if (t & MP_PLUGIN_VISUAL) {
                    /* radio : un seul visuel actif (re-clic = aucun) */
                    int was_active = p->enabled;
                    for (int j = 0; j < mp_plugins_count(); j++) {
                        mp_plugin* q = mp_plugins_get(j);
                        if (q->api->type() & MP_PLUGIN_VISUAL)
                            mp_plugins_set_enabled(j, !was_active && j == i);
                    }
                } else if (t & MP_PLUGIN_SKIN) {
                    /* radio : un seul skin actif (re-clic = palette par défaut) */
                    int was_active = p->enabled;
                    for (int j = 0; j < mp_plugins_count(); j++) {
                        mp_plugin* q = mp_plugins_get(j);
                        if (q->api->type() & MP_PLUGIN_SKIN)
                            mp_plugins_set_enabled(j, !was_active && j == i);
                    }
                } else {
                    mp_plugins_set_enabled(i, !p->enabled);
                }
                mp_plugins_apply_skins(g_hwnd);
                rebuild_plugins_menu(menu_bar());
                /* un service peut avoir une action au clic (ex. Lyrics) */
                if ((t & MP_PLUGIN_SERVICE) && p->api->service) {
                    p->api->service(p, MP_SERVICE_CLICK, NULL);
                    mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL);
    cc_push_web_config(g_cfg.web_enabled, g_cfg.web_port, g_cfg.web_ips);
                }
            }
        }
        break;
    }
    status_update();
}

/* ------------------------------------------------------------------ */
/* Fenêtre principale                                                  */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        status_init(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, 1, 250, NULL);   /* status bar */
        SetTimer(hwnd, 2, 33, NULL);    /* rendu visuel ~30 FPS */
        if (mp_update_get_mode() == 1 || mp_update_get_mode() == 3)
            SetTimer(hwnd, 3, 4000, NULL); /* vérif. mises à jour au démarrage */
        if (mp_update_get_mode() == 3)
            SetTimer(hwnd, 4, 3600000, NULL); /* mode autonome : toutes les heures */
        /* icône de l'application */
        HICON hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCE(1));
        if (hIcon) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t path_w[MAX_PATH];
        DragQueryFileW((HDROP)wp, 0, path_w, MAX_PATH);
        DragFinish((HDROP)wp);
        if (GetFileAttributesW(path_w) & FILE_ATTRIBUTE_DIRECTORY) {
            if (playlist_open_folder(path_w) != 0) {
                wchar_t msg[600];
                swprintf(msg, 600, lang_get("err_folder"), path_w);
                MessageBoxW(hwnd, msg, APP_TITLE, MB_ICONERROR);
            }
        } else {
            char path_utf8[MAX_PATH * 3];
            wide_to_utf8(path_w, path_utf8, sizeof(path_utf8));
            if (cc_open(path_utf8) != 0) {
                wchar_t msg[600];
                swprintf(msg, 600, lang_get("err_open"), path_w);
                MessageBoxW(hwnd, msg, APP_TITLE, MB_ICONERROR);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE:   client_play_pause(); break;
        case 'S':        client_transport("stop", 1); break;
        case 'N':        playlist_next(); break;
        case VK_UP:      SendMessageW(hwnd, WM_COMMAND, IDM_VOL_UP, 0); break;
        case VK_DOWN:    SendMessageW(hwnd, WM_COMMAND, IDM_VOL_DOWN, 0); break;
        case VK_F11:     toggle_fullscreen(hwnd); break;
        case VK_ESCAPE:
            if (g_fullscreen) toggle_fullscreen(hwnd);
            break;
        case 'O':
            if (GetKeyState(VK_CONTROL) & 0x8000) do_open_dialog();
            break;
        case 'L':
            toggle_playlist_win();
            break;
        }
        status_update();
        return 0;
    case WM_LBUTTONDOWN:
        mouse_down(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (g_dj_drag > 0) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            dj_drag_set(g_dj_drag, pt);
            return 0;
        }
        if (g_vol_drag) mouse_move(GET_X_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        g_dj_drag = -1;
        mouse_up();
        return 0;
    case WM_TIMER:
        if (wp == 2) {
            /* CD audio : fin de piste → piste suivante */
            if (g_cd_mode) {
                if (g_cd_was_playing && !cd_playing() && !cd_paused())
                    playlist_next();
                g_cd_was_playing = cd_playing();
            }
            /* rafraîchit la zone visuelle uniquement si un plugin visuel
               est actif (sinon le timer ne fait presque rien) */
            if (mp_plugins_has_visual()) {
                RECT rc;
                get_content_rect(hwnd, &rc);
                InvalidateRect(hwnd, &rc, FALSE);
            }
            return 0;
        }
        if (wp == 3) {
            /* vérification automatique des mises à jour (one-shot) */
            KillTimer(hwnd, 3);
            mp_update_check_async(hwnd, 0);
            return 0;
        }
        if (wp == 4) {
            /* mode autonome : vérification toutes les heures */
            mp_update_check_async(hwnd, 0);
            return 0;
        }
        if (wp == 1) {
            /* état du moteur (client/serveur) ; l'enchaînement des
             * morceaux est géré par le CORE (son playlist_tick) */
            cc_poll();
        }
        status_update();
        return 0;

    case MP_UPDATE_DONE: {
        /* résultat de la vérification de mises à jour
         * wp = manuel (1) / automatique (0) ; lp = 0 à jour, 1 dispo, 2 erreur */
        int manual = (int)wp;
        int state = (int)lp;
        if (state == 1 && !manual && mp_update_get_mode() == 3) {
            /* mode autonome : appliquer et redémarrer sans demander */
            if (mp_update_apply_and_restart() == 0) {
                PostQuitMessage(0);   /* le script relancera le programme */
                return 0;
            }
        }
        if (state == 1) {
            /* avertissement : mise à jour maintenant, plus tard,
             * ou ignorer cette version (seules les suivantes seront
             * proposées) */
            int r = (int)DialogBoxParamW(GetModuleHandleW(NULL),
                                         MAKEINTRESOURCEW(IDD_UPDATE),
                                         hwnd, upd_dlg_proc, 0);
            if (r == 3) {
                mp_update_skip(mp_update_latest());
            } else if (r == 1) {
                /* v042-c7 : le chemin manuel utilisait PowerShell
                 * Expand-Archive (apply_update_and_restart) qui échoue
                 * silencieusement quand le core verrouille les DLL des
                 * core_plugins.  On passe par mp_update_apply_and_restart
                 * (tar.exe natif + kill core + vérification d'intégrité)
                 * comme le chemin autonome. */
                if (mp_update_apply_and_restart() == 0) {
                    PostQuitMessage(0);
                    return 0;
                } else {
                    MessageBoxW(hwnd, lang_get("upd_dl_error"),
                                lang_get("upd_title"), MB_OK | MB_ICONERROR);
                }
            }
            /* r == 2 : plus tard */
        } else if (state == 0 && manual) {
            wchar_t msg[256];
            swprintf(msg, 256, lang_get("upd_uptodate"), MP_VERSION);
            MessageBoxW(hwnd, msg, lang_get("upd_title"), MB_OK | MB_ICONINFORMATION);
        } else if (state == 2 && manual) {
            MessageBoxW(hwnd, lang_get("upd_error"), lang_get("upd_title"),
                        MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    case WM_RBUTTONUP:
        show_context_menu(hwnd);
        return 0;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lp;
        if (mi->CtlType == ODT_MENU) {
            menu_measure(mi);
            return TRUE;
        }
        break;
    }
    case WM_INITMENUPOPUP:
        /* état de la commande Aléatoire dans le menu File */
        CheckMenuItem((HMENU)wp, IDM_SHUFFLE,
                      MF_BYCOMMAND | (g_shuffle ? MF_CHECKED : MF_UNCHECKED));
        break;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_MENU) {
            menu_draw(di);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        on_command(LOWORD(wp), GetMenu(hwnd));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        get_content_rect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            /* double buffer : dessin hors écran puis un seul BitBlt
             * (élimine le scintillement des effets visuels) */
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldbmp = (HBITMAP)SelectObject(mem, bmp);
            if (g_skin_bg) {
                /* fond du skin dessiné à sa taille native (1:1) : avec
                 * skin_set_window_size la fenêtre fait exactement cette
                 * taille, donc aucune déformation */
                UINT iw = 0, ih = 0;
                GdipGetImageWidth(g_skin_bg, &iw);
                GdipGetImageHeight(g_skin_bg, &ih);
                if (iw == 0 || ih == 0) { iw = (UINT)w; ih = (UINT)h; }
                GpGraphics* g = NULL;
                if (GdipCreateFromHDC(mem, &g) == Ok) {
                    /* si l'image est plus petite que la zone, combler
                     * d'abord avec la couleur de fond du skin */
                    if ((int)iw < w || (int)ih < h) {
                        RECT rf = { 0, 0, w, h };
                        HBRUSH wb = CreateSolidBrush(g_skin.bg);
                        FillRect(mem, &rf, wb);
                        DeleteObject(wb);
                    }
                    GdipDrawImageRectI(g, g_skin_bg, 0, 0, (INT)iw, (INT)ih);
                    GdipDeleteGraphics(g);
                }
            } else {
                RECT rf = { 0, 0, w, h };
                HBRUSH wb = CreateSolidBrush(g_skin.bg);
                FillRect(mem, &rf, wb);
                DeleteObject(wb);
            }
            RECT rc2 = { 0, 0, w, h };
            paint_center(mem, &rc2);
            BitBlt(hdc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldbmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        /* le double buffer du WM_PAINT couvre TOUTE la fenêtre (fond
         * peint dans le mem DC) : ne rien effacer ici, sinon flash
         * du fond à chaque frame (scintillement) */
        return 1;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        if (g_skin_win_fixed && !g_fullscreen) {
            /* skin à taille fixe : borne haute = borne basse */
            int ch = g_skin_win_h;
            if (g_status && g_skin_status_visible) {
                RECT sr;
                GetWindowRect(g_status, &sr);
                ch += (sr.bottom - sr.top);
            }
            RECT wr = { 0, 0, g_skin_win_w, ch };
            AdjustWindowRectEx(&wr, GetWindowLongW(hwnd, GWL_STYLE),
                               g_skin_menu_visible ? TRUE : FALSE,
                               GetWindowLongW(hwnd, GWL_EXSTYLE));
            mmi->ptMinTrackSize.x = mmi->ptMaxTrackSize.x = wr.right - wr.left;
            mmi->ptMinTrackSize.y = mmi->ptMaxTrackSize.y = wr.bottom - wr.top;
        } else {
            mmi->ptMinTrackSize.x = 420;
            mmi->ptMinTrackSize.y = 260;
        }
        return 0;
    }
    case WM_SIZE: {
        /* redimensionnement : recalcule les parts de la status bar
         * et redessine toute la zone (visuel + progression + contrôles) */
        SendMessageW(g_status, WM_SIZE, 0, 0);
        RECT cr;
        GetClientRect(hwnd, &cr);
        int parts[4] = { cr.right * 45 / 100, cr.right * 62 / 100, cr.right * 74 / 100, -1 };
        SendMessageW(g_status, SB_SETPARTS, 4, (LPARAM)parts);
        status_update();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_CLOSE:
        save_state();
        mp_plugins_shutdown();
        sp_stop(); cc_stop();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Mode --selftest : vérifie le pipeline de décodage sous Wine/Win11   */
/* ------------------------------------------------------------------ */
static int run_selftest(int argc, char** argv)
{
    FILE* log = fopen("selftest.log", "w");
    if (!log) return 2;

    fprintf(log, "MusicPlayer " MP_VERSION " — self-test\n");
    fprintf(log, "FFmpeg : %s\n", av_version_info());

    mp_init();
    fprintf(log, "Audio device : %s\n", mp_audio_device_ok() ? "OK" : "ABSENT (silent mode)");
    int all_ok = 1;

    /* argv[0] est le premier token de la ligne de commande ("--selftest") */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) continue;
        int ok = 1;
        fprintf(log, "\n== %s ==\n", argv[i]);
        if (mp_open(argv[i]) == 0) {
            Sleep(900);   /* laisse le décodeur remplir le buffer */

            mp_state st = mp_get_state();
            double pos = mp_get_position();
            double dur = mp_get_duration();
            ok = (st == MP_STATE_PLAYING) && pos > 0.05 && dur > 0.0;
            fprintf(log, "  playback  : state=%d pos=%.2fs dur=%.2fs -> %s\n",
                    st, pos, dur, ok ? "PASS" : "FAIL");

            /* test vitesse */
            mp_set_speed(2.0f);
            Sleep(150);
            float sp = mp_get_speed();
            int sp_ok = (sp == 2.0f);
            fprintf(log, "  speed     : x%.1f -> %s\n", sp, sp_ok ? "PASS" : "FAIL");
            ok &= sp_ok;
            mp_set_speed(1.0f);

            /* test pause/reprise */
            mp_pause();
            Sleep(150);
            int ps_ok = (mp_get_state() == MP_STATE_PAUSED);
            mp_play();
            Sleep(150);
            ps_ok &= (mp_get_state() == MP_STATE_PLAYING);
            fprintf(log, "  pause     : -> %s\n", ps_ok ? "PASS" : "FAIL");
            ok &= ps_ok;

            /* test stop → position 0 */
            mp_stop();
            double pos2 = mp_get_position();
            int st2 = mp_get_state();
            int stop_ok = (st2 == MP_STATE_STOPPED && pos2 == 0.0);
            fprintf(log, "  stop      : state=%d pos=%.2fs -> %s\n", st2, pos2,
                    stop_ok ? "PASS" : "FAIL");
            ok &= stop_ok;

            /* test lecture complète du fichier jusqu'à la fin */
            mp_play();
            Sleep((DWORD)(dur * 1000.0) + 1200);
            int fin_ok = (mp_get_state() == MP_STATE_FINISHED);
            fprintf(log, "  end       : state=%d (dur=%.2fs) -> %s\n",
                    mp_get_state(), dur, fin_ok ? "PASS" : "FAIL");
            ok &= fin_ok;

            mp_stop();
        } else {
            ok = 0;
            fprintf(log, "  open      : FAIL\n");
        }
        all_ok &= ok;
    }

    mp_shutdown();
    fprintf(log, "\nSELFTEST %s\n", all_ok ? "PASS" : "FAIL");
    fclose(log);
    return all_ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Point d'entrée                                                      */
/* ------------------------------------------------------------------ */
static void resolve_plugins_dir(void)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = L'\0';
    swprintf(g_plugins_dir, MAX_PATH, L"%ls\\plugins", exe);
    swprintf(g_skins_dir, MAX_PATH, L"%ls\\skins", exe);
    swprintf(g_lang_dir, MAX_PATH, L"%ls\\lang", exe);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrev; (void)nCmdShow;
    srand((unsigned)GetTickCount());   /* mode aléatoire de la playlist */

    /* mode test : MusicPlayer.exe --selftest fichier1 fichier2 ... */
    if (lpCmdLine && strstr(lpCmdLine, "--selftest")) {
        int argc = 0;
        char* argv[64];
        char cmdline[1024];
        strncpy(cmdline, lpCmdLine, sizeof(cmdline) - 1);
        cmdline[sizeof(cmdline) - 1] = '\0';
        char* tok = strtok(cmdline, " \t");
        while (tok && argc < 63) { argv[argc++] = tok; tok = strtok(NULL, " \t"); }
        return run_selftest(argc, argv);
    }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    /* résultat de la dernière mise à jour (updater.log écrit par le
     * script de mise à jour) : un échec d'extraction — ou une version
     * incohérente — s'affiche ici */
    {
        wchar_t ulog[MAX_PATH];
        GetModuleFileNameW(NULL, ulog, MAX_PATH);
        wchar_t* sl = wcsrchr(ulog, L'\\');
        if (sl) wcscpy(sl + 1, L"updater.log");
        FILE* uf = _wfopen(ulog, L"r");
        if (uf) {
            char buf[512] = "";
            fgets(buf, sizeof(buf), uf);
            fclose(uf);
            DeleteFileW(ulog);
            if (strncmp(buf, "FAIL", 4) == 0) {
                MessageBoxA(NULL, buf, "MusicPlayer update", MB_ICONERROR);
            } else if (strncmp(buf, "OK:", 3) == 0) {
                char ver[64] = "";
                const char* v = buf + 3;
                while (*v == ' ' || *v == '\r' || *v == '\n') v++;
                snprintf(ver, sizeof(ver), "%s", v);
                if (strcmp(ver, MP_VERSION) != 0) {
                    /* le zip contenait une autre version que le binaire :
                     * zip plus récent = extraction ratée (fichiers
                     * verrouillés) ; zip plus vieux = rien à faire */
                    int zip_newer = strcmp(ver, MP_VERSION) > 0;
                    char msg[700];
                    if (zip_newer) {
                        snprintf(msg, sizeof(msg),
                                 "Update failed: the downloaded zip "
                                 "contained %s but this binary is still "
                                 "%s.\n"
                                 "The extraction did not replace the "
                                 "files (files locked?).\n"
                                 "Download the zip manually from the "
                                 "GitHub release page.", ver, MP_VERSION);
                    } else {
                        snprintf(msg, sizeof(msg),
                                 "Note: the downloaded zip contained %s "
                                 "(this binary is %s) — the update was "
                                 "already applied or is newer.",
                                 ver, MP_VERSION);
                    }
                    MessageBoxA(NULL, msg, "MusicPlayer update",
                                zip_newer ? MB_ICONWARNING : MB_ICONINFORMATION);
                }
            }
        }
    }

    cc_start();
    sp_start();   /* le client joue le flux du moteur (/stream) */

    /* configuration persistante : volume, vitesse, reprise de session */
    config_load();
    mp_set_log_level(g_cfg.log_level);

    sp_set_volume(g_cfg.volume / 100.0f);
    cc_cmd_val("speed", g_cfg.speed);
    playlist_set_shuffle(g_cfg.shuffle);
    /* plein écran multi-écrans */
    g_fs_screens = g_cfg.fs_screens;
    g_fs_mode[0] = g_cfg.fs_mode1;
    g_fs_mode[1] = g_cfg.fs_mode2;
    g_fs_mode[2] = g_cfg.fs_mode3;

    resolve_plugins_dir();
    {
        char dir_utf8[MAX_PATH * 2], dbg[600];
        wide_to_utf8(g_plugins_dir, dir_utf8, sizeof(dir_utf8));
        _snprintf(dbg, sizeof(dbg), "Plugins directory : %s", dir_utf8);
        log_line(dbg);
    }
    mp_plugins_scan(g_plugins_dir, g_skins_dir, &g_host, 0);

    /* vérification des mises à jour des plugins (manifeste plugins.json)
     * en arrière-plan : seul le plugin concerné est téléchargé */
    if (mp_update_get_mode() != 0) {
        HANDLE t = CreateThread(NULL, 0, plugins_upd_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }

    /* langue : préférence mémorisée, sinon langue du système, sinon anglais */
    lang_init(g_lang_dir, NULL);
    lang_pref_load();

    /* GDI+ : images de fond des skins */
    {
        GdiplusStartupInput gsi;
        memset(&gsi, 0, sizeof(gsi));
        gsi.GdiplusVersion = 1;
        GdiplusStartup(&g_gdiplus_token, &gsi, NULL);
    }

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MusicPlayerWnd";
    if (!RegisterClassW(&wc)) {
        char dbg[256];
        _snprintf(dbg, sizeof(dbg), "RegisterClassW failed (err=%lu)", GetLastError());
        log_line(dbg);
    }

    g_hwnd = CreateWindowExW(0, L"MusicPlayerWnd", APP_TITLE,
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 640, 300,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) {
        char dbg[256];
        _snprintf(dbg, sizeof(dbg), "CreateWindowExW failed (err=%lu)", GetLastError());
        log_line(dbg);
        return 1;
    }
    log_line("Window created");

    g_menu_bar = create_menus();
    SetMenu(g_hwnd, g_skin_menu_visible ? g_menu_bar : NULL);
    refresh_speed_check(GetSubMenu(GetSubMenu(g_menu_bar, 1), 0));
    rebuild_plugins_menu(g_menu_bar);
    mp_plugins_apply_skins(g_hwnd);
    log_line("Menus built");

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    log_line("Window shown");

    /* serveur web : config persistée + démarrage si activé */
    web_load_config();
    web_apply();
    if (g_web_enabled) {
        char dbg[256];
        _snprintf(dbg, sizeof(dbg), "Web server enabled on port %d", g_web_port);
        log_line(dbg);
    }

    /* fichier ou dossier passé en ligne de commande :
       "MusicPlayer.exe chemin.mp3" ou "MusicPlayer.exe C:\Musique" */
    if (lpCmdLine && *lpCmdLine) {
        char file[MAX_PATH * 3];
        strncpy(file, lpCmdLine, sizeof(file) - 1);
        file[sizeof(file) - 1] = '\0';
        size_t len = strlen(file);
        if (len > 0 && file[0] == '"' && file[len - 1] == '"') {
            memmove(file, file + 1, len - 2);
            file[len - 2] = '\0';
        }
        if (GetFileAttributesA(file) & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t dir_w[MAX_PATH];
            utf8_to_wide(file, dir_w, MAX_PATH);
            if (playlist_open_folder(dir_w) != 0) {
                char dbg[512];
                _snprintf(dbg, sizeof(dbg), "Command-line folder open failed : %s", file);
                log_line(dbg);
            }
        } else if (cc_open(file) != 0) {
            char dbg[512];
            _snprintf(dbg, sizeof(dbg), "Command-line open failed : %s", file);
            log_line(dbg);
        }
    } else {
        /* pas de ligne de commande : reprise de la dernière session */
        resume_last_session();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
