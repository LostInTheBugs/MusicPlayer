/* src/core/core_main.c — musicplayer-core.exe : le moteur, sans UI.
 *
 * Le core héberge : le moteur audio (player.c, sans carte son : le PCM
 * est diffusé aux clients via /stream), la playlist, le CD, et les
 * plugins SERVICE réseau (webserver, metadata, cover, upnp, rtp,
 * multiroom). Il expose l'API REST publique (core_http.c) sur le port
 * 8080 par défaut.
 *
 * Mode défaut : lancé par le client, arrêté par la commande shutdown.
 * Mode service Windows : installé via sc.exe (Settings ▸ Interface).
 *
 * Fenêtre invisible : timers d'enchaînement de playlist + sortie. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../plugin.h"
#include "../plugin_loader.h"
#include "../player.h"
#include "../config.h"
#include "../cd.h"
#include "core_playlist.h"

HWND g_core_hwnd = NULL;
static wchar_t g_plugins_dir[MAX_PATH];
static volatile LONG g_quit = 0;

void core_http_start(void);
void core_http_stop(void);
void core_plist_init(void);

/* ------------------------------------------------------------------ */
/* Journal                                                             */
/* ------------------------------------------------------------------ */
static void core_log(const char* msg)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    wchar_t logpath[MAX_PATH];
    swprintf(logpath, MAX_PATH, L"%ls\\musicplayer-core.log", exe);
    FILE* f = _wfopen(logpath, L"a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d] %s\n",
                st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
    OutputDebugStringA(msg);
}

/* ------------------------------------------------------------------ */
/* Host API offerte aux plugins (services réseau du core)              */
/* ------------------------------------------------------------------ */
static const char* host_get_file_name(void) { return mp_get_file_name(); }

static int host_plist_count(void)
{
    int n;
    core_plist_lock(); n = g_plist_n; core_plist_unlock();
    return n;
}

static const wchar_t* host_plist_name(int i)
{
    static wchar_t name[512];
    const wchar_t* full = NULL;
    core_plist_lock();
    if (i >= 0 && i < g_plist_n) full = g_plist[i];
    core_plist_unlock();
    if (!full) return L"";
    const wchar_t* b = wcsrchr(full, L'\\');
    wcsncpy(name, b ? b + 1 : full, 511);
    name[511] = 0;
    return name;
}

static int host_plist_index(void)
{
    int i;
    core_plist_lock(); i = g_plist_idx; core_plist_unlock();
    return i;
}

static void host_plist_play(int i)
{
    core_plist_lock();
    core_plist_play_index(i);
    core_plist_unlock();
}

static const wchar_t* host_plist_path(int i)
{
    static wchar_t path[MAX_PATH];
    const wchar_t* full = NULL;
    core_plist_lock();
    if (i >= 0 && i < g_plist_n) full = g_plist[i];
    core_plist_unlock();
    if (!full) return L"";
    wcsncpy(path, full, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    return path;
}

static int host_web_enabled(void) { return g_cfg.web_enabled; }
static int host_web_port(void)    { return g_cfg.web_port; }
static int host_web_audio(void)   { return g_cfg.web_audio; }
static const char* host_web_ips(void) { return g_cfg.web_ips; }
static int host_web_find_free_port(void) { return 0; }

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

/* skins / fenêtre : no-op dans le core (pas d'UI) */
static void host_noop4(int a, int b, int c) { (void)a; (void)b; (void)c; }
static void host_noop_colors(const mp_skin_colors* c) { (void)c; }
static void host_noop_bg(const char* p) { (void)p; }
static void host_noop_vis(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
static void host_noop_wsize(int w, int h, int fixed) { (void)w; (void)h; (void)fixed; }
static void host_noop_audio(int m) { (void)m; }
static const mp_skin_colors* host_noop_skincolors(void) { return NULL; }

static int host_get_state(void) { return (int)mp_get_state(); }
static int host_get_audio_out(void) { return 0; }

/* Jaquette d'un fichier : cover.* dans le dossier, sinon APIC du MP3.
 * Buffer statique : valable jusqu'au prochain appel. */
static unsigned char g_cover_buf[2 * 1024 * 1024];

const unsigned char* core_get_cover(const char* path, size_t* len)
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
        snprintf(p, sizeof(p), "%s%s", dir, names[i]);
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
                            p2++;                                /* type */
                            while (p2 < buf + fsz && *p2) p2++;  /* descr */
                            p2++;                                /* \0 */
                            size_t remain = (size_t)(buf + fsz - p2);
                            if (p2 < buf + fsz && remain > 0 &&
                                remain <= sizeof(g_cover_buf)) {
                                memcpy(g_cover_buf, p2, remain);
                                *len = remain;
                                found = 1;
                            }
                        }
                        free(buf);
                        if (found) break;
                    }
                }
            }
            pos += 10 + (long)fsz;
        }
    }
    fclose(f);
    return found ? g_cover_buf : NULL;
}

static void host_next(void)
{
    core_plist_lock();
    core_plist_next();
    core_plist_unlock();
}
static void host_shuffle_toggle(void)
{
    core_plist_lock();
    core_plist_set_shuffle(!core_plist_get_shuffle());
    core_plist_unlock();
}
static int host_get_shuffle(void)
{
    int s;
    core_plist_lock(); s = core_plist_get_shuffle(); core_plist_unlock();
    return s;
}

static const mp_host_api g_host = {
    4,
    core_log,
    host_get_state, mp_get_position, mp_get_duration,
    mp_get_volume, mp_get_speed, host_get_file_name,
    mp_play_pause, mp_stop, host_next,
    mp_set_volume, mp_set_speed, host_noop_audio, host_get_audio_out,
    host_shuffle_toggle, host_get_shuffle,
    NULL, NULL,                  /* dj : le mix DJ est côté client */
    host_plist_count, host_plist_name, host_plist_index, host_plist_play,
    NULL,                        /* main_window */
    host_web_enabled, host_web_port, host_web_audio, host_web_ips,
    host_web_find_free_port,
    host_svc_port, host_svc_ips,
    mp_web_read,
    host_noop_colors, host_noop_bg, host_noop_vis,
    host_noop4,                  /* skin_set_layout */
    mp_plugins_get_metadata, core_get_cover, host_plist_path,
    host_noop_skincolors,
    host_noop_wsize,
    mp_web_reader_open, mp_web_reader_close, mp_web_read_n
};

/* ------------------------------------------------------------------ */
/* Plugins : seuls les services réseau du core (dossier core_plugins)  */
/* ------------------------------------------------------------------ */
static void load_plugins(void)
{
    mp_plugins_scan(g_plugins_dir, NULL, &g_host, 1);
}

/* ------------------------------------------------------------------ */
/* Fenêtre invisible : timers (enchaînement playlist) + sortie         */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK core_wnd_proc(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 250, NULL);
        return 0;
    case WM_TIMER:
        if (w == 1) {
            core_plist_lock();
            core_plist_tick();
            core_plist_unlock();
            return 0;
        }
        return 0;
    case WM_APP + 1:          /* shutdown demandé par l'API */
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Mode service Windows : `musicplayer-core.exe --service`             */
/* L'installation se fait depuis le client (Settings ▸ Interface).     */
/* ------------------------------------------------------------------ */
static SERVICE_STATUS         g_svc_status;
static SERVICE_STATUS_HANDLE  g_svc_handle;

static int run_core(HINSTANCE hInst);

static DWORD WINAPI svc_ctrl(DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)type; (void)data; (void)ctx;
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_svc_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_svc_status.dwWaitHint = 5000;
        SetServiceStatus(g_svc_handle, &g_svc_status);
        /* même chemin que le shutdown par l'API : WM_APP+1 sort la boucle */
        PostMessageW(g_core_hwnd, WM_APP + 1, 0, 0);
        break;
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_svc_handle, &g_svc_status);
        break;
    default:
        break;
    }
    return NO_ERROR;
}

static void WINAPI svc_main(DWORD argc, LPTSTR* argv)
{
    (void)argc; (void)argv;
    g_svc_handle = RegisterServiceCtrlHandlerExW(L"MusicPlayerCore", svc_ctrl, NULL);
    if (!g_svc_handle) return;

    g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwCurrentState = SERVICE_START_PENDING;
    g_svc_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_svc_status.dwWin32ExitCode = 0;
    g_svc_status.dwServiceSpecificExitCode = 0;
    g_svc_status.dwCheckPoint = 0;
    g_svc_status.dwWaitHint = 5000;
    SetServiceStatus(g_svc_handle, &g_svc_status);

    run_core(GetModuleHandleW(NULL));   /* inclut la boucle de messages (WM_APP+1 la termine) */

    g_svc_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_svc_handle, &g_svc_status);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    (void)hPrev; (void)nShow;

    if (lpCmd && strstr(lpCmd, "--service")) {
        SERVICE_TABLE_ENTRYW table[] = {
            { L"MusicPlayerCore", svc_main },
            { NULL, NULL }
        };
        StartServiceCtrlDispatcherW(table);
        return 0;
    }
    return run_core(hInst);
}

/* Démarre le moteur (fenêtre invisible + boucle de messages). */
static int run_core(HINSTANCE hInst)
{
    config_load();
    core_plist_init();
    core_log("core starting");

    GetModuleFileNameW(NULL, g_plugins_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_plugins_dir, L'\\');
    if (slash) *slash = 0;
    wcscat(g_plugins_dir, L"\\core_plugins");

    /* fenêtre invisible */
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = core_wnd_proc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MPCoreWnd";
    RegisterClassW(&wc);
    g_core_hwnd = CreateWindowW(L"MPCoreWnd", L"MusicPlayer Core",
                                WS_OVERLAPPEDWINDOW, 0, 0, 0, 0,
                                NULL, NULL, hInst, NULL);

    mp_init();                 /* moteur sans carte son (mode silencieux) */
    load_plugins();
    /* démarre les services réseau (webserver, upnp, rtp, multiroom) :
     * ils n'ouvrent leurs sockets qu'à la réception de cet événement */
    mp_plugins_service(MP_SERVICE_WEB_APPLY, NULL);
    core_http_start();
    core_log("core ready (REST on 8080)");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0 && !InterlockedCompareExchange(&g_quit, 1, 1)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    core_http_stop();
    mp_plugins_shutdown();
    mp_shutdown();
    core_log("core stopped");
    return 0;
}
