/*
 * MusicPlayer — plugin : jaquette (cover art).
 * Type SERVICE. Affiche l'image de la chanson en cours : jaquette
 * intégrée au MP3 (frame APIC des balises ID3v2) ou fichier
 * cover.jpg / folder.jpg / cover.png / front.jpg placé à côté du
 * morceau. Clic sur le plugin dans Plugins ▸ Services.
 */

#include <windows.h>
#include <gdiplus.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "plugin.h"

static const mp_host_api* g_h = NULL;
static HWND g_win = NULL;
static ULONG_PTR g_gdi_token = 0;
static GpImage* g_img = NULL;

static const char* pl_name(void)    { return "Cover art"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Shows the track cover art (APIC tag or cover.jpg next to the track)"; }
static unsigned pl_type(void) { return MP_PLUGIN_SERVICE; }

/* ------------------------------------------------------------------ */
/* GDI+ : chargement d'image                                           */
/* ------------------------------------------------------------------ */
static void gdi_start(void)
{
    if (g_gdi_token) return;
    GdiplusStartupInput in;
    memset(&in, 0, sizeof(in));
    in.GdiplusVersion = 1;
    GdiplusStartup(&g_gdi_token, &in, NULL);
}

static GpImage* load_image_file(const wchar_t* path)
{
    GpImage* img = NULL;
    if (GdipLoadImageFromFile(path, &img) == Ok)
        return img;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* APIC (ID3v2) : extrait les données de l'image intégrée              */
/* ------------------------------------------------------------------ */
static int extract_apic(const char* path, char** out, int* out_len)
{
    *out = NULL;
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3') {
        fclose(f);
        return 0;
    }
    unsigned sz = ((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) |
                  ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
    long pos = 10, end = 10 + (long)sz;
    int found = 0;
    while (pos + 10 <= end) {
        unsigned char fr[10];
        if (fseek(f, pos, SEEK_SET) != 0) break;
        if (fread(fr, 1, 10, f) != 10) break;
        unsigned fsz = ((unsigned)fr[4] << 24) | ((unsigned)fr[5] << 16) |
                       ((unsigned)fr[6] << 8) | fr[7];
        if (fr[0] == 'A' && fr[1] == 'P' && fr[2] == 'I' && fr[3] == 'C') {
            /* encodage(1) + mime(NUL) + type(1) + description(NUL) + data */
            if (fsz > 10 && fseek(f, pos + 10, SEEK_SET) == 0) {
                unsigned char* buf = (unsigned char*)malloc((size_t)fsz + 1);
                if (buf) {
                    if (fread(buf, 1, (size_t)fsz, f) == (size_t)fsz) {
                        const unsigned char* p = buf + 1;      /* encodage */
                        while (p < buf + fsz && *p) p++;       /* mime */
                        p++;                                    /* NUL */
                        if (p < buf + fsz) p++;                 /* type */
                        while (p < buf + fsz && *p) p++;        /* description */
                        p++;                                    /* NUL */
                        if (p < buf + fsz) {
                            long len = (long)(buf + fsz - p);
                            *out = (char*)malloc((size_t)len);
                            if (*out) {
                                memcpy(*out, p, (size_t)len);
                                *out_len = (int)len;
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
    fclose(f);
    return found;
}

/* ------------------------------------------------------------------ */
/* Fenêtre d'affichage                                                 */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK cover_wnd_proc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_DESTROY:
        g_win = NULL;
        return 0;
    case WM_CLOSE:
        DestroyWindow(hw);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hw, &ps);
        if (g_img) {
            RECT rc;
            GetClientRect(hw, &rc);
            int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            GpGraphics* gfx = NULL;
            if (GdipCreateFromHDC(hdc, &gfx) == Ok) {
                REAL iw = 0, ih = 0;
                GdipGetImageWidth(g_img, (REAL*)&iw);
                GdipGetImageHeight(g_img, (REAL*)&ih);
                if (iw > 0 && ih > 0 && cw > 0 && ch > 0) {
                    REAL scale = (REAL)cw / iw;
                    if (ih * scale > (REAL)ch) scale = (REAL)ch / ih;
                    REAL dw = iw * scale, dh = ih * scale;
                    GdipDrawImageRect(gfx, g_img,
                                      (REAL)((cw - dw) / 2), (REAL)((ch - dh) / 2),
                                      dw, dh);
                }
                GdipDeleteGraphics(gfx);
            }
        }
        EndPaint(hw, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hw, m, wp, lp);
}

static void show_cover(void)
{
    if (g_win) { DestroyWindow(g_win); return; }   /* toggle */
    if (!g_h || !g_h->get_file_name) return;
    const char* fn = g_h->get_file_name();
    if (!fn || !fn[0]) return;

    gdi_start();
    if (g_img) { GdipDisposeImage(g_img); g_img = NULL; }

    /* 1) fichier d'image dans le même dossier que le morceau */
    char dir[MAX_PATH * 3];
    strncpy(dir, fn, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char* slash = strrchr(dir, '/');
    if (!slash) slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0; else dir[0] = 0;

    static const char* names[] = { "cover.jpg", "folder.jpg", "cover.png", "front.jpg" };
    for (int i = 0; i < 4 && !g_img; i++) {
        char p[MAX_PATH * 3];
        _snprintf(p, sizeof(p), "%s%s", dir, names[i]);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
            wchar_t pw[MAX_PATH * 3];
            MultiByteToWideChar(CP_UTF8, 0, p, -1, pw, MAX_PATH * 3);
            g_img = load_image_file(pw);
        }
    }

    /* 2) jaquette intégrée (APIC) */
    if (!g_img) {
        char* data = NULL;
        int len = 0;
        if (extract_apic(fn, &data, &len) && len > 0) {
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            wcscat(tmp, L"mp_cover.bin");
            FILE* f = _wfopen(tmp, L"wb");
            if (f) {
                fwrite(data, 1, (size_t)len, f);
                fclose(f);
                g_img = load_image_file(tmp);
            }
            free(data);
        }
    }

    if (!g_img) {
        MessageBoxW(NULL,
            L"No cover art found (APIC tag or cover.jpg next to the track).",
            L"Cover", MB_OK | MB_ICONINFORMATION);
        return;
    }

    HINSTANCE inst = GetModuleHandleW(NULL);
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = cover_wnd_proc;
        wc.hInstance = inst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MPCover";
        RegisterClassW(&wc);
        reg = 1;
    }
    g_win = CreateWindowExW(0, L"MPCover", L"Cover art",
                            WS_OVERLAPPEDWINDOW, 220, 140, 360, 360,
                            NULL, NULL, inst, NULL);
    if (g_win) ShowWindow(g_win, SW_SHOW);
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)self; (void)data;
    if (event == MP_SERVICE_CLICK) show_cover();
}

static void pl_destroy(mp_plugin* self)
{
    (void)self;
    if (g_img) { GdipDisposeImage(g_img); g_img = NULL; }
    if (g_gdi_token) { GdiplusShutdown(g_gdi_token); g_gdi_token = 0; }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    NULL, pl_destroy,
    NULL, NULL, NULL, NULL,   /* process, audio_frames, render, apply_skin */
    pl_service, NULL          /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
