/*
 * Plugin MusicPlayer — "3D Spectrum" (analyseur de spectre rotatif 3D)
 * ====================================================================
 * Type : visuel. Les barres du spectre sont disposées en cylindre autour
 * d'un axe vertical qui tourne en continu (projection perspective
 * maison, tri par profondeur — algorithme du peintre).
 */
#include "plugin.h"

#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FFT_SIZE 1024
#define NB_BARS   48
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
static HBRUSH   g_bg_bands[16];
static HBRUSH   g_axis_br;
static HBRUSH   g_floor_br;
static HBRUSH   g_white = NULL;

/* ------------------------------------------------------------------ */
static const char* name(void)        { return "3D Spectrum"; }
static const char* version(void)     { return "0.1.0"; }
static const char* description(void) { return "Spectre 3D rotatif (cylindre de barres)"; }
static unsigned type(void)           { return MP_PLUGIN_VISUAL; }

static void hsv_to_rgb3d(float h, float s, float v, BYTE* r, BYTE* g, BYTE* b)
{
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60)       { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; }
    else              { rr = c; bb = x; }
    *r = (BYTE)((rr + m) * 255.0f);
    *g = (BYTE)((gg + m) * 255.0f);
    *b = (BYTE)((bb + m) * 255.0f);
}

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

/* petit helper : brush blanc */
static HBRUSH g_palette_white(void)
{
    if (!g_white) g_white = CreateSolidBrush(RGB(255, 255, 255));
    return g_white;
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

    for (int i = 0; i < PAL_SIZE; i++) {
        float t = (float)i / (PAL_SIZE - 1);
        BYTE r, g, b;
        hsv_to_rgb3d(270.0f - 270.0f * t, 0.95f, 0.30f + 0.70f * t, &r, &g, &b);
        g_palette[i] = CreateSolidBrush(RGB(r, g, b));
        hsv_to_rgb3d(270.0f - 270.0f * t, 0.95f, 0.10f + 0.16f * t, &r, &g, &b);
        g_palette_dark[i] = CreateSolidBrush(RGB(r, g, b));
    }
    for (int i = 0; i < 16; i++) {
        float t = (float)i / 15.0f;
        g_bg_bands[i] = CreateSolidBrush(RGB(
            (BYTE)(8 + 8 * t), (BYTE)(8 + 10 * t), (BYTE)(18 + 26 * t)));
    }
    g_axis_br = CreateSolidBrush(RGB(120, 130, 160));
    g_floor_br = CreateSolidBrush(RGB(40, 46, 66));

    InitializeCriticalSection(&g_cap_lock);
    g_last_frames = 0;
    memset(g_last, 0, sizeof(g_last));
    memset(g_levels, 0, sizeof(g_levels));
    g_angle = 0.0f;

    if (g_host && g_host->log)
        g_host->log("3D Spectrum: init OK (48 bars, rotating cylinder)");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    if (g_white) { DeleteObject(g_white); g_white = NULL; }
    DeleteObject(g_axis_br);
    DeleteObject(g_floor_br);
    for (int i = 0; i < 16; i++) DeleteObject(g_bg_bands[i]);
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

    /* fond dégradé */
    int band_h = (height + 15) / 16;
    for (int i = 0; i < 16; i++) {
        RECT r = { 0, i * band_h, width, (i + 1) * band_h };
        if (r.top >= height) break;
        if (r.bottom > height) r.bottom = height;
        FillRect(hdc, &r, g_bg_bands[i]);
    }

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

    g_angle += 0.022f;
    if (g_angle > 6.2832f) g_angle -= 6.2832f;

    int cx = width / 2;
    int cy = height / 2;
    float rx = (float)(width * 0.34);
    float ry = (float)(height * 0.22);
    int max_h = height - 10;

    /* tri des barres par profondeur (derrière d'abord) */
    int order[NB_BARS];
    for (int i = 0; i < NB_BARS; i++) order[i] = i;
    for (int i = 0; i < NB_BARS - 1; i++)
        for (int j = i + 1; j < NB_BARS; j++) {
            float zi = sinf((float)order[i] / NB_BARS * 6.2832f + g_angle);
            float zj = sinf((float)order[j] / NB_BARS * 6.2832f + g_angle);
            if (zi > zj) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }

    /* sol : ellipse sombre */
    Ellipse(hdc, cx - (int)rx, cy - (int)(ry * 0.4f), cx + (int)rx, cy + (int)(ry * 0.4f));
    /* axe vertical */
    RECT axis = { cx - 2, 4, cx + 2, cy + (int)(ry * 0.4f) };
    FillRect(hdc, &axis, g_axis_br);

    /* barres */
    int bw = 6;
    for (int k = 0; k < NB_BARS; k++) {
        int b = order[k];
        float a = (float)b / NB_BARS * 6.2832f + g_angle;
        float s = sinf(a), c = cosf(a);
        int x = cx + (int)(c * rx);
        int y = cy + (int)(s * ry * 0.5f);
        int bh = (int)(g_levels[b] * max_h);
        if (bh < 2) bh = 2;
        int color = (int)(g_levels[b] * (PAL_SIZE - 1));
        if (color < 0) color = 0;
        if (color >= PAL_SIZE) color = PAL_SIZE - 1;
        /* barres derrière : plus sombres (profondeur) */
        HBRUSH br = (s < 0.0f) ? g_palette_dark[color] : g_palette[color];
        /* épaisseur variable selon la profondeur (fausse perspective) */
        int w = (s < 0.0f) ? bw - 2 : bw;
        if (w < 2) w = 2;
        RECT bar = { x - w / 2, y - bh, x + w / 2 + 1, y };
        FillRect(hdc, &bar, br);
        /* tête de barre claire */
        if (s >= 0.0f) {
            RECT cap = { x - w / 2, y - bh, x + w / 2 + 1, y - bh + 2 };
            FillRect(hdc, &cap, g_palette_white());
        }
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
