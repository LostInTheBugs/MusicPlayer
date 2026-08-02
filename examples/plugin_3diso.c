/*
 * Plugin MusicPlayer — "3D Isometric" (paysage de barres 3D isométrique)
 * ======================================================================
 * Type : visuel. Grille rectangulaire de bâtons vue en perspective
 * isométrique : les colonnes = fréquences (arc-en-ciel de gauche à
 * droite), les rangées = le temps qui défile (la rangée avant = maintenant,
 * les rangées arrière = l'historique). Style "WM3DSpectrum" / spectrogramme
 * 3D (faberacoustical, researchgate).
 */
#include "plugin.h"

#include <windows.h>
#include <math.h>
#include <string.h>

#define FFT_SIZE 1024
#define COLS     30        /* colonnes de fréquences */
#define ROWS     16        /* rangées de temps (historique) */
#define CAP_SIZE 4096

static const mp_host_api* g_host = NULL;

static float    g_window[FFT_SIZE];
static float    g_last[CAP_SIZE];
static unsigned g_last_frames = 0;
static CRITICAL_SECTION g_cap_lock;

static float g_hist[ROWS][COLS];     /* hist[0] = maintenant, [ROWS-1] = passé */
static float g_smooth[COLS];         /* lissage des colonnes */
static HBRUSH g_palette[COLS];
static HBRUSH g_palette_dark[COLS];
static HBRUSH g_black;
static HPEN   g_grid_pen;

/* ------------------------------------------------------------------ */
static const char* name(void)        { return "3D Isometric"; }
static const char* version(void)     { return "0.1.0"; }
static const char* description(void) { return "Paysage de barres 3D isométrique (spectrogramme)"; }
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

    /* palette : une couleur par colonne (fréquence) */
    for (int c = 0; c < COLS; c++) {
        float h = 210.0f - 210.0f * (float)c / (COLS - 1);
        BYTE r, g, b;
        hue_to_rgb(h, &r, &g, &b);
        g_palette[c] = CreateSolidBrush(RGB(r, g, b));
        g_palette_dark[c] = CreateSolidBrush(RGB(r / 3, g / 3, b / 3));
    }
    g_black = CreateSolidBrush(RGB(0, 0, 28));
    g_grid_pen = CreatePen(PS_SOLID, 1, RGB(24, 28, 60));

    InitializeCriticalSection(&g_cap_lock);
    g_last_frames = 0;
    memset(g_last, 0, sizeof(g_last));
    memset(g_hist, 0, sizeof(g_hist));

    if (g_host && g_host->log)
        g_host->log("3D Isometric: init OK (30x16 bars, iso projection)");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    DeleteObject(g_black);
    DeleteObject(g_grid_pen);
    for (int c = 0; c < COLS; c++) {
        DeleteObject(g_palette[c]);
        DeleteObject(g_palette_dark[c]);
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

    /* géométrie iso : grille remplissant la zone, CENTRÉE sur l'écran */
    float cos30 = 0.866f, sin30 = 0.5f;
    float c0 = (COLS - 1) * 0.5f;   /* centre de la grille en colonnes */
    float r0 = (ROWS - 1) * 0.5f;   /* centre en rangées */
    float gw = (COLS + 1) * cos30 + (ROWS + 1) * cos30;   /* largeur projetée (dx=dy=1) */
    float gh = (COLS + 1) * sin30 + (ROWS + 1) * sin30;   /* hauteur projetée */
    float s = (float)width / gw;
    float sh = (float)(height * 0.80f) / gh;
    if (sh < s) s = sh;
    float dx = s, dy = s;

    int cx = width / 2;
    int max_h = (int)(height * 0.55f);

    /* centre vertical : grille au sol + hauteur des barres */
    float py_min = ((0 - c0) + (ROWS - 1 - r0)) * sin30 * dy;    /* arrière (haut) */
    float py_max = ((COLS - 1 - c0) + (0 - r0)) * sin30 * dy;    /* avant (bas) */
    float base_y = (float)height * 0.5f - (py_min + py_max - (float)max_h) * 0.5f;
    int bw = (int)(s * 0.55f);
    if (bw < 1) bw = 1;

    /* grille au sol */
    HPEN oldp = (HPEN)SelectObject(hdc, g_grid_pen);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    for (int r = 0; r <= ROWS; r++) {
        float x1 = ((0 - c0) - (r - r0)) * cos30 * dx;
        float y1 = ((0 - c0) + (r - r0)) * sin30 * dy;
        float x2 = ((COLS - c0) - (r - r0)) * cos30 * dx;
        float y2 = ((COLS - c0) + (r - r0)) * sin30 * dy;
        MoveToEx(hdc, cx + (int)x1, (int)(base_y + y1), NULL);
        LineTo(hdc, cx + (int)x2, (int)(base_y + y2));
    }
    for (int c = 0; c <= COLS; c++) {
        float x1 = ((c - c0) - (0 - r0)) * cos30 * dx;
        float y1 = ((c - c0) + (0 - r0)) * sin30 * dy;
        float x2 = ((c - c0) - (ROWS - r0)) * cos30 * dx;
        float y2 = ((c - c0) + (ROWS - r0)) * sin30 * dy;
        MoveToEx(hdc, cx + (int)x1, (int)(base_y + y1), NULL);
        LineTo(hdc, cx + (int)x2, (int)(base_y + y2));
    }
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);

    /* bâtons : de l'arrière (r grand) vers l'avant (r=0) */
    for (int r = ROWS - 1; r >= 0; r--) {
        for (int c = 0; c < COLS; c++) {
            float lvl = g_hist[r][c];
            int h = (int)(lvl * max_h);
            if (h < 1) continue;
            /* position au sol du bâton (centre de la cellule) */
            float px = cx + ((c - c0) - (r - r0)) * cos30 * dx + cos30 * dx * 0.5f;
            float py = base_y + ((c - c0) + (r - r0)) * sin30 * dy + sin30 * dy * 0.5f;
            int x = (int)px;
            int y = (int)py;
            int hw = bw / 2;

            /* face avant droite (claire) */
            RECT rd = { x, y - h, x + hw + 1, y };
            FillRect(hdc, &rd, g_palette[c]);
            /* face avant gauche (sombre) */
            RECT rg = { x - hw, y - h, x, y };
            FillRect(hdc, &rg, g_palette_dark[c]);
            /* sommet (losange clair) */
            POINT top[4] = {
                { x - hw, y - h },
                { x, y - h - hw },
                { x + hw, y - h },
                { x, y - h + hw }
            };
            HBRUSH oldb2 = (HBRUSH)SelectObject(hdc, g_palette[c]);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            HPEN oldp2 = (HPEN)SelectObject(hdc, pen);
            Polygon(hdc, top, 4);
            SelectObject(hdc, oldb2);
            SelectObject(hdc, oldp2);
            DeleteObject(pen);
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
