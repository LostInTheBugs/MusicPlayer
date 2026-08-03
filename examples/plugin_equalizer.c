/*
 * Equalizer — plugin AUDIO_EFFECT + SERVICE
 * =========================================
 * Égaliseur 10 bandes style Winamp :
 *   60 170 310 600 1k 3k 6k 12k 14k 16k Hz (±12 dB) + preamp (±12 dB)
 *
 * La fenêtre de l'égaliseur est DÉTACHÉE et s'attache sous la fenêtre
 * principale : elle la suit quand on la déplace (comme Winamp).
 *
 * - clic sur un curseur : ajuster la bande (vertical) / le preamp (horizontal)
 * - bouton ON (vert) en haut à droite : active/désactive l'effet
 * - bouton [x] : cache la fenêtre (rouvrir : menu Plugins ▸ Effets ▸ Equalizer)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <windowsx.h>

#include "../src/plugin.h"

static const mp_host_api* g_h = NULL;

/* ------------------------------------------------------------------ */
/* Traitement audio : 10 biquads peaking par canal                     */
/* ------------------------------------------------------------------ */
#define NBANDS 10
static const double g_freqs[NBANDS] = {
    60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000
};
static double g_gain[NBANDS];      /* dB, -12..+12 */
static double g_preamp = 0.0;      /* dB */
static volatile LONG g_eq_on = 1;

typedef struct {
    double b0, b1, b2, a1, a2;
    double x1, x2, y1, y2;
} bq_t;

static bq_t g_bq[NBANDS][2];       /* [bande][canal] */
static int g_inited = 0;
static int g_last_rate = 0;

static void bq_peaking(bq_t* q, double f, double gain_db, double qv,
                       double sr)
{
    double A = pow(10.0, gain_db / 40.0);
    double w = 2.0 * 3.14159265358979 * f / sr;
    double cw = cos(w), sw = sin(w);
    double alpha = sw / (2.0 * qv);
    double a0 = 1.0 + alpha / A;
    q->b0 = (1.0 + alpha * A) / a0;
    q->b1 = (-2.0 * cw) / a0;
    q->b2 = (1.0 - alpha * A) / a0;
    q->a1 = (-2.0 * cw) / a0;
    q->a2 = (1.0 - alpha / A) / a0;
    q->x1 = q->x2 = q->y1 = q->y2 = 0.0;
}

static double bq_run(bq_t* q, double x)
{
    double y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 -
               q->a1 * q->y1 - q->a2 * q->y2;
    q->x2 = q->x1;
    q->x1 = x;
    q->y2 = q->y1;
    q->y1 = y;
    return y;
}

static void eq_reinit(int rate)
{
    for (int b = 0; b < NBANDS; b++)
        for (int c = 0; c < 2; c++)
            bq_peaking(&g_bq[b][c], g_freqs[b], g_gain[b] + g_preamp,
                       1.1, (double)rate);
}

static void pl_process(mp_plugin* self, float* samples, unsigned frames,
                       unsigned channels, unsigned sample_rate)
{
    (void)self;
    if (!g_eq_on) return;
    if (!g_inited || g_last_rate != (int)sample_rate) {
        eq_reinit((int)sample_rate);
        g_last_rate = (int)sample_rate;
        g_inited = 1;
    }
    int nc = channels > 2 ? 2 : (int)channels;
    for (unsigned i = 0; i < frames; i++) {
        for (int c = 0; c < nc; c++) {
            double v = samples[i * channels + (unsigned)c];
            for (int b = 0; b < NBANDS; b++)
                v = bq_run(&g_bq[b][c], v);
            samples[i * channels + (unsigned)c] = (float)v;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Fenêtre de l'égaliseur (attachée sous la fenêtre principale)        */
/* ------------------------------------------------------------------ */
#define EQ_W 316
#define EQ_H 152

static HWND g_eq_win = NULL;
static RECT g_sl[NBANDS + 1];      /* 10 bandes + preamp */
static int g_drag = -1;

static void eq_set_gain(int idx, double db)
{
    if (db < -12.0) db = -12.0;
    if (db > 12.0) db = 12.0;
    if (idx < NBANDS) g_gain[idx] = db;
    else             g_preamp = db;
    g_inited = 0;                  /* recalcule les filtres */
}

static void eq_drag(int idx, POINT pt)
{
    if (idx < NBANDS) {
        RECT* s = &g_sl[idx];
        int half = (s->bottom - s->top) / 2;
        if (half <= 0) return;
        double v = (double)(s->top + half - pt.y) / (double)half;
        eq_set_gain(idx, v * 12.0);
    } else {
        RECT* s = &g_sl[NBANDS];
        int span = s->right - s->left;
        if (span <= 0) return;
        double v = (double)(pt.x - s->left) / (double)span;
        eq_set_gain(NBANDS, (v * 2.0 - 1.0) * 12.0);
    }
}

static void eq_follow(void)
{
    if (!g_eq_win || !g_h) return;
    HWND main = g_h->main_window();
    if (!main) return;
    RECT mr;
    GetWindowRect(main, &mr);
    RECT er;
    GetWindowRect(g_eq_win, &er);
    int w = er.right - er.left;
    int h = er.bottom - er.top;
    SetWindowPos(g_eq_win, NULL, mr.left, mr.bottom + 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void eq_paint(HDC hdc)
{
    RECT rc;
    GetClientRect(g_eq_win, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(28, 32, 40));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    HFONT f = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
    HFONT f2 = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, f);
    SetTextColor(hdc, RGB(120, 210, 120));
    RECT tr = { 8, 5, 150, 22 };
    DrawTextW(hdc, L"EQUALIZER", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* bouton ON */
    RECT on = { EQ_W - 74, 6, EQ_W - 36, 20 };
    HBRUSH onb = CreateSolidBrush(g_eq_on ? RGB(60, 160, 60) : RGB(90, 90, 100));
    FillRect(hdc, &on, onb);
    DeleteObject(onb);
    SelectObject(hdc, f2);
    SetTextColor(hdc, RGB(240, 240, 240));
    DrawTextW(hdc, g_eq_on ? L"ON" : L"OFF", -1, &on,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    /* bouton fermer */
    RECT xr = { EQ_W - 28, 6, EQ_W - 10, 20 };
    SetTextColor(hdc, RGB(200, 90, 80));
    DrawTextW(hdc, L"✕", -1, &xr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* sliders verticaux */
    for (int i = 0; i < NBANDS; i++) {
        RECT* s = &g_sl[i];
        HBRUSH trk = CreateSolidBrush(RGB(60, 66, 78));
        FillRect(hdc, s, trk);
        DeleteObject(trk);
        /* ligne 0 dB */
        int half = (s->bottom - s->top) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 140, 100));
        HPEN op = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, s->left, s->top + half, NULL);
        LineTo(hdc, s->right, s->top + half);
        SelectObject(hdc, op);
        DeleteObject(pen);
        /* curseur */
        double db = g_gain[i];
        int cy = s->top + half - (int)(db / 12.0 * half);
        RECT k = { s->left + 1, cy - 5, s->right - 1, cy + 5 };
        HBRUSH kb = CreateSolidBrush(RGB(140, 220, 140));
        FillRect(hdc, &k, kb);
        DeleteObject(kb);
        /* label fréquence */
        SelectObject(hdc, f2);
        SetTextColor(hdc, RGB(170, 180, 190));
        char lbl[16];
        if (g_freqs[i] >= 1000)
            _snprintf(lbl, sizeof(lbl), "%dk", (int)(g_freqs[i] / 1000.0));
        else
            _snprintf(lbl, sizeof(lbl), "%d", (int)g_freqs[i]);
        RECT lr = { s->left - 4, s->bottom + 3, s->right + 4, s->bottom + 18 };
        DrawTextA(hdc, lbl, -1, &lr, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, f);
    }

    /* preamp (horizontal, en bas) */
    {
        RECT* s = &g_sl[NBANDS];
        HBRUSH trk = CreateSolidBrush(RGB(60, 66, 78));
        FillRect(hdc, s, trk);
        DeleteObject(trk);
        double db = g_preamp;
        int cx = s->left + (int)((db + 12.0) / 24.0 * (s->right - s->left));
        RECT k = { cx - 5, s->top - 1, cx + 5, s->bottom + 1 };
        HBRUSH kb = CreateSolidBrush(RGB(140, 220, 140));
        FillRect(hdc, &k, kb);
        DeleteObject(kb);
        SelectObject(hdc, f2);
        SetTextColor(hdc, RGB(170, 180, 190));
        RECT lr = { 10, s->top - 2, 64, s->bottom + 2 };
        DrawTextW(hdc, L"PREAMP", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, f);
    }

    SelectObject(hdc, old);
    DeleteObject(f);
    DeleteObject(f2);
}

static LRESULT CALLBACK eq_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_TIMER:
        eq_follow();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        eq_paint(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        RECT on = { EQ_W - 74, 6, EQ_W - 36, 20 };
        RECT xr = { EQ_W - 28, 6, EQ_W - 10, 20 };
        if (PtInRect(&on, pt)) {
            InterlockedExchange(&g_eq_on, !g_eq_on);
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }
        if (PtInRect(&xr, pt)) {
            ShowWindow(h, SW_HIDE);
            return 0;
        }
        for (int i = 0; i <= NBANDS; i++) {
            if (PtInRect(&g_sl[i], pt)) {
                g_drag = i;
                eq_drag(i, pt);
                InvalidateRect(h, NULL, TRUE);
                return 0;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_drag >= 0) {
            POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            eq_drag(g_drag, pt);
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        g_drag = -1;
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_eq_win = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void eq_open(void)
{
    if (!g_h) return;
    HWND main = g_h->main_window();
    if (!main) return;
    if (g_eq_win) {
        ShowWindow(g_eq_win, SW_SHOW);
        eq_follow();
        return;
    }
    WNDCLASSW wc;
    if (!GetClassInfoW(GetModuleHandleW(NULL), L"MPEqWin", &wc)) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = eq_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"MPEqWin";
        RegisterClassW(&wc);
    }
    RECT mr;
    GetWindowRect(main, &mr);
    g_eq_win = CreateWindowExW(WS_EX_TOPMOST, L"MPEqWin", L"Equalizer",
                               WS_POPUP,
                               mr.left, mr.bottom + 2, EQ_W, EQ_H,
                               main, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_eq_win) return;
    /* géométrie des curseurs */
    int sl_w = 20, gap = 6;
    int x0 = 12;
    for (int i = 0; i < NBANDS; i++) {
        g_sl[i].left = x0 + i * (sl_w + gap);
        g_sl[i].top = 26;
        g_sl[i].right = g_sl[i].left + sl_w;
        g_sl[i].bottom = g_sl[i].top + 88;
    }
    g_sl[NBANDS].left = 74;
    g_sl[NBANDS].top = 134;
    g_sl[NBANDS].right = EQ_W - 14;
    g_sl[NBANDS].bottom = g_sl[NBANDS].top + 8;
    ShowWindow(g_eq_win, SW_SHOW);
    SetTimer(g_eq_win, 1, 200, NULL);
    eq_follow();
}

static void eq_close(void)
{
    if (g_eq_win) {
        DestroyWindow(g_eq_win);
        g_eq_win = NULL;
    }
}

/* ------------------------------------------------------------------ */
static const char* pl_name(void) { return "Equalizer"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "10-band Winamp-style equalizer with a detachable window docked under the main window"; }
static unsigned pl_type(void)
{ return MP_PLUGIN_AUDIO_EFFECT | MP_PLUGIN_SERVICE; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_h = host;
    for (int i = 0; i < NBANDS; i++) g_gain[i] = 0.0;
    g_preamp = 0.0;
    g_inited = 0;
    g_eq_on = 1;
    return 0;
}

static void pl_destroy(mp_plugin* self)
{
    (void)self;
    eq_close();
    g_h = NULL;
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)data;
    if (!g_h) return;
    if (event == MP_SERVICE_CLICK) {
        if (self->enabled) eq_open();
        else if (g_eq_win) ShowWindow(g_eq_win, SW_HIDE);
    } else if (event == MP_SERVICE_WEB_APPLY) {
        if (self->enabled) eq_open();
    }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    pl_init, pl_destroy,
    pl_process, NULL, NULL, NULL,  /* process, audio_frames, render, skin */
    pl_service, NULL               /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
