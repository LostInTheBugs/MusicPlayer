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

#define FFT_SIZE 1024
#define COLS     24        /* colonnes de fréquences */
#define ROWS     14        /* rangées de temps (historique) */
#define SEGS     6         /* segments du dégradé vertical */
#define CAP_SIZE 4096

static const mp_host_api* g_host = NULL;

static float    g_window[FFT_SIZE];
static float    g_last[CAP_SIZE];
static unsigned g_last_frames = 0;
static CRITICAL_SECTION g_cap_lock;

static float  g_hist[ROWS][COLS];      /* hist[0] = maintenant, [ROWS-1] = passé */
static float  g_smooth[COLS];
static HBRUSH g_brushes[COLS][SEGS];   /* dégradé par colonne */
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
        }
    }
    g_black = CreateSolidBrush(RGB(0, 0, 28));
    g_grid_pen = CreatePen(PS_SOLID, 1, RGB(28, 32, 62));

    InitializeCriticalSection(&g_cap_lock);
    g_last_frames = 0;
    memset(g_last, 0, sizeof(g_last));
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
        for (int s = 0; s < SEGS; s++)
            DeleteObject(g_brushes[c][s]);
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
static void render(mp_plugin* self, void* hdc_v, int width, int height)
{
    (void)self;
    if (width <= 0 || height <= 0) return;
    HDC hdc = (HDC)hdc_v;

    /* fond bleu nuit */
    RECT all = { 0, 0, width, height };
    FillRect(hdc, &all, g_black);

    /* niveaux FFT → rangée courante */
    float re[FFT_SIZE], im[FFT_SIZE];
    EnterCriticalSection(&g_cap_lock);
    unsigned n = g_last_frames;
    if (n > 0) { memset(re, 0, sizeof(re)); memcpy(re, g_last, n * sizeof(float)); }
    LeaveCriticalSection(&g_cap_lock);

    if (n > 0) {
        memset(im, 0, sizeof(im));
        for (int i = 0; i < FFT_SIZE; i++) re[i] *= g_window[i];
        fft_radix2(re, im, FFT_SIZE);
        const float gain = 12.0f;
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
            else g_smooth[c] = row[c] + (g_smooth[c] - row[c]) * 0.93f;
            row[c] = g_smooth[c];
        }
        /* décalage : le présent devient l'historique */
        for (int r = ROWS - 1; r >= 1; r--)
            memcpy(g_hist[r], g_hist[r - 1], sizeof(g_hist[0]));
        memcpy(g_hist[0], row, sizeof(row));
    }

    /* --- géométrie : vue 3D isométrique de côté (axes en diagonale) --- */
    float ax = 0.985f, ay = 0.17f;    /* colonnes (fréquences) → bas-droite */
    float bx = 0.94f, by = 0.34f;     /* rangées (temps) → haut-gauche */
    float c0 = (COLS - 1) * 0.5f;
    float r0 = (ROWS - 1) * 0.5f;

    /* échelle : la grille tient dans la zone (fond rétréci 50 %) */
    float gw = COLS * ax + ROWS * bx;
    float gh = COLS * ay + ROWS * by;
    float cell = (float)width / gw;
    float cell_h = (float)(height * 0.78f) / gh;
    if (cell_h < cell) cell = cell_h;

    /* bbox projetée (avec le rétrécissement des rangées) pour centrer */
    float px_min = 1e9f, px_max = -1e9f, py_min = 1e9f, py_max = -1e9f;
    for (int rr = 0; rr <= ROWS; rr++) {
        float sc = 1.0f - 0.5f * (float)rr / ROWS;
        for (int cc = 0; cc <= COLS; cc++) {
            float x = ((cc - c0) * ax - (rr - r0) * bx) * cell * sc;
            float y = ((cc - c0) * ay + (rr - r0) * by) * cell * sc;
            if (x < px_min) px_min = x;
            if (x > px_max) px_max = x;
            if (y < py_min) py_min = y;
            if (y > py_max) py_max = y;
        }
    }
    int cx = (int)(width * 0.5f - (px_min + px_max) * 0.5f);
    float base_y = (float)height * 0.5f - (py_min + py_max) * 0.5f;
    int max_h = (int)(height * 0.40f);
    int bw = (int)(cell * 0.42f);
    if (bw < 2) bw = 2;

    /* grille au sol */
    HPEN oldp = (HPEN)SelectObject(hdc, g_grid_pen);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    for (int r = 0; r <= ROWS; r++) {
        float sc = 1.0f - 0.5f * (float)r / ROWS;
        float y = base_y + ((0 - c0) * ay + (r - r0) * by) * cell * sc;
        float x1 = cx + ((0 - c0) * ax - (r - r0) * bx) * cell * sc;
        float x2 = cx + ((COLS - c0) * ax - (r - r0) * bx) * cell * sc;
        MoveToEx(hdc, (int)x1, (int)y, NULL);
        LineTo(hdc, (int)x2, (int)y);
    }
    for (int c = 0; c <= COLS; c++) {
        float y0 = base_y + ((c - c0) * ay + (0 - r0) * by) * cell;
        float yf = base_y + ((c - c0) * ay + (ROWS - r0) * by) * cell * 0.5f;
        float x0 = cx + ((c - c0) * ax - (0 - r0) * bx) * cell;
        float xf = cx + ((c - c0) * ax - (ROWS - r0) * bx) * cell * 0.5f;
        MoveToEx(hdc, (int)x0, (int)y0, NULL);
        LineTo(hdc, (int)xf, (int)yf);
    }
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);

    /* barres : du fond (r grand) vers l'avant (r=0) */
    for (int r = ROWS - 1; r >= 0; r--) {
        float sc = 1.0f - 0.5f * (float)r / (ROWS - 1);
        int bh_max = (int)(max_h * sc);
        int bw2 = (int)(bw * sc);
        if (bw2 < 2) bw2 = 2;
        int lat = bw2 / 3;
        if (lat < 1) lat = 1;
        for (int c = 0; c < COLS; c++) {
            float lvl = g_hist[r][c];
            int bh = (int)(lvl * bh_max);
            if (bh < 2) continue;
            float x = cx + ((c - c0) * ax - (r - r0) * bx) * cell * sc;
            float yb = base_y + ((c - c0) * ay + (r - r0) * by) * cell * sc;
            int x0 = (int)x - bw2 / 2;
            int ybi = (int)yb;

            /* face avant : dégradé vertical (sombre → lumineux) */
            for (int s = 0; s < SEGS; s++) {
                int y0 = ybi - bh + bh * s / SEGS;
                int y1 = y0 + bh / SEGS + 1;
                RECT rc = { x0, y0, x0 + bw2, y1 };
                FillRect(hdc, &rc, g_brushes[c][s]);
            }
            /* face latérale gauche (vers le fond, sombre) */
            POINT side[4] = {
                { x0, ybi },
                { x0 - lat, ybi - lat / 2 },
                { x0 - lat, ybi - lat / 2 - bh },
                { x0, ybi - bh }
            };
            HBRUSH oldb2 = (HBRUSH)SelectObject(hdc, g_brushes[c][0]);
            HPEN oldp2 = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
            Polygon(hdc, side, 4);
            /* dessus (lumineux) */
            POINT top[4] = {
                { x0, ybi - bh },
                { x0 - lat, ybi - lat / 2 - bh },
                { x0 + bw2 - lat, ybi - lat / 2 - bh },
                { x0 + bw2, ybi - bh }
            };
            SelectObject(hdc, g_brushes[c][SEGS - 1]);
            Polygon(hdc, top, 4);
            SelectObject(hdc, oldb2);
            SelectObject(hdc, oldp2);
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
