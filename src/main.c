/*
 * MusicPlayer — interface graphique Windows (Win32, Unicode)
 * Menu, status bar, glisser-déposer, raccourcis clavier, mode --selftest.
 *
 * Conventions : toute l'UI est en UTF-16 (W). Les chemins de fichiers sont
 * convertis en UTF-8 pour le moteur (FFmpeg gère l'UTF-8 sur Windows).
 */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/avutil.h>

#include "player.h"
#include "plugin.h"
#include "plugin_loader.h"

#ifndef MP_VERSION
#define MP_VERSION "2026.08.001"
#endif
/* indirection : les arguments de ## ne sont pas expansés, d'où les 2 niveaux */
#define MP_WIDE2(x) L##x
#define MP_WIDE(x)  MP_WIDE2(x)
#define MP_VERSION_W MP_WIDE(MP_VERSION)

#define APP_TITLE L"MusicPlayer " MP_VERSION_W

/* IDs de commandes */
enum {
    IDM_OPEN = 101, IDM_EXIT = 102,
    IDM_PLAYPAUSE = 201, IDM_STOP = 202,
    IDM_SPEED_BASE = 300,   /* +0 → 0.5x, +1 → 1.0x, +2 → 1.5x, +3 → 2.0x */
    IDM_VOL_UP = 401, IDM_VOL_DOWN = 402, IDM_VOL_SHOW = 403,
    IDM_PLUGIN_RELOAD = 501,
    IDM_PLUGIN_BASE = 600,  /* items plugins dynamiques */
    IDM_ABOUT = 901
};

#define SPEED_COUNT 4
static const float SPEED_VALUES[SPEED_COUNT] = { 0.5f, 1.0f, 1.5f, 2.0f };

static HWND g_hwnd = NULL;
static HWND g_status = NULL;
static wchar_t g_plugins_dir[MAX_PATH] = { 0 };

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
/* API hôte exposée aux plugins                                        */
/* ------------------------------------------------------------------ */
static const char* host_file_name(void) { return mp_get_file_name(); }
static int         host_get_state(void) { return (int)mp_get_state(); }
static double      host_get_position(void) { return mp_get_position(); }
static double      host_get_duration(void) { return mp_get_duration(); }
static float       host_get_volume(void) { return mp_get_volume(); }
static float       host_get_speed(void) { return mp_get_speed(); }

static const mp_host_api g_host = {
    MP_PLUGIN_API_VERSION,
    log_line,
    host_get_state,
    host_get_position,
    host_get_duration,
    host_get_volume,
    host_get_speed,
    host_file_name
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
        const char* base = strrchr(fn, '\\');
        base = base ? base + 1 : fn;
        utf8_to_wide(base, s1, 280);
    } else {
        wcscpy(s1, L" (aucun fichier)");
    }

    double pos = mp_get_position(), dur = mp_get_duration();
    wchar_t p[16], d[16];
    fmt_time(p, 16, pos);
    fmt_time(d, 16, dur);
    swprintf(s2, 32, L" %ls / %ls", p, d);

    swprintf(s3, 32, L" x%.1f", mp_get_speed());
    swprintf(s4, 48, L" Vol %d%%", (int)(mp_get_volume() * 100.0f + 0.5f));

    SendMessageW(g_status, SB_SETTEXT, 0, (LPARAM)s1);
    SendMessageW(g_status, SB_SETTEXT, 1, (LPARAM)s2);
    SendMessageW(g_status, SB_SETTEXT, 2, (LPARAM)s3);
    SendMessageW(g_status, SB_SETTEXT, 3, (LPARAM)s4);

    /* titre + état dans la zone centrale */
    static const wchar_t* state_txt[] = { L"Arrêté", L"Lecture", L"Pause", L"Terminé" };
    wchar_t title[320];
    swprintf(title, 320, L"%ls — %ls", APP_TITLE, state_txt[mp_get_state()]);
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

static void refresh_volume_item(HMENU menu)
{
    wchar_t label[32];
    swprintf(label, 32, L"Volume : %d%%", (int)(mp_get_volume() * 100.0f + 0.5f));
    ModifyMenuW(menu, IDM_VOL_SHOW, MF_BYCOMMAND | MF_GRAYED | MF_STRING, IDM_VOL_SHOW, label);
}

static void rebuild_plugins_menu(HMENU parent)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_PLUGIN_RELOAD, L"Recharger les plugins");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    int n = mp_plugins_count();
    if (n == 0) {
        AppendMenuW(m, MF_GRAYED | MF_STRING, 0, L"(aucun plugin)");
    } else {
        for (int i = 0; i < n; i++) {
            mp_plugin* p = mp_plugins_get(i);
            wchar_t label[160], name_w[128], ver_w[32];
            utf8_to_wide(p->api->name(), name_w, 128);
            utf8_to_wide(p->api->version() ? p->api->version() : "?", ver_w, 32);
            swprintf(label, 160, L"%ls %ls", name_w, ver_w);
            AppendMenuW(m, MF_STRING, IDM_PLUGIN_BASE + i, label);
            CheckMenuItem(m, IDM_PLUGIN_BASE + i,
                          MF_BYCOMMAND | (p->enabled ? MF_CHECKED : MF_UNCHECKED));
        }
    }
    /* remplace l'ancien sous-menu Plugins (position 3) */
    RemoveMenu(parent, 3, MF_BYPOSITION);
    InsertMenuW(parent, 3, MF_BYPOSITION | MF_POPUP, (UINT_PTR)m, L"Plugins");
    DrawMenuBar(g_hwnd);
}

static HMENU create_menus(void)
{
    HMENU bar = CreateMenu();

    HMENU mFile = CreatePopupMenu();
    AppendMenuW(mFile, MF_STRING, IDM_OPEN, L"&Ouvrir…\tCtrl+O");
    AppendMenuW(mFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mFile, MF_STRING, IDM_EXIT, L"&Quitter");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mFile, L"&Fichier");

    HMENU mPlay = CreatePopupMenu();
    AppendMenuW(mPlay, MF_STRING, IDM_PLAYPAUSE, L"&Lecture / Pause\tEspace");
    AppendMenuW(mPlay, MF_STRING, IDM_STOP, L"&Stop\tS");
    AppendMenuW(mPlay, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mPlay, MF_POPUP, (UINT_PTR)build_speed_menu(), L"&Vitesse");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mPlay, L"&Lecture");

    HMENU mVol = CreatePopupMenu();
    AppendMenuW(mVol, MF_STRING, IDM_VOL_UP, L"&Monter\t↑");
    AppendMenuW(mVol, MF_STRING, IDM_VOL_DOWN, L"&Descendre\t↓");
    AppendMenuW(mVol, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mVol, MF_GRAYED | MF_STRING, IDM_VOL_SHOW, L"Volume : 80%");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mVol, L"&Volume");

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)CreatePopupMenu(), L"P&lugins");

    HMENU mHelp = CreatePopupMenu();
    AppendMenuW(mHelp, MF_STRING, IDM_ABOUT, L"À &propos…");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mHelp, L"A&ide");

    return bar;
}

/* ------------------------------------------------------------------ */
/* Dialogues                                                           */
/* ------------------------------------------------------------------ */
static void do_open_dialog(void)
{
    wchar_t path_w[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Fichiers audio (*.mp3;*.mp4)\0*.mp3;*.mp4\0"
                      L"MP3 (*.mp3)\0*.mp3\0"
                      L"MP4 (*.mp4)\0*.mp4\0"
                      L"Tous les fichiers (*.*)\0*.*\0";
    ofn.lpstrFile = path_w;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Ouvrir un fichier audio";

    if (GetOpenFileNameW(&ofn)) {
        char path_utf8[MAX_PATH * 3];
        wide_to_utf8(path_w, path_utf8, sizeof(path_utf8));
        if (mp_open(path_utf8) != 0) {
            wchar_t msg[600];
            swprintf(msg, 600, L"Impossible d'ouvrir \"%ls\".\nFormat non supporté ou fichier corrompu.", path_w);
            MessageBoxW(g_hwnd, msg, APP_TITLE, MB_ICONERROR);
        }
    }
}

static void do_about(void)
{
    wchar_t msg[1024];
    swprintf(msg, 1024,
        L"MusicPlayer " MP_VERSION_W L"\n\n"
        L"Lecteur audio MP3 / MP4 pour Windows.\n"
        L"Décodage : FFmpeg %hs\nAudio : miniaudio 0.11\nPlugins : %d chargé(s)\n\n"
        L"Raccourcis : Espace = lecture/pause, S = stop,\n"
        L"↑/↓ = volume, Ctrl+O = ouvrir",
        av_version_info(), mp_plugins_count());
    MessageBoxW(g_hwnd, msg, L"À propos", MB_OK | MB_ICONINFORMATION);
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

static void paint_center(HDC hdc, RECT* rc)
{
    /* un plugin visuel actif remplace le texte par son rendu */
    if (mp_plugins_has_visual()) {
        mp_plugins_visual_render(hdc, rc->right - rc->left, rc->bottom - rc->top);
        return;
    }

    SetBkMode(hdc, TRANSPARENT);
    const char* fn = mp_get_file_name();
    static const wchar_t* state_txt[] = { L"Arrêté", L"Lecture en cours", L"En pause", L"Terminé" };
    const wchar_t* st = state_txt[mp_get_state()];

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
        SetTextColor(hdc, RGB(30, 30, 30));
        RECT r = *rc;
        DrawTextW(hdc, base_w, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, old);
    }
    {
        HFONT old = (HFONT)SelectObject(hdc, small);
        SetTextColor(hdc, RGB(90, 90, 90));
        RECT r = *rc;
        r.top = rc->top + 38;
        DrawTextW(hdc, st, -1, &r, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, old);
    }

    DeleteObject(big);
    DeleteObject(small);
}

/* ------------------------------------------------------------------ */
/* Gestion des commandes                                               */
/* ------------------------------------------------------------------ */
static void on_command(int id, HMENU bar)
{
    switch (id) {
    case IDM_OPEN:      do_open_dialog(); break;
    case IDM_EXIT:      SendMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    case IDM_PLAYPAUSE: mp_play_pause(); break;
    case IDM_STOP:      mp_stop(); break;
    case IDM_ABOUT:     do_about(); break;

    case IDM_VOL_UP: {
        float v = mp_get_volume() + 0.05f;
        if (v > 1.0f) v = 1.0f;
        mp_set_volume(v);
        refresh_volume_item(bar);
        break;
    }
    case IDM_VOL_DOWN: {
        float v = mp_get_volume() - 0.05f;
        if (v < 0.0f) v = 0.0f;
        mp_set_volume(v);
        refresh_volume_item(bar);
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
            refresh_speed_check(GetSubMenu(GetMenu(g_hwnd), 1));
        } else if (id >= IDM_PLUGIN_BASE) {
            int i = id - IDM_PLUGIN_BASE;
            mp_plugin* p = mp_plugins_get(i);
            if (p) {
                mp_plugins_set_enabled(i, !p->enabled);
                mp_plugins_apply_skins(g_hwnd);
                rebuild_plugins_menu(GetMenu(g_hwnd));
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
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t path_w[MAX_PATH];
        DragQueryFileW((HDROP)wp, 0, path_w, MAX_PATH);
        DragFinish((HDROP)wp);
        char path_utf8[MAX_PATH * 3];
        wide_to_utf8(path_w, path_utf8, sizeof(path_utf8));
        if (mp_open(path_utf8) != 0) {
            wchar_t msg[600];
            swprintf(msg, 600, L"Impossible d'ouvrir \"%ls\".\nFormat non supporté ou fichier corrompu.", path_w);
            MessageBoxW(hwnd, msg, APP_TITLE, MB_ICONERROR);
        }
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE:   mp_play_pause(); break;
        case 'S':        mp_stop(); break;
        case VK_UP:      SendMessageW(hwnd, WM_COMMAND, IDM_VOL_UP, 0); break;
        case VK_DOWN:    SendMessageW(hwnd, WM_COMMAND, IDM_VOL_DOWN, 0); break;
        case 'O':
            if (GetKeyState(VK_CONTROL) & 0x8000) do_open_dialog();
            break;
        }
        status_update();
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
        status_update();
        return 0;
    case WM_COMMAND:
        on_command(LOWORD(wp), GetMenu(hwnd));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        get_center_rect(hwnd, &rc);
        paint_center(hdc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: {
        SendMessageW(g_status, WM_SIZE, 0, 0);
        return 0;
    }
    case WM_CLOSE:
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

    fprintf(log, "MusicPlayer " MP_VERSION " — selftest\n");
    fprintf(log, "FFmpeg : %s\n", av_version_info());

    mp_init();
    fprintf(log, "Périphérique audio : %s\n", mp_audio_device_ok() ? "OK" : "ABSENT (mode silencieux)");
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
            fprintf(log, "  lecture   : état=%d pos=%.2fs dur=%.2fs -> %s\n",
                    st, pos, dur, ok ? "PASS" : "FAIL");

            /* test vitesse */
            mp_set_speed(2.0f);
            Sleep(150);
            float sp = mp_get_speed();
            int sp_ok = (sp == 2.0f);
            fprintf(log, "  vitesse   : x%.1f -> %s\n", sp, sp_ok ? "PASS" : "FAIL");
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
            fprintf(log, "  stop      : état=%d pos=%.2fs -> %s\n", st2, pos2,
                    stop_ok ? "PASS" : "FAIL");
            ok &= stop_ok;

            /* test lecture complète du fichier jusqu'à la fin */
            mp_play();
            Sleep((DWORD)(dur * 1000.0) + 1200);
            int fin_ok = (mp_get_state() == MP_STATE_FINISHED);
            fprintf(log, "  fin       : état=%d (durée=%.2fs) -> %s\n",
                    mp_get_state(), dur, fin_ok ? "PASS" : "FAIL");
            ok &= fin_ok;

            mp_stop();
        } else {
            ok = 0;
            fprintf(log, "  ouverture : FAIL\n");
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
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrev; (void)nCmdShow;

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

    resolve_plugins_dir();
    {
        char dir_utf8[MAX_PATH * 2], dbg[600];
        wide_to_utf8(g_plugins_dir, dir_utf8, sizeof(dir_utf8));
        _snprintf(dbg, sizeof(dbg), "Répertoire plugins : %s", dir_utf8);
        log_line(dbg);
    }
    mp_plugins_scan(g_plugins_dir, &g_host);

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
        _snprintf(dbg, sizeof(dbg), "RegisterClassW a échoué (err=%lu)", GetLastError());
        log_line(dbg);
    }

    g_hwnd = CreateWindowExW(0, L"MusicPlayerWnd", APP_TITLE,
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 620, 190,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) {
        char dbg[256];
        _snprintf(dbg, sizeof(dbg), "CreateWindowExW a échoué (err=%lu)", GetLastError());
        log_line(dbg);
        return 1;
    }
    log_line("Fenêtre créée");

    HMENU bar = create_menus();
    SetMenu(g_hwnd, bar);
    refresh_speed_check(GetSubMenu(bar, 1));
    rebuild_plugins_menu(bar);
    mp_plugins_apply_skins(g_hwnd);
    log_line("Menus construits");

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    log_line("Fenêtre affichée");

    /* fichier passé en ligne de commande : "MusicPlayer.exe chemin.mp3" */
    if (lpCmdLine && *lpCmdLine) {
        char file[MAX_PATH * 3];
        strncpy(file, lpCmdLine, sizeof(file) - 1);
        file[sizeof(file) - 1] = '\0';
        size_t len = strlen(file);
        if (len > 0 && file[0] == '"' && file[len - 1] == '"') {
            memmove(file, file + 1, len - 2);
            file[len - 2] = '\0';
        }
        if (mp_open(file) != 0) {
            char dbg[512];
            _snprintf(dbg, sizeof(dbg), "Ouverture en ligne de commande a échoué : %s", file);
            log_line(dbg);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
