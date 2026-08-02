/*
 * Plugin MusicPlayer — "3D Isometric" (paysage de barres 3D)
 * ==========================================================
 * Type : visuel. Style "GLBars" / WM3DSpectrum : grille rectangulaire
 * de barres en perspective (les rangées du fond sont plus petites et
 * plus hautes), 24 colonnes de fréquences en arc-en-ciel, 14 rangées
 * de temps qui défilent. Chaque barre a un dégradé vertical : sombre
 * à la base, vive et lumineuse au sommet. Fond bleu nuit.
 */
#include "plugin.h"

#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#define FFT_SIZE 1024
#define COLS     24        /* colonnes de fréquences */
#define ROWS     14        /* rangées de temps (historique) */
#define SEGS     6         /* segments du dégradé vertical */
#define CAP_SIZE 32768     /* tampon audio : ~0,74 s à 44,1 kHz */

static const mp_host_api* g_host = NULL;

static float    g_window[FFT_SIZE];
static float    g_ring[CAP_SIZE];       /* tampon audio continu */
static unsigned g_ring_frames = 0;
static CRITICAL_SECTION g_cap_lock;

static float  g_hist[ROWS][COLS];      /* hist[0] = maintenant, [ROWS-1] = passé */
static float  g_smooth[COLS];
static HBRUSH g_brushes[COLS][SEGS];   /* dégradé par colonne */
static HBRUSH g_brushes_dark[COLS][SEGS]; /* version sombre (rangées du fond) */
static HBRUSH g_black;
static HPEN   g_grid_pen;

/* ------------------------------------------------------------------ */
static const char* name(void)        { return "3D Isometric"; }
static const char* version(void)     { return "0.4.0"; }
static const char* description(void) { return "Paysage de barres 3D en perspective (style GLBars)"; }
static unsigned type(void)           { return MP_PLUGIN_VISUAL; }

/* arc-en-ciel : hue 210° (bleu) → 0° (rouge) */
static void hue_to_rgb(float h, BYTE* r, BYTE* g, BYTE* b)
{
    float c = 1.0f;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    if (h < 60)       { *r = 255; *g = (BYTE)(x * 255); *b = 0; }
    else if (h < 120) { *r = (BYTE)(x * 255); *g = 255; *b = 0; }
    else if (h < 180) { *r = 0; *g = 255; *b = (BYTE)(x * 255); }
    else if (h < 240) { *r = 0; *g = (BYTE)(x * 255); *b = 255; }
    else if (h < 300) { *r = (BYTE)(x * 255); *g = 0; *b = 255; }
    else              { *r = 255; *g = 0; *b = (BYTE)(x * 255); }
}

/* ------------------------------------------------------------------ */
/* FFT radix-2                                                         */
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
static int init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_host = host;

    for (int i = 0; i < FFT_SIZE; i++)
        g_window[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * i / (FFT_SIZE - 1)));

    /* dégradé par colonne : sombre à la base → vif et lumineux au sommet */
    for (int c = 0; c < COLS; c++) {
        float h = 210.0f - 210.0f * (float)c / (COLS - 1);
        BYTE r, g, b;
        hue_to_rgb(h, &r, &g, &b);
        for (int s = 0; s < SEGS; s++) {
            float lum = 0.28f + 0.72f * (float)s / (SEGS - 1);   /* 0.28 → 1.0 */
            /* mélange avec du blanc pour le haut (éclat) */
            float wr = (float)s / (SEGS - 1) * 0.35f;
            BYTE rr = (BYTE)(r * lum * (1.0f - wr) + 255.0f * wr);
            BYTE gg = (BYTE)(g * lum * (1.0f - wr) + 255.0f * wr);
            BYTE bb = (BYTE)(b * lum * (1.0f - wr) + 255.0f * wr);
            g_brushes[c][s] = CreateSolidBrush(RGB(rr, gg, bb));
            g_brushes_dark[c][s] = CreateSolidBrush(RGB(rr / 2, gg / 2, bb / 2));
        }
    }
    g_black = CreateSolidBrush(RGB(0, 0, 28));
    g_grid_pen = CreatePen(PS_SOLID, 1, RGB(28, 32, 62));

    InitializeCriticalSection(&g_cap_lock);
    g_ring_frames = 0;
    memset(g_ring, 0, sizeof(g_ring));
    memset(g_hist, 0, sizeof(g_hist));
    memset(g_smooth, 0, sizeof(g_smooth));

    if (g_host && g_host->log)
        g_host->log("3D Isometric: init OK (24x14, perspective, gradient bars)");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    DeleteObject(g_black);
    DeleteObject(g_grid_pen);
    for (int c = 0; c < COLS; c++)
        for (int s = 0; s < SEGS; s++) {
            DeleteObject(g_brushes[c][s]);
            DeleteObject(g_brushes_dark[c][s]);
        }
    DeleteCriticalSection(&g_cap_lock);
    if (g_host && g_host->log) g_host->log("3D Isometric: unloaded");
    g_host = NULL;
}

/* ------------------------------------------------------------------ */
static void audio_frames(mp_plugin* self, const float* samples, unsigned frames,
                         unsigned channels, unsigned sample_rate)
{
    (void)self; (void)sample_rate;
    if (channels == 0 || frames == 0) return;
    if (frames > CAP_SIZE) frames = CAP_SIZE;

    /* mono + accumulation dans le tampon (ring simple) */
    EnterCriticalSection(&g_cap_lock);
    if (g_ring_frames + frames > CAP_SIZE) {
        unsigned drop = g_ring_frames + frames - CAP_SIZE;
        memmove(g_ring, g_ring + drop, (CAP_SIZE - frames) * sizeof(float));
        float* dst = g_ring + CAP_SIZE - frames;
        for (unsigned i = 0; i < frames; i++) {
            float m = samples[i * channels];
            for (unsigned c = 1; c < channels; c++)
                m += samples[i * channels + c];
            dst[i] = m / (float)channels;
        }
        g_ring_frames = CAP_SIZE;
    } else {
        float* dst = g_ring + g_ring_frames;
        for (unsigned i = 0; i < frames; i++) {
            float m = samples[i * channels];
            for (unsigned c = 1; c < channels; c++)
                m += samples[i * channels + c];
            dst[i] = m / (float)channels;
        }
        g_ring_frames += frames;
    }
    LeaveCriticalSection(&g_cap_lock);
}

/* ------------------------------------------------------------------ */
static void render(mp_plugin* self, void* hdc_v, int width, int height)
{
    (void)self;
    if (width <= 0 || height <= 0) return;
    HDC hdc = (HDC)hdc_v;

    /* fond bleu nuit */
    RECT all = { 0, 0, width, height };
    FillRect(hdc, &all, g_black);

    /* niveaux FFT : analyse le tampon à chaque frame (30 FPS) */
    float re[FFT_SIZE], im[FFT_SIZE];
    int has_data = 0;
    EnterCriticalSection(&g_cap_lock);
    if (g_ring_frames >= FFT_SIZE) {
        memcpy(re, g_ring + g_ring_frames - FFT_SIZE, FFT_SIZE * sizeof(float));
        has_data = 1;
    }
    LeaveCriticalSection(&g_cap_lock);

    if (has_data) {
        /* détection de silence : les zéros (rafales Wine / pause) ne doivent
         * pas vider le paysage — retombée très lente au lieu du lissage */
        float e = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) e += re[i] * re[i];
        e = sqrtf(e / FFT_SIZE);
        if (e < 0.002f) {
            for (int c = 0; c < COLS; c++) g_smooth[c] *= 0.995f;
        } else {
            memset(im, 0, sizeof(im));
            for (int i = 0; i < FFT_SIZE; i++) re[i] *= g_window[i];
            fft_radix2(re, im, FFT_SIZE);
        const float gain = 18.0f;
        int max_bin = FFT_SIZE / 2;
        float row[COLS];
        for (int c = 0; c < COLS; c++) {
            float lo = powf((float)max_bin, (float)c / COLS);
            float hi = powf((float)max_bin, (float)(c + 1) / COLS);
            int ilo = (int)lo; if (ilo < 1) ilo = 1;
            int ihi = (int)hi; if (ihi > max_bin) ihi = max_bin;
            float mag = 0.0f;
            int cnt = 0;
            for (int k = ilo; k < ihi; k++) { mag += sqrtf(re[k] * re[k] + im[k] * im[k]); cnt++; }
            if (cnt > 0) mag /= cnt;
            mag = mag / (float)FFT_SIZE * gain;
            if (mag > 1.0f) mag = 1.0f;
            row[c] = mag;
        }
            /* lissage : montée rapide, descente lente */
            for (int c = 0; c < COLS; c++) {
                if (row[c] > g_smooth[c]) g_smooth[c] = row[c];
                else g_smooth[c] = row[c] + (g_smooth[c] - row[c]) * 0.95f;
                row[c] = g_smooth[c];
            }
            /* décalage : le présent devient l'historique */
            for (int r = ROWS - 1; r >= 1; r--)
                memcpy(g_hist[r], g_hist[r - 1], sizeof(g_hist[0]));
            memcpy(g_hist[0], row, sizeof(row));
        }
    }

    /* --- géométrie : caméra frontale basse (vue "côté" de la grille) ---
     * Les rangées s'élèvent en rétrécissant vers le centre (point de
     * fuite central) : la grille au sol est un trapèze, les barres du
     * fond sont plus petites, plus hautes et plus sombres. */
    float c0 = (COLS - 1) * 0.5f;
    float cell = (float)width * 0.92f / COLS;      /* pas des colonnes au premier plan */
    float depth_h = (float)height * 0.42f;         /* hauteur de la grille (profondeur) */
    int   max_h = (int)(height * 0.40f);           /* barre max au premier plan */
    float sc_min = 0.35f;                          /* taille du fond (point de fuite) */

    /* centrage : grille au sol + barres */
    float base_y = (float)height * 0.5f + (depth_h + max_h * sc_min) * 0.5f;
    int   cx = width / 2;
    int   bw = (int)(cell * 0.44f);
    if (bw < 2) bw = 2;

    /* grille au sol : trapèze */
    HPEN oldp = (HPEN)SelectObject(hdc, g_grid_pen);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    for (int r = 0; r <= ROWS; r++) {
        float sc = 1.0f - (1.0f - sc_min) * (float)r / ROWS;
        float y = base_y - (float)r / ROWS * depth_h;
        float half = c0 * cell * sc;
        MoveToEx(hdc, cx - (int)half, (int)y, NULL);
        LineTo(hdc, cx + (int)half, (int)y);
    }
    for (int c = 0; c <= COLS; c++) {
        float x0 = cx + (c - c0) * cell;
        float xf = cx + (c - c0) * cell * sc_min;
        MoveToEx(hdc, (int)x0, (int)base_y, NULL);
        LineTo(hdc, (int)xf, (int)(base_y - depth_h));
    }
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);

    /* barres : du fond (r grand) vers l'avant (r=0) */
    for (int r = ROWS - 1; r >= 0; r--) {
        float t = (float)r / (ROWS - 1);
        float sc = 1.0f - (1.0f - sc_min) * t;     /* 1.0 → 0.35 */
        int bh_max = (int)(max_h * sc);
        int bw2 = (int)(bw * sc);
        if (bw2 < 2) bw2 = 2;
        float yb = base_y - t * depth_h;
        int dark = (r > ROWS / 2);                 /* rangées du fond plus sombres */
        for (int c = 0; c < COLS; c++) {
            float lvl = g_hist[r][c];
            int bh = (int)(lvl * bh_max);
            if (bh < 2) continue;
            int x0 = cx + (int)((c - c0) * cell * sc) - bw2 / 2;
            int ybi = (int)yb;

            /* face avant : dégradé vertical (sombre → lumineux) */
            for (int s = 0; s < SEGS; s++) {
                int y0 = ybi - bh + bh * s / SEGS;
                int y1 = y0 + bh / SEGS + 1;
                RECT rc = { x0, y0, x0 + bw2, y1 };
                FillRect(hdc, &rc, dark ? g_brushes_dark[c][s] : g_brushes[c][s]);
            }
            /* sommet lumineux (ligne claire) */
            RECT cap = { x0, ybi - bh, x0 + bw2, ybi - bh + 2 };
            FillRect(hdc, &cap, g_brushes[c][SEGS - 1]);
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
