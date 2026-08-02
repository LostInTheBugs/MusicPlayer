/*
 * Plugin MusicPlayer — "Spectrum" (visualiseur de spectre)
 * ========================================================
 * Type : visuel. Analyse le flux audio (hook audio_frames) et dessine
 * un spectre logarithmique animé dans la zone d'affichage (~30 FPS).
 *
 * Implémentation :
 *   - FFT radix-2 maison (1024 points), fenêtre de Hann
 *   - 48 barres sur échelle de fréquences logarithmique
 *   - lissage (décroissance exponentielle des barres + pics lumineux)
 *   - palette de couleurs précalculée (vert -> jaune -> rouge)
 *
 * État conservé en variables statiques de la DLL (plugin à instance
 * unique) — pattern de référence pour les plugins visuels.
 */
#include "plugin.h"

#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FFT_SIZE 1024          /* points FFT (puissance de 2) */
#define NB_BARS  48            /* nombre de barres affichées */
#define PAL_SIZE 256           /* couleurs de la palette */
#define CAP_SIZE 4096          /* taille max d'un bloc capturé */

/* ------------------------------------------------------------------ */
/* État du plugin                                                      */
/* ------------------------------------------------------------------ */
static const mp_host_api* g_host = NULL;

static float    g_window[FFT_SIZE];      /* fenêtre de Hann */
static float    g_last[CAP_SIZE];        /* dernier bloc audio reçu (mono) */
static unsigned g_last_frames = 0;       /* frames valides dans g_last */
static CRITICAL_SECTION g_cap_lock;

static float    g_levels[NB_BARS];       /* niveaux lissés 0..1 */
static float    g_peaks[NB_BARS];        /* pics (décroissance lente) */
static HBRUSH   g_palette[PAL_SIZE];     /* barres (vives) */
static HBRUSH   g_palette_halo[PAL_SIZE];/* halo sombre autour des barres */
static HBRUSH   g_bg_bands[20];          /* fond en dégradé */
static HBRUSH   g_brush_grid;            /* grille discrète */
static HBRUSH   g_brush_peak;            /* pics blancs */

/* ------------------------------------------------------------------ */
/* Identification                                                      */
/* ------------------------------------------------------------------ */
static const char* name(void)        { return "Spectrum"; }
static const char* version(void)     { return "0.1.0"; }
static const char* description(void) { return "Visualiseur de spectre (FFT 1024, 48 barres log)"; }
static unsigned type(void)           { return MP_PLUGIN_VISUAL; }

/* ------------------------------------------------------------------ */
/* Couleurs : HSV -> RGB, palette vert->jaune->rouge par intensité     */
/* ------------------------------------------------------------------ */
static void hsv_to_rgb(float h, float s, float v, BYTE* r, BYTE* g, BYTE* b)
{
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60)      { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; }
    else               { rr = c; bb = x; }
    *r = (BYTE)((rr + m) * 255.0f);
    *g = (BYTE)((gg + m) * 255.0f);
    *b = (BYTE)((bb + m) * 255.0f);
}

/* ------------------------------------------------------------------ */
/* FFT radix-2 itérative (bit-reversal + papillons)                    */
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

    /* fenêtre de Hann */
    for (int i = 0; i < FFT_SIZE; i++)
        g_window[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * i / (FFT_SIZE - 1)));

    /* palette : hue 200° (bleu) -> 0° (rouge), luminosité croissante */
    for (int i = 0; i < PAL_SIZE; i++) {
        float t = (float)i / (PAL_SIZE - 1);
        BYTE r, g, b;
        hsv_to_rgb(200.0f * (1.0f - t), 0.92f, 0.30f + 0.70f * t, &r, &g, &b);
        g_palette[i] = CreateSolidBrush(RGB(r, g, b));
        /* halo : même teinte, bien plus sombre */
        hsv_to_rgb(200.0f * (1.0f - t), 0.92f, 0.10f + 0.18f * t, &r, &g, &b);
        g_palette_halo[i] = CreateSolidBrush(RGB(r, g, b));
    }
    /* fond : dégradé bleu nuit */
    for (int i = 0; i < 20; i++) {
        float t = (float)i / 19.0f;
        g_bg_bands[i] = CreateSolidBrush(RGB(
            (BYTE)(6 + 10 * t), (BYTE)(8 + 14 * t), (BYTE)(16 + 24 * t)));
    }
    g_brush_grid = CreateSolidBrush(RGB(32, 38, 58));
    g_brush_peak = CreateSolidBrush(RGB(255, 255, 255));

    InitializeCriticalSection(&g_cap_lock);
    g_last_frames = 0;
    memset(g_last, 0, sizeof(g_last));
    memset(g_levels, 0, sizeof(g_levels));
    memset(g_peaks, 0, sizeof(g_peaks));

    if (g_host && g_host->log) g_host->log("Spectrum : init OK (FFT 1024, 48 barres)");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    DeleteObject(g_brush_peak);
    DeleteObject(g_brush_grid);
    for (int i = 0; i < 20; i++) DeleteObject(g_bg_bands[i]);
    for (int i = 0; i < PAL_SIZE; i++) {
        DeleteObject(g_palette[i]);
        DeleteObject(g_palette_halo[i]);
    }
    DeleteCriticalSection(&g_cap_lock);
    if (g_host && g_host->log) g_host->log("Spectrum : déchargé");
    g_host = NULL;
}

/* ------------------------------------------------------------------ */
/* Capture audio (thread audio) : moyenne stéréo -> dernier bloc mono  */
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
/* Rendu (thread UI, ~30 FPS)                                          */
/* ------------------------------------------------------------------ */
static void render(mp_plugin* self, void* hdc_v, int width, int height)
{
    (void)self;
    if (width <= 0 || height <= 0) return;
    HDC hdc = (HDC)hdc_v;

    /* --- fond : dégradé bleu nuit --- */
    int band_h = (height + 19) / 20;
    for (int i = 0; i < 20; i++) {
        RECT r = { 0, i * band_h, width, (i + 1) * band_h };
        if (r.top >= height) break;
        if (r.bottom > height) r.bottom = height;
        FillRect(hdc, &r, g_bg_bands[i]);
    }
    /* grille horizontale discrète (25 / 50 / 75 %) */
    for (int g = 1; g < 4; g++) {
        RECT r = { 0, height * g / 4, width, height * g / 4 + 1 };
        FillRect(hdc, &r, g_brush_grid);
    }

    /* --- snapshot du dernier bloc reçu (sous lock) --- */
    float re[FFT_SIZE], im[FFT_SIZE];
    int have_signal = 0;
    EnterCriticalSection(&g_cap_lock);
    {
        unsigned n = g_last_frames;
        if (n > 0) {
            memset(re, 0, sizeof(re));
            memcpy(re, g_last, n * sizeof(float));
            have_signal = 1;
        }
    }
    LeaveCriticalSection(&g_cap_lock);

    /* --- niveaux par bandes logarithmiques (bin 1 .. FFT_SIZE/2) --- */
    const float gain = 10.0f;
    float new_levels[NB_BARS];
    int   new_counts[NB_BARS];
    memset(new_counts, 0, sizeof(new_counts));
    memset(new_levels, 0, sizeof(new_levels));

    if (have_signal) {
        memset(im, 0, sizeof(im));
        for (int i = 0; i < FFT_SIZE; i++) re[i] *= g_window[i];
        fft_radix2(re, im, FFT_SIZE);

        int max_bin = FFT_SIZE / 2;
        for (int b = 0; b < NB_BARS; b++) {
            float lo = powf((float)max_bin, (float)b / NB_BARS);
            float hi = powf((float)max_bin, (float)(b + 1) / NB_BARS);
            int ilo = (int)lo; if (ilo < 1) ilo = 1;
            int ihi = (int)hi; if (ihi > max_bin) ihi = max_bin;
            float mag = 0.0f;
            for (int k = ilo; k < ihi; k++) {
                mag += sqrtf(re[k] * re[k] + im[k] * im[k]);
                new_counts[b]++;
            }
            if (new_counts[b] > 0) mag /= (float)new_counts[b];
            mag = mag / (float)FFT_SIZE * gain;
            if (mag > 1.0f) mag = 1.0f;
            new_levels[b] = mag;
        }
    }

    /* --- lissage : montée rapide, descente lente --- */
    for (int b = 0; b < NB_BARS; b++) {
        float lvl = new_levels[b];
        if (lvl > g_levels[b]) g_levels[b] = lvl;
        else g_levels[b] = lvl + (g_levels[b] - lvl) * 0.90f;

        if (lvl >= g_peaks[b]) g_peaks[b] = lvl;
        else g_peaks[b] = lvl + (g_peaks[b] - lvl) * 0.95f;
    }

    /* --- dessin des barres --- */
    const int gap = 1;
    int bw = (width - (NB_BARS - 1) * gap) / NB_BARS;
    if (bw < 2) bw = 2;
    int base_y = height - 3;

    for (int b = 0; b < NB_BARS; b++) {
        int x = b * (bw + gap);
        int bh = (int)(g_levels[b] * (height - 10));
        if (bh > 0) {
            int color = (int)(g_levels[b] * (PAL_SIZE - 1));
            if (color < 0) color = 0;
            if (color >= PAL_SIZE) color = PAL_SIZE - 1;
            /* halo : barre élargie, sombre (effet glow) */
            RECT halo = { x - 1, base_y - bh, x + bw + 1, base_y };
            FillRect(hdc, &halo, g_palette_halo[color]);
            /* barre principale */
            RECT bar = { x, base_y - bh, x + bw, base_y };
            FillRect(hdc, &bar, g_palette[color]);
        }
        /* pic lumineux */
        if (g_peaks[b] > 0.02f) {
            int py = base_y - (int)(g_peaks[b] * (height - 10)) - 1;
            if (py < 0) py = 0;
            RECT pk = { x, py, x + bw, py + 2 };
            FillRect(hdc, &pk, g_brush_peak);
        }
    }

    /* liseré de base */
    RECT line = { 0, height - 1, width, height };
    FillRect(hdc, &line, (HBRUSH)GetStockObject(GRAY_BRUSH));
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    name, version, description, type,
    init, destroy,
    NULL,             /* pas d'effet audio */
    audio_frames,     /* flux audio (analyse) */
    render,           /* rendu visuel */
    NULL              /* pas de skin */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
