/*
 * MusicPlayer — plugin : paroles (lyrics) des chansons.
 * Type SERVICE. Lit le fichier .lrc placé à côté du morceau (même nom,
 * extension .lrc) et l'affiche dans une fenêtre.
 * Clic sur le plugin dans Plugins ▸ Services pour afficher/masquer.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "plugin.h"

static const mp_host_api* g_h = NULL;
static HWND g_win = NULL;

static const char* pl_name(void)    { return "Lyrics"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Shows the song lyrics (.lrc file next to the track)"; }
static unsigned pl_type(void) { return MP_PLUGIN_SERVICE; }

static LRESULT CALLBACK lyrics_wnd_proc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_DESTROY:
        g_win = NULL;
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
        /* redimensionne l'éditeur + le bouton */
        int w = LOWORD(lp), h = HIWORD(lp);
        HWND e = GetDlgItem(hw, 10);
        HWND b = GetDlgItem(hw, 1);
        if (e) SetWindowPos(e, NULL, 8, 8, w - 16, h - 52, SWP_NOZORDER);
        if (b) SetWindowPos(b, NULL, 8, h - 40, 90, 26, SWP_NOZORDER);
        return 0;
    }
    }
    return DefWindowProcW(hw, m, wp, lp);
}

/* Charge les paroles : fichier .lrc à côté du morceau, timestamps
 * [mm:ss.xx] retirés, conversion UTF-8 → UTF-16. */
static void load_lyrics(wchar_t* out, size_t cap)
{
    out[0] = 0;
    if (!g_h || !g_h->get_file_name) return;
    const char* fn = g_h->get_file_name();
    if (!fn || !fn[0]) return;

    char lrc[MAX_PATH * 3];
    strncpy(lrc, fn, sizeof(lrc) - 5);
    lrc[sizeof(lrc) - 5] = 0;
    char* dot = strrchr(lrc, '.');
    if (dot) strcpy(dot, ".lrc");

    FILE* f = fopen(lrc, "rb");
    if (!f) return;
    char* buf = (char*)malloc(65536);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, 65535, f);
    fclose(f);
    buf[n] = 0;

    /* retire les timestamps [mm:ss.xx] */
    char* src = buf;
    char* dst = buf;
    while (*src) {
        if (*src == '[') {
            char* e = strchr(src, ']');
            if (e) { src = e + 1; continue; }
        }
        *dst++ = *src++;
    }
    *dst = 0;

    MultiByteToWideChar(CP_UTF8, 0, buf, (int)(dst - buf), out, (int)cap);
    out[cap - 1] = 0;
    free(buf);
}

static void show_lyrics(void)
{
    if (g_win) { DestroyWindow(g_win); return; }   /* toggle */

    wchar_t text[65536];
    load_lyrics(text, 65536);
    if (text[0] == 0) {
        MessageBoxW(NULL, L"No .lrc lyrics file next to the track.",
                    L"Lyrics", MB_OK | MB_ICONINFORMATION);
        return;
    }

    HINSTANCE inst = GetModuleHandleW(NULL);
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = lyrics_wnd_proc;
        wc.hInstance = inst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MPLyrics";
        RegisterClassW(&wc);
        reg = 1;
    }

    g_win = CreateWindowExW(0, L"MPLyrics", L"Lyrics",
                            WS_OVERLAPPEDWINDOW, 180, 140, 480, 420,
                            NULL, NULL, inst, NULL);
    if (!g_win) return;
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                    ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                    8, 8, 460, 366, g_win, (HMENU)10, inst, NULL);
    HWND btn = CreateWindowW(L"BUTTON", L"Close",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                             8, 378, 90, 26, g_win, (HMENU)1, inst, NULL);
    HFONT font = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HFONT bfont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(GetDlgItem(g_win, 10), WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(btn, WM_SETFONT, (WPARAM)bfont, TRUE);
    ShowWindow(g_win, SW_SHOW);
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)self; (void)data;
    if (event == MP_SERVICE_CLICK) show_lyrics();
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    NULL, NULL,           /* init, destroy */
    NULL, NULL, NULL, NULL,   /* process, audio_frames, render, apply_skin */
    pl_service, NULL      /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
