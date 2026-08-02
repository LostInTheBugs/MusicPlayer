/*
 * Plugin MusicPlayer — "Fractal" (plasma fractal)
 * ================================================
 * Type : visuel. Plasma génératif calculé pixel par pixel (buffer DIB
 * en mémoire, BitBlt vers l'écran) : superposition d'ondes sinusoïdales
 * avec rotation des couleurs. L'énergie de la musique module la vitesse
 * d'évolution et l'échelle des ondes.
 */
#include "plugin.h"

#include <windows.h>
#include <math.h>
#include <string.h>

#define PAL_SIZE 256

static const mp_host_api* g_host = NULL;

static CRITICAL_SECTION g_lock;
static float g_energy = 0.0f;          /* RMS du dernier bloc (0..1) */

static BYTE   g_pal[PAL_SIZE][3];      /* palette RGB précalculée */
static HBITMAP g_dib = NULL;
static void*   g_bits = NULL;
static int     g_bmp_w = 0, g_bmp_h = 0;
static HDC     g_memdc = NULL;
static float   g_t = 0.0f;

/* ------------------------------------------------------------------ */
static const char* name(void)        { return "Fractal"; }
static const char* version(void)     { return "0.1.0"; }
static const char* description(void) { return "Plasma fractal (ondes sinusoïdales, rotation de couleurs)"; }
static unsigned type(void)           { return MP_PLUGIN_VISUAL; }

static void hsv_to_rgb_fr(float h, float s, float v, BYTE* r, BYTE* g, BYTE* b)
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
static int init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_host = host;

    /* palette : anneau HSV complet, 3 boucles de teinte */
    for (int i = 0; i < PAL_SIZE; i++) {
        float h = (float)(i % 96) / 96.0f * 360.0f;
        hsv_to_rgb_fr(h, 1.0f, 0.55f + 0.45f * (float)(i % 64) / 63.0f,
                      &g_pal[i][0], &g_pal[i][1], &g_pal[i][2]);
    }

    InitializeCriticalSection(&g_lock);
    g_energy = 0.0f;
    g_t = 0.0f;

    if (g_host && g_host->log)
        g_host->log("Fractal: init OK (plasma, 256-color palette)");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    if (g_dib) { DeleteObject(g_dib); g_dib = NULL; }
    if (g_memdc) { DeleteDC(g_memdc); g_memdc = NULL; }
    DeleteCriticalSection(&g_lock);
    if (g_host && g_host->log) g_host->log("Fractal: unloaded");
    g_host = NULL;
}

/* ------------------------------------------------------------------ */
/* Flux audio : niveau d'énergie (module la vitesse du plasma)         */
/* ------------------------------------------------------------------ */
static void audio_frames(mp_plugin* self, const float* samples, unsigned frames,
                         unsigned channels, unsigned sample_rate)
{
    (void)self; (void)sample_rate;
    if (channels == 0 || frames == 0) return;

    double sum = 0.0;
    for (unsigned i = 0; i < frames * channels; i++) {
        float v = samples[i];
        sum += (double)v * v;
    }
    float e = (float)sqrt(sum / (frames * channels));
    EnterCriticalSection(&g_lock);
    g_energy = 0.85f * g_energy + 0.15f * e;
    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------------------------ */
/* Rendu : plasma pixel par pixel dans un DIB                          */
/* ------------------------------------------------------------------ */
static void render(mp_plugin* self, void* hdc_v, int width, int height)
{
    (void)self;
    if (width <= 0 || height <= 0) return;
    HDC hdc = (HDC)hdc_v;

    /* recrée le DIB si la taille change */
    if (!g_dib || g_bmp_w != width || g_bmp_h != height) {
        if (g_dib) DeleteObject(g_dib);
        if (g_memdc) DeleteDC(g_memdc);
        g_memdc = CreateCompatibleDC(hdc);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;   /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_dib = CreateDIBSection(g_memdc, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
        SelectObject(g_memdc, g_dib);
        g_bmp_w = width;
        g_bmp_h = height;
    }

    EnterCriticalSection(&g_lock);
    float energy = g_energy;
    LeaveCriticalSection(&g_lock);

    g_t += 0.02f + energy * 0.10f;
    float t = g_t;

    /* échelle des ondes : légèrement modulée par l'énergie */
    float s1 = 0.045f + energy * 0.02f;
    float s2 = 0.033f - energy * 0.008f;
    float s3 = 0.022f;

    unsigned char* px = (unsigned char*)g_bits;
    float cx = width * 0.5f, cy = height * 0.5f;
    float phase = t * 2.2f;

    for (int y = 0; y < height; y++) {
        float yy = (float)y;
        for (int x = 0; x < width; x++) {
            float xx = (float)x;
            float dx = xx - cx, dy = yy - cy;
            float v = sinf(xx * s1 + t) + sinf(yy * s2 - t * 0.7f) +
                      sinf((xx + yy) * s3 + phase) +
                      sinf(sqrtf(dx * dx + dy * dy) * 0.05f - t * 1.1f);
            int idx = (int)((v + 4.0f) * 0.125f * 255.0f) & 255;
            unsigned char* p = px + ((size_t)y * width + x) * 4;
            p[0] = g_pal[idx][2];
            p[1] = g_pal[idx][1];
            p[2] = g_pal[idx][0];
            p[3] = 0;
        }
    }

    BitBlt(hdc, 0, 0, width, height, g_memdc, 0, 0, SRCCOPY);
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    name, version, description, type,
    init, destroy,
    NULL,           /* pas d'effet audio */
    audio_frames,   /* flux audio (énergie) */
    render,         /* rendu visuel */
    NULL            /* pas de skin */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
