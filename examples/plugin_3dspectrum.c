/*
 * Plugin MusicPlayer — "3D Spectrum" (analyseur de spectre rotatif 3D)
 * ====================================================================
 * Type : visuel. Style "Spectrum3D" (spectrum3d.sourceforge.net) :
 * fond noir, grille wireframe au sol, barres fines disposées en cylindre
 * qui tourne — devant lumineux (blanc→jaune→rouge), arrière sombre.
 * Projection perspective maison, tri par profondeur (peintre).
 */
#include "plugin.h"

#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FFT_SIZE 1024
#define NB_BARS   72
#define PAL_SIZE  256
#define CAP_SIZE  4096

static const mp_host_api* g_host = NULL;

static float    g_window[FFT_SIZE];
static float    g_last[CAP_SIZE];
static unsigned g_last_frames = 0;
static CRITICAL_SECTION g_cap_lock;

static float    g_levels[NB_BARS];
static float    g_angle = 0.0f;
static HBRUSH   g_palette[PAL_SIZE];
static HBRUSH   g_palette_dark[PAL_SIZE];
static HBRUSH   g_grid_pen_br;
static HPEN     g_grid_pen;
static HBRUSH   g_black;

/* ------------------------------------------------------------------ */
static const char* name(void)        { return "3D Spectrum"; }
static const char* version(void)     { return "0.2.0"; }
static const char* description(void) { return "Spectre 3D rotatif (style Spectrum3D)"; }
static unsigned type(void)           { return MP_PLUGIN_VISUAL; }

/* ------------------------------------------------------------------ */
/* FFT radix-2 (identique au plugin Spectrum)                          */
/* ------------------------------------------------------------------ */
static void fft_radix2(float* re, float* im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * 3.14159265358979f / (float)len;
        float w_re = cosf(ang), w_im = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cur_re = 1.0f, cur_im = 0.0f;
            int half = len >> 1;
            for (int k = 0; k < half; k++) {
                float u_re = re[i + k], u_im = im[i + k];
                float v_re = re[i + k + half] * cur_re - im[i + k + half] * cur_im;
                float v_im = re[i + k + half] * cur_im + im[i + k + half] * cur_re;
                re[i + k] = u_re + v_re;
                im[i + k] = u_im + v_im;
                re[i + k + half] = u_re - v_re;
                im[i + k + half] = u_im - v_im;
                float n_re = cur_re * w_re - cur_im * w_im;
                cur_im = cur_re * w_im + cur_im * w_re;
                cur_re = n_re;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Cycle de vie                                                        */
/* ------------------------------------------------------------------ */
static int init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_host = host;

    for (int i = 0; i < FFT_SIZE; i++)
        g_window[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * i / (FFT_SIZE - 1)));

    /* palette "spectrum3d" : blanc → jaune → orange → rouge selon la hauteur */
    for (int i = 0; i < PAL_SIZE; i++) {
        float t = (float)i / (PAL_SIZE - 1);
        BYTE r, g, b;
        if (t < 0.5f) {
            /* blanc → jaune pâle */
            r = 240; g = (BYTE)(240 - 30.0f * (t * 2.0f)); b = (BYTE)(240 - 90.0f * (t * 2.0f));
        } else {
            /* jaune → orange → rouge */
            float u = (t - 0.5f) * 2.0f;
            r = (BYTE)(240 - 40.0f * u);
            g = (BYTE)(210 - 170.0f * u);
            b = (BYTE)(60 - 55.0f * u);
            if (b < 0) b = 0;
        }
        g_palette[i] = CreateSolidBrush(RGB(r, g, b));
        /* version sombre pour les barres de derrière */
        g_palette_dark[i] = CreateSolidBrush(RGB(r / 4, g / 4, b / 4));
    }
    g_black = CreateSolidBrush(RGB(4, 4, 6));
    g_grid_pen = CreatePen(PS_SOLID, 1, RGB(72, 72, 80));

    InitializeCriticalSection(&g_cap_lock);
    g_last_frames = 0;
    memset(g_last, 0, sizeof(g_last));
    memset(g_levels, 0, sizeof(g_levels));
    g_angle = 0.0f;

    if (g_host && g_host->log)
        g_host->log("3D Spectrum: init OK (72 bars, Spectrum3D style)");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    DeleteObject(g_grid_pen);
    DeleteObject(g_black);
    for (int i = 0; i < PAL_SIZE; i++) {
        DeleteObject(g_palette[i]);
        DeleteObject(g_palette_dark[i]);
    }
    DeleteCriticalSection(&g_cap_lock);
    if (g_host && g_host->log) g_host->log("3D Spectrum: unloaded");
    g_host = NULL;
}

/* ------------------------------------------------------------------ */
/* Capture audio                                                       */
/* ------------------------------------------------------------------ */
static void audio_frames(mp_plugin* self, const float* samples, unsigned frames,
                         unsigned channels, unsigned sample_rate)
{
    (void)self; (void)sample_rate;
    if (channels == 0 || frames == 0) return;
    if (frames > CAP_SIZE) frames = CAP_SIZE;

    EnterCriticalSection(&g_cap_lock);
    for (unsigned i = 0; i < frames; i++) {
        float mono = samples[i * channels];
        for (unsigned c = 1; c < channels; c++)
            mono += samples[i * channels + c];
        g_last[i] = mono / (float)channels;
    }
    g_last_frames = frames;
    LeaveCriticalSection(&g_cap_lock);
}

/* ------------------------------------------------------------------ */
/* Rendu : cylindre de barres en rotation                              */
/* ------------------------------------------------------------------ */
static void render(mp_plugin* self, void* hdc_v, int width, int height)
{
    (void)self;
    if (width <= 0 || height <= 0) return;
    HDC hdc = (HDC)hdc_v;

    /* fond noir */
    RECT all = { 0, 0, width, height };
    FillRect(hdc, &all, g_black);

    /* niveaux FFT (dernier bloc) */
    float re[FFT_SIZE], im[FFT_SIZE];
    EnterCriticalSection(&g_cap_lock);
    unsigned n = g_last_frames;
    if (n > 0) { memset(re, 0, sizeof(re)); memcpy(re, g_last, n * sizeof(float)); }
    LeaveCriticalSection(&g_cap_lock);

    if (n > 0) {
        memset(im, 0, sizeof(im));
        for (int i = 0; i < FFT_SIZE; i++) re[i] *= g_window[i];
        fft_radix2(re, im, FFT_SIZE);
        const float gain = 10.0f;
        int max_bin = FFT_SIZE / 2;
        for (int b = 0; b < NB_BARS; b++) {
            float lo = powf((float)max_bin, (float)b / NB_BARS);
            float hi = powf((float)max_bin, (float)(b + 1) / NB_BARS);
            int ilo = (int)lo; if (ilo < 1) ilo = 1;
            int ihi = (int)hi; if (ihi > max_bin) ihi = max_bin;
            float mag = 0.0f;
            int cnt = 0;
            for (int k = ilo; k < ihi; k++) { mag += sqrtf(re[k] * re[k] + im[k] * im[k]); cnt++; }
            if (cnt > 0) mag /= cnt;
            mag = mag / (float)FFT_SIZE * gain;
            if (mag > 1.0f) mag = 1.0f;
            if (mag > g_levels[b]) g_levels[b] = mag;
            else g_levels[b] = mag + (g_levels[b] - mag) * 0.88f;
        }
    }

    g_angle += 0.025f;
    if (g_angle > 6.2832f) g_angle -= 6.2832f;

    int cx = width / 2;
    int cy = height / 2;
    float rx = (float)(width * 0.38);
    float ry = (float)(height * 0.24);
    int max_h = height - 8;

    /* --- grille wireframe au sol (comme Spectrum3D) --- */
    HPEN oldp = (HPEN)SelectObject(hdc, g_grid_pen);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    for (int ring = 1; ring <= 4; ring++) {
        float rr = rx * ring / 4.0f;
        Ellipse(hdc, cx - (int)rr, cy - (int)(rr * 0.32f), cx + (int)rr, cy + (int)(rr * 0.32f));
    }
    for (int i = 0; i < 12; i++) {
        float a = (float)i / 12.0f * 6.2832f;
        MoveToEx(hdc, cx, cy, NULL);
        LineTo(hdc, cx + (int)(cosf(a) * rx), cy + (int)(sinf(a) * rx * 0.32f));
    }
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);

    /* tri des barres par profondeur (derrière d'abord) */
    int order[NB_BARS];
    for (int i = 0; i < NB_BARS; i++) order[i] = i;
    for (int i = 0; i < NB_BARS - 1; i++)
        for (int j = i + 1; j < NB_BARS; j++) {
            float zi = sinf((float)order[i] / NB_BARS * 6.2832f + g_angle);
            float zj = sinf((float)order[j] / NB_BARS * 6.2832f + g_angle);
            if (zi > zj) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }

    /* barres fines en cylindre */
    int bw = 2;
    for (int k = 0; k < NB_BARS; k++) {
        int b = order[k];
        float a = (float)b / NB_BARS * 6.2832f + g_angle;
        float s = sinf(a), c = cosf(a);
        int x = cx + (int)(c * rx);
        int y = cy + (int)(s * ry * 0.32f);
        int bh = (int)(g_levels[b] * max_h);
        if (bh < 2) bh = 2;
        int color = (int)(g_levels[b] * (PAL_SIZE - 1));
        if (color < 0) color = 0;
        if (color >= PAL_SIZE) color = PAL_SIZE - 1;
        HBRUSH br = (s < 0.0f) ? g_palette_dark[color] : g_palette[color];
        RECT bar = { x - bw / 2, y - bh, x + bw / 2 + 1, y };
        FillRect(hdc, &bar, br);
    }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    name, version, description, type,
    init, destroy,
    NULL,           /* pas d'effet audio */
    audio_frames,   /* flux audio (analyse) */
    render,         /* rendu visuel */
    NULL            /* pas de skin */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
