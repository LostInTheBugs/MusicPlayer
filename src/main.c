/*
 * MusicPlayer — interface graphique Windows (Win32, Unicode)
 * Menu, status bar, glisser-déposer, raccourcis clavier, mode --selftest.
 *
 * Conventions : toute l'UI est en UTF-16 (W). Les chemins de fichiers sont
 * convertis en UTF-8 pour le moteur (FFmpeg gère l'UTF-8 sur Windows).
 */
#include <winsock2.h>
#include <windows.h>
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
#include "plugin.h"
#include "plugin_loader.h"
#include "lang.h"
#include "update.h"
#include "config.h"

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
    IDM_PLAYPAUSE = 201, IDM_STOP = 202, IDM_NEXT = 203,
    IDM_SPEED_BASE = 300,   /* +0 → 0.5x, +1 → 1.0x, +2 → 1.5x, +3 → 2.0x */
    IDM_VOL_UP = 401, IDM_VOL_DOWN = 402, IDM_VOL_SHOW = 403,
    IDM_PLUGIN_RELOAD = 501,
    IDM_PLUGIN_CFG = 502,   /* Settings ▸ Plugins… */
    IDM_PLUGIN_BASE = 600,  /* items plugins dynamiques */
    IDM_LANG_BASE = 700,    /* items langues dynamiques */
    IDM_FULLSCREEN = 801,
    IDM_CHECK_UPDATE = 802, IDM_AUTO_UPDATE = 803,
    IDM_WEB_SERVER = 804,
    IDM_ABOUT = 901
};

#define SPEED_COUNT 4
static const float SPEED_VALUES[SPEED_COUNT] = { 0.5f, 1.0f, 1.5f, 2.0f };

/* barre de contrôles (boutons + volume) */
#define CTRL_H 32
#define PROGRESS_H 16

static HWND g_hwnd = NULL;
static HWND g_status = NULL;
static wchar_t g_plugins_dir[MAX_PATH] = { 0 };
static wchar_t g_lang_dir[MAX_PATH] = { 0 };

static int  g_fullscreen = 0;
static RECT g_win_normal = { 0, 0, 640, 300 };
static RECT g_rc_play, g_rc_stop, g_rc_next, g_rc_shuffle, g_rc_plist, g_rc_fs, g_rc_vol;
static int  g_vol_drag = 0;   /* curseur de volume en cours de glissement */

static void status_update(void);        /* définie plus bas */
static void wide_to_utf8(const wchar_t* in, char* out, int out_chars); /* idem */
static void log_line(const char* s);    /* idem */
static void playlist_win_rebuild(void); /* fenêtre playlist */
static void playlist_win_highlight(void);

/* ------------------------------------------------------------------ */
/* Playlist : lecture d'un dossier (avec ses sous-dossiers)            */
/* ------------------------------------------------------------------ */
#define PLAYLIST_MAX 4096
static wchar_t* g_plist[PLAYLIST_MAX];
static int      g_plist_n = 0;
static int      g_plist_idx = -1;
static wchar_t  g_plist_dir[MAX_PATH] = { 0 };   /* dossier de la playlist */

static void playlist_clear(void)
{
    for (int i = 0; i < g_plist_n; i++) free(g_plist[i]);
    g_plist_n = 0;
    g_plist_idx = -1;
    playlist_win_rebuild();
}

static void playlist_scan(const wchar_t* dir)
{
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                wchar_t sub[MAX_PATH];
                swprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
                playlist_scan(sub);
            }
        } else {
            const wchar_t* e = wcsrchr(fd.cFileName, L'.');
            if (e && (!_wcsicmp(e, L".mp3") || !_wcsicmp(e, L".mp4")) &&
                g_plist_n < PLAYLIST_MAX) {
                size_t cap = wcslen(dir) + wcslen(fd.cFileName) + 2;
                wchar_t* full = (wchar_t*)malloc(cap * sizeof(wchar_t));
                if (full) {
                    swprintf(full, cap, L"%ls\\%ls", dir, fd.cFileName);
                    g_plist[g_plist_n++] = full;
                }
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int playlist_cmp(const void* a, const void* b)
{
    return _wcsicmp(*(const wchar_t* const*)a, *(const wchar_t* const*)b);
}

static int playlist_play_index(int i)
{
    if (i < 0 || i >= g_plist_n) return -1;
    g_plist_idx = i;
    char utf8[MAX_PATH * 3];
    wide_to_utf8(g_plist[i], utf8, sizeof(utf8));
    int rc = mp_open(utf8);
    playlist_win_highlight();
    return rc;
}

/* Ouvre un dossier en playlist : scan récursif + tri + lecture du premier */
static int playlist_open_folder(const wchar_t* dir)
{
    wcscpy(g_plist_dir, dir);
    playlist_clear();
    playlist_scan(dir);
    if (g_plist_n == 0) return -1;
    qsort(g_plist, g_plist_n, sizeof(wchar_t*), playlist_cmp);
    return playlist_play_index(0);
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
        mp_stop();
    } else if (playlist_play_index(g_plist_idx + 1) != 0) {
        g_plist_idx++;                 /* fichier illisible : on saute */
        playlist_next();
    }
    status_update();
}

/* Appelé par le timer : enchaîne automatiquement à la fin d'un morceau */
static void playlist_tick(void)
{
    if (g_plist_n > 0 && g_plist_idx >= 0 && g_plist_idx < g_plist_n &&
        mp_get_state() == MP_STATE_FINISHED)
        playlist_next();
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
/* Journal                                                             */
/* ------------------------------------------------------------------ */
static void log_line(const char* msg)
{
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    FILE* f = fopen("musicplayer.log", "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02u:%02u:%02u] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
}

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
static const char* host_file_name(void) { return mp_get_file_name(); }
static int         host_get_state(void) { return (int)mp_get_state(); }
static double      host_get_position(void) { return mp_get_position(); }
static double      host_get_duration(void) { return mp_get_duration(); }
static float       host_get_volume(void) { return mp_get_volume(); }
static float       host_get_speed(void) { return mp_get_speed(); }
static void        host_play_pause(void) { mp_play_pause(); }
static void        host_stop(void) { mp_stop(); }
static void        host_next(void) { playlist_next(); }
static void        host_set_volume(float v) { mp_set_volume(v); }
static void        host_set_speed(float s) { mp_set_speed(s); }
static void        host_set_audio_out(int m) { mp_set_audio_out(m); }
static int         host_get_audio_out(void) { return mp_get_audio_out(); }
static void        host_shuffle_toggle(void)
{ playlist_set_shuffle(!playlist_get_shuffle()); }
static int         host_get_shuffle(void) { return playlist_get_shuffle(); }
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
{ return mp_web_read(dst, frames); }

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
    RGB(92, 98, 116),    /* prog_border */
};

/* recrée les pinceaux de la progression (couleurs figées) */
static void skin_reset_brushes(void);

static void host_skin_set_colors(const mp_skin_colors* c)
{
    if (!c) return;
    g_skin = *c;
    skin_reset_brushes();
    if (g_hwnd) {
        InvalidateRect(g_hwnd, NULL, TRUE);
        status_update();
    }
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
    host_plist_count, host_plist_name, host_plist_index, host_plist_play,
    host_main_window,
    host_web_enabled, host_web_port, host_web_audio, host_web_ips,
    host_web_find_free_port,
    host_web_read,
    host_skin_set_colors,
    host_get_metadata, host_get_cover, host_plist_path
};

/* ------------------------------------------------------------------ */
/* Status bar                                                          */
/* ------------------------------------------------------------------ */
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
    const char* fn = mp_get_file_name();
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

    double pos = mp_get_position(), dur = mp_get_duration();
    wchar_t p[16], d[16];
    fmt_time(p, 16, pos);
    fmt_time(d, 16, dur);
    swprintf(s2, 32, L" %ls / %ls", p, d);

    swprintf(s3, 32, L" x%.1f", mp_get_speed());
    swprintf(s4, 48, lang_get("vol_show"), (int)(mp_get_volume() * 100.0f + 0.5f));

    SendMessageW(g_status, SB_SETTEXT, 0, (LPARAM)s1);
    SendMessageW(g_status, SB_SETTEXT, 1, (LPARAM)s2);
    SendMessageW(g_status, SB_SETTEXT, 2, (LPARAM)s3);
    SendMessageW(g_status, SB_SETTEXT, 3, (LPARAM)s4);

    /* titre + état dans la zone centrale */
    static const char* state_keys[] = { "state_stopped", "state_playing", "state_paused", "state_finished" };
    wchar_t title[320];
    swprintf(title, 320, L"%ls — %ls", APP_TITLE, lang_get(state_keys[mp_get_state()]));
    SetWindowTextW(g_hwnd, title);
    InvalidateRect(g_hwnd, NULL, TRUE);
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
    float sp = mp_get_speed();
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
    HMENU mVis = CreatePopupMenu();   /* visuels : radio */
    HMENU mFX = CreatePopupMenu();    /* effets audio : cases */
    HMENU mSkin = CreatePopupMenu();  /* skins : radio */
    HMENU mSvc = CreatePopupMenu();   /* services : cases */

    int n = mp_plugins_count();
    int vis_active = -1;
    int skin_active = -1;
    for (int i = 0; i < n; i++) {
        mp_plugin* p = mp_plugins_get(i);
        if (!p || !p->api || !p->visible) continue;   /* masqué : absent */
        wchar_t label[160], name_w[128], ver_w[32];
        utf8_to_wide(p->api->name(), name_w, 128);
        utf8_to_wide(p->api->version() ? p->api->version() : "?", ver_w, 32);
        swprintf(label, 160, L"%ls %ls", name_w, ver_w);

        unsigned t = p->api->type();
        HMENU target;
        if (t & MP_PLUGIN_SKIN)          target = mSkin;
        else if (t & MP_PLUGIN_VISUAL)   target = mVis;
        else if (t & MP_PLUGIN_AUDIO_EFFECT) target = mFX;
        else if (t & MP_PLUGIN_SERVICE)  target = mSvc;
        else                             continue;

        AppendMenuW(target, MF_STRING, IDM_PLUGIN_BASE + i, label);
        if (t & MP_PLUGIN_VISUAL) {
            if (p->enabled) vis_active = i;
        } else if (t & MP_PLUGIN_SKIN) {
            if (p->enabled) skin_active = i;
        } else {
            CheckMenuItem(target, IDM_PLUGIN_BASE + i,
                          MF_BYCOMMAND | (p->enabled ? MF_CHECKED : MF_UNCHECKED));
        }
    }
    if (vis_active >= 0)
        CheckMenuRadioItem(mVis, IDM_PLUGIN_BASE, IDM_PLUGIN_BASE + n - 1,
                           IDM_PLUGIN_BASE + vis_active, MF_BYCOMMAND);
    if (skin_active >= 0)
        CheckMenuRadioItem(mSkin, IDM_PLUGIN_BASE, IDM_PLUGIN_BASE + n - 1,
                           IDM_PLUGIN_BASE + skin_active, MF_BYCOMMAND);

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
        if (GetMenuItemCount(mSkin) > 0)
            AppendMenuW(m, MF_POPUP, (UINT_PTR)mSkin, lang_get("plugins_skins"));
        else DestroyMenu(mSkin);
        if (GetMenuItemCount(mSvc) > 0)
            AppendMenuW(m, MF_POPUP, (UINT_PTR)mSvc, lang_get("plugins_services"));
        else DestroyMenu(mSvc);
    }
    RemoveMenu(parent, 2, MF_BYPOSITION);
    InsertMenuW(parent, 2, MF_BYPOSITION | MF_POPUP, (UINT_PTR)m, lang_get("menu_plugins"));
    DrawMenuBar(g_hwnd);
}

/* Sous-menu des langues disponibles (dans le menu Paramètres, position 3) */
static void rebuild_lang_menu(HMENU settings)
{
    HMENU m = CreatePopupMenu();
    int n = 0;
    const lang_info* li = lang_list(&n);
    for (int i = 0; i < n; i++) {
        AppendMenuW(m, MF_STRING, IDM_LANG_BASE + i, li[i].name);
        if (wcscmp(li[i].code, lang_code()) == 0)
            CheckMenuRadioItem(m, IDM_LANG_BASE, IDM_LANG_BASE + n - 1,
                               IDM_LANG_BASE + i, MF_BYCOMMAND);
    }
    RemoveMenu(settings, 3, MF_BYPOSITION);
    InsertMenuW(settings, 3, MF_BYPOSITION | MF_POPUP, (UINT_PTR)m, lang_get("menu_lang"));
    DrawMenuBar(g_hwnd);
}

static HMENU create_menus(void)
{
    HMENU bar = CreateMenu();

    HMENU mFile = CreatePopupMenu();
    AppendMenuW(mFile, MF_STRING, IDM_OPEN, lang_get("open"));
    AppendMenuW(mFile, MF_STRING, IDM_OPEN_FOLDER, lang_get("menu_open_folder"));
    AppendMenuW(mFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mFile, MF_STRING, IDM_EXIT, lang_get("quit"));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mFile, lang_get("menu_file"));

    /* Paramètres : vitesse, plein écran, langue, mises à jour */
    HMENU mSettings = CreatePopupMenu();
    AppendMenuW(mSettings, MF_POPUP, (UINT_PTR)build_speed_menu(), lang_get("speed"));
    AppendMenuW(mSettings, MF_STRING, IDM_FULLSCREEN, lang_get("fullscreen"));
    AppendMenuW(mSettings, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mSettings, MF_POPUP, (UINT_PTR)CreatePopupMenu(), lang_get("menu_lang"));
    AppendMenuW(mSettings, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mSettings, MF_STRING, IDM_CHECK_UPDATE, lang_get("menu_check_updates"));
    AppendMenuW(mSettings,
                 MF_STRING | (mp_update_auto_enabled() ? MF_CHECKED : MF_UNCHECKED),
                 IDM_AUTO_UPDATE, lang_get("menu_auto_update"));
    AppendMenuW(mSettings, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mSettings, MF_STRING, IDM_WEB_SERVER, lang_get("menu_web_server"));
    AppendMenuW(mSettings, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mSettings, MF_STRING, IDM_PLUGIN_CFG, lang_get("menu_plugins_cfg"));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mSettings, lang_get("menu_settings"));

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)CreatePopupMenu(), lang_get("menu_plugins"));

    HMENU mHelp = CreatePopupMenu();
    AppendMenuW(mHelp, MF_STRING, IDM_ABOUT, lang_get("about"));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mHelp, lang_get("menu_help"));

    return bar;
}

/* Reconstruction complète de la barre de menus (changement de langue) */
static void rebuild_menus(void)
{
    HMENU old = GetMenu(g_hwnd);
    HMENU bar = create_menus();
    SetMenu(g_hwnd, bar);
    HMENU mSettings = GetSubMenu(bar, 1);
    refresh_speed_check(GetSubMenu(mSettings, 0));
    rebuild_lang_menu(mSettings);          /* position 3 dans Paramètres */
    rebuild_plugins_menu(bar);             /* position 2 dans la barre */
    if (old) DestroyMenu(old);
    mp_plugins_apply_skins(g_hwnd);
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
        if (mp_open(path_utf8) != 0) {
            wchar_t msg[600];
            swprintf(msg, 600, lang_get("err_open"), path_w);
            MessageBoxW(g_hwnd, msg, APP_TITLE, MB_ICONERROR);
        }
    }
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
}

static INT_PTR CALLBACK web_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
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
static INT_PTR CALLBACK plugins_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        HWND lv = GetDlgItem(h, IDC_PLG_LIST);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMNW c0, c1, c2;
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
        c2.cx = 100; c2.pszText = L"Description";
        ListView_InsertColumn(lv, 2, &c2);
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
            ListView_SetCheckState(lv, row, p->visible ? TRUE : FALSE);
            row++;
        }
        SetDlgItemTextW(h, IDC_PLG_LBL, lang_get("plugins_dlg_lbl"));
        SetWindowTextW(h, lang_get("plugins_dlg_title"));
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
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
/* Mise à jour : téléchargement + application + relancement auto       */
/* ------------------------------------------------------------------ */
static void apply_update_and_restart(const wchar_t* zip_path,
                                     const wchar_t* exe_path)
{
    /* PowerShell extrait le zip dans le dossier de l'exe puis relance
     * l'application ; on ferme MusicPlayer juste après. */
    wchar_t dir[MAX_PATH];
    wcscpy(dir, exe_path);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    wchar_t ps[2048];
    swprintf(ps, 2048,
        L"powershell -NoProfile -WindowStyle Hidden -Command \""
        L"Expand-Archive -Force -Path '%ls' -DestinationPath '%ls'; "
        L"Start-Process -FilePath '%ls'\"",
        zip_path, dir, exe_path);
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, ps, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

static INT_PTR CALLBACK upd_dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
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
        mp_open(g_cfg.last_path);
    }
}

static void save_state(void)
{
    g_cfg.volume = (int)(mp_get_volume() * 100.0f + 0.5f);
    g_cfg.speed = mp_get_speed();
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
    } else if (mp_get_file_name()) {
        strncpy(g_cfg.last_path, mp_get_file_name(), sizeof(g_cfg.last_path) - 1);
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
    int y = rc->bottom - CTRL_H;
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

    /* fond de la barre */
    RECT bg = { rc->left, rc->bottom - CTRL_H, rc->right, rc->bottom };
    HBRUSH bbg = CreateSolidBrush(g_skin.ctrl_bar);
    FillRect(hdc, &bg, bbg);
    DeleteObject(bbg);
    HPEN sep = CreatePen(PS_SOLID, 1, g_skin.ctrl_sep);
    HPEN oldp = (HPEN)SelectObject(hdc, sep);
    MoveToEx(hdc, bg.left, bg.top, NULL);
    LineTo(hdc, bg.right, bg.top);
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
    draw_glyph_play(hdc, &gp, mp_get_state() == MP_STATE_PLAYING);

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
    float vol = mp_get_volume();
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
    mp_set_volume(v);
    status_update();
}

/* ------------------------------------------------------------------ */
/* Zone centrale (WM_PAINT)                                            */
/* ------------------------------------------------------------------ */
static void get_center_rect(HWND hwnd, RECT* rc)
{
    GetClientRect(hwnd, rc);
    if (g_status) {
        RECT sr;
        GetWindowRect(g_status, &sr);
        rc->bottom -= (sr.bottom - sr.top);
    }
}

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

    /* fond */
    RECT bg = { rc->left, rc->top, rc->right, rc->bottom };
    FillRect(hdc, &bg, g_pg_bg);

    /* remplissage */
    double dur = mp_get_duration();
    double pos = mp_get_position();
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

static void paint_center(HDC hdc, RECT* rc)
{
    RECT vis_rc, bar_rc, ctrl_rc;
    if (g_fullscreen) {
        /* plein écran : la zone visuelle occupe tout */
        vis_rc = *rc;
        bar_rc.left = bar_rc.right = ctrl_rc.left = ctrl_rc.right = 0;
        bar_rc.top = bar_rc.bottom = ctrl_rc.top = ctrl_rc.bottom = 0;
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

    /* un plugin visuel actif remplace le texte par son rendu */
    if (mp_plugins_has_visual()) {
        mp_plugins_visual_render(hdc, vis_rc.right - vis_rc.left, vis_rc.bottom - vis_rc.top);
    } else {
        SetBkMode(hdc, TRANSPARENT);
        const char* fn = mp_get_file_name();
        static const char* state_keys[] = { "center_stopped", "center_playing", "center_paused", "center_finished" };
        const wchar_t* st = lang_get(state_keys[mp_get_state()]);

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
/* Plein écran                                                         */
/* ------------------------------------------------------------------ */
static void toggle_fullscreen(HWND hwnd)
{
    if (!g_fullscreen) {
        GetWindowRect(hwnd, &g_win_normal);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP);
        SetWindowLongW(hwnd, GWL_EXSTYLE,
                       GetWindowLongW(hwnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, sw, sh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 1;
        ShowWindow(g_status, SW_HIDE);
    } else {
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
    get_center_rect(hwnd, &rc);
    layout_controls(&rc);

    /* clic sur la barre de progression : aller directement au moment
     * choisi (la barre est juste au-dessus de la rangée de boutons) */
    if (y >= rc.bottom - CTRL_H - PROGRESS_H && y < rc.bottom - CTRL_H) {
        double dur = mp_get_duration();
        int w = rc.right - rc.left;
        if (dur > 0.0 && w > 0) {
            double ratio = (double)(x - rc.left) / (double)w;
            if (ratio < 0.0) ratio = 0.0;
            if (ratio > 1.0) ratio = 1.0;
            mp_seek(ratio * dur);
            status_update();
        }
        return;
    }

    if (x >= g_rc_play.left && x <= g_rc_play.right &&
        y >= g_rc_play.top && y <= g_rc_play.bottom) {
        mp_play_pause();
        status_update();
    } else if (x >= g_rc_stop.left && x <= g_rc_stop.right &&
               y >= g_rc_stop.top && y <= g_rc_stop.bottom) {
        mp_stop();
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
    case IDM_EXIT:      SendMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    case IDM_PLAYPAUSE: mp_play_pause(); break;
    case IDM_STOP:      mp_stop(); break;
    case IDM_NEXT:      playlist_next(); break;
    case IDM_FULLSCREEN: toggle_fullscreen(g_hwnd); break;
    case IDM_CHECK_UPDATE:
        mp_update_check_async(g_hwnd, 1);
        break;
    case IDM_AUTO_UPDATE: {
        int on = !mp_update_auto_enabled();
        mp_update_set_auto(on);
        CheckMenuItem(GetSubMenu(GetMenu(g_hwnd), 1), IDM_AUTO_UPDATE,
                      MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        break;
    }
    case IDM_WEB_SERVER:
        do_web_dialog();
        break;
    case IDM_ABOUT:     do_about(); break;

    case IDM_VOL_UP: {
        float v = mp_get_volume() + 0.05f;
        if (v > 2.0f) v = 2.0f;
        mp_set_volume(v);
        status_update();
        break;
    }
    case IDM_VOL_DOWN: {
        float v = mp_get_volume() - 0.05f;
        if (v < 0.0f) v = 0.0f;
        mp_set_volume(v);
        status_update();
        break;
    }
    case IDM_PLUGIN_RELOAD:
        mp_plugins_scan(g_plugins_dir, &g_host);
        mp_plugins_apply_skins(g_hwnd);
        rebuild_plugins_menu(GetMenu(g_hwnd));
        break;

    default:
        if (id >= IDM_SPEED_BASE && id < IDM_SPEED_BASE + SPEED_COUNT) {
            mp_set_speed(SPEED_VALUES[id - IDM_SPEED_BASE]);
            refresh_speed_check(GetSubMenu(GetSubMenu(GetMenu(g_hwnd), 1), 0));
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
                rebuild_plugins_menu(GetMenu(g_hwnd));
                /* un service peut avoir une action au clic (ex. Lyrics) */
                if ((t & MP_PLUGIN_SERVICE) && p->api->service) {
                    p->api->service(p, MP_SERVICE_CLICK, NULL);
                    mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL);
                }
            }
        } else if (id >= IDM_LANG_BASE) {
            int i = id - IDM_LANG_BASE;
            int n = 0;
            const lang_info* li = lang_list(&n);
            if (i >= 0 && i < n) {
                if (lang_set(li[i].code) == 0) {
                    lang_pref_save(li[i].code);
                    rebuild_menus();
                    status_update();
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
        if (mp_update_auto_enabled())
            SetTimer(hwnd, 3, 4000, NULL); /* vérif. mises à jour au démarrage */
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
            if (mp_open(path_utf8) != 0) {
                wchar_t msg[600];
                swprintf(msg, 600, lang_get("err_open"), path_w);
                MessageBoxW(hwnd, msg, APP_TITLE, MB_ICONERROR);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE:   mp_play_pause(); break;
        case 'S':        mp_stop(); break;
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
        if (g_vol_drag) mouse_move(GET_X_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        mouse_up();
        return 0;
    case WM_TIMER:
        if (wp == 2) {
            /* rafraîchit la zone visuelle uniquement si un plugin visuel
               est actif (sinon le timer ne fait presque rien) */
            if (mp_plugins_has_visual()) {
                RECT rc;
                get_center_rect(hwnd, &rc);
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
        if (wp == 1)
            playlist_tick();       /* enchaîne à la fin d'un morceau */
        status_update();
        return 0;

    case MP_UPDATE_DONE: {
        /* résultat de la vérification de mises à jour
         * wp = manuel (1) / automatique (0) ; lp = 0 à jour, 1 dispo, 2 erreur */
        int manual = (int)wp;
        int state = (int)lp;
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
                wchar_t zip_path[MAX_PATH];
                GetEnvironmentVariableW(L"APPDATA", zip_path, MAX_PATH);
                wcscat(zip_path, L"\\MusicPlayer");
                CreateDirectoryW(zip_path, NULL);
                wcscat(zip_path, L"\\update.zip");
                wchar_t msg[256];
                swprintf(msg, 256, lang_get("upd_downloading"),
                         mp_update_latest());
                MessageBoxW(hwnd, msg, lang_get("upd_title"),
                            MB_OK | MB_ICONINFORMATION);
                if (mp_update_download(mp_update_latest(), zip_path) == 0) {
                    wchar_t exe[MAX_PATH];
                    GetModuleFileNameW(NULL, exe, MAX_PATH);
                    apply_update_and_restart(zip_path, exe);
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
    case WM_COMMAND:
        on_command(LOWORD(wp), GetMenu(hwnd));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        get_center_rect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            /* double buffer : dessin hors écran puis un seul BitBlt
             * (élimine le scintillement des effets visuels) */
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldbmp = (HBITMAP)SelectObject(mem, bmp);
            RECT all = { 0, 0, w, h };
            HBRUSH wb = CreateSolidBrush(g_skin.bg);
            FillRect(mem, &all, wb);
            DeleteObject(wb);
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
    case WM_ERASEBKGND: {
        /* efface le fond (nécessaire au redimensionnement :
         * les zones qui s'agrandissent doivent être repeintes) */
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH wb = CreateSolidBrush(g_skin.bg);
        FillRect(hdc, &rc, wb);
        DeleteObject(wb);
        return 1;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 420;
        mmi->ptMinTrackSize.y = 260;
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
        mp_shutdown();
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

    mp_init();

    /* configuration persistante : volume, vitesse, reprise de session */
    config_load();
    mp_set_volume(g_cfg.volume / 100.0f);
    mp_set_speed(g_cfg.speed);
    playlist_set_shuffle(g_cfg.shuffle);

    resolve_plugins_dir();
    {
        char dir_utf8[MAX_PATH * 2], dbg[600];
        wide_to_utf8(g_plugins_dir, dir_utf8, sizeof(dir_utf8));
        _snprintf(dbg, sizeof(dbg), "Plugins directory : %s", dir_utf8);
        log_line(dbg);
    }
    mp_plugins_scan(g_plugins_dir, &g_host);

    /* langue : préférence mémorisée, sinon langue du système, sinon anglais */
    lang_init(g_lang_dir, NULL);
    lang_pref_load();

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

    HMENU bar = create_menus();
    SetMenu(g_hwnd, bar);
    refresh_speed_check(GetSubMenu(GetSubMenu(bar, 1), 0));
    rebuild_lang_menu(GetSubMenu(bar, 1));
    rebuild_plugins_menu(bar);
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
        } else if (mp_open(file) != 0) {
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
