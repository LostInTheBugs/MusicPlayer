/*
 * Plugin MusicPlayer — "Hypnotic" (tunnel hypnotique)
 * ===================================================
 * Type : visuel. Anneaux concentriques rotatifs à des vitesses
 * différentes, couleurs qui défilent, pulsation au rythme de la musique.
 */
#include "plugin.h"

#include <windows.h>
#include <math.h>

#define RINGS 9

static const mp_host_api* g_host = NULL;

static CRITICAL_SECTION g_lock;
static float g_energy = 0.0f;      /* RMS (0..1) */
static float g_angle = 0.0f;
static float g_hue = 0.0f;

static HBRUSH g_bg_bands[12];
static HPEN   g_pens[RINGS];

/* ------------------------------------------------------------------ */
static const char* name(void)        { return "Hypnotic"; }
static const char* version(void)     { return "0.1.0"; }
static const char* description(void) { return "Tunnel hypnotique rotatif, pulsé par la musique"; }
static unsigned type(void)           { return MP_PLUGIN_VISUAL; }

static void hsv_to_rgb_hy(float h, float s, float v, BYTE* r, BYTE* g, BYTE* b)
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

    for (int i = 0; i < 12; i++) {
        float t = (float)i / 11.0f;
        g_bg_bands[i] = CreateSolidBrush(RGB(
            (BYTE)(4 + 6 * t), (BYTE)(4 + 8 * t), (BYTE)(10 + 18 * t)));
    }
    for (int i = 0; i < RINGS; i++)
        g_pens[i] = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));

    InitializeCriticalSection(&g_lock);
    g_energy = 0.0f;
    g_angle = 0.0f;
    g_hue = 0.0f;

    if (g_host && g_host->log)
        g_host->log("Hypnotic: init OK (9 rotating rings, music-pulsed)");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    for (int i = 0; i < RINGS; i++) DeleteObject(g_pens[i]);
    for (int i = 0; i < 12; i++) DeleteObject(g_bg_bands[i]);
    DeleteCriticalSection(&g_lock);
    if (g_host && g_host->log) g_host->log("Hypnotic: unloaded");
    g_host = NULL;
}

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
static void render(mp_plugin* self, void* hdc_v, int width, int height)
{
    (void)self;
    if (width <= 0 || height <= 0) return;
    HDC hdc = (HDC)hdc_v;

    /* fond dégradé */
    int band_h = (height + 11) / 12;
    for (int i = 0; i < 12; i++) {
        RECT r = { 0, i * band_h, width, (i + 1) * band_h };
        if (r.top >= height) break;
        if (r.bottom > height) r.bottom = height;
        FillRect(hdc, &r, g_bg_bands[i]);
    }

    EnterCriticalSection(&g_lock);
    float energy = g_energy;
    LeaveCriticalSection(&g_lock);

    g_angle += 0.03f + energy * 0.05f;
    g_hue += 1.2f;
    if (g_hue > 360.0f) g_hue -= 360.0f;

    int cx = width / 2;
    int cy = height / 2;
    float max_r = sqrtf((float)(cx * cx + cy * cy));
    float pulse = 1.0f + energy * 0.35f;

    for (int i = 0; i < RINGS; i++) {
        float base = max_r * (0.12f + 0.10f * i) * pulse;
        float wob = sinf(g_angle * (float)(i + 1) * 0.7f) * (max_r * 0.02f);
        int r = (int)(base + wob);
        if (r < 4) r = 4;

        BYTE cr, cg, cb;
        hsv_to_rgb_hy(fmodf(g_hue + i * 38.0f, 360.0f), 0.9f,
                      0.5f + 0.5f * (0.5f + 0.5f * sinf(g_angle * 2.0f + i)),
                      &cr, &cg, &cb);
        DeleteObject(g_pens[i]);
        g_pens[i] = CreatePen(PS_SOLID, (i == 0 ? 3 : 2), RGB(cr, cg, cb));

        HPEN old = (HPEN)SelectObject(hdc, g_pens[i]);
        HBRUSH oldb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Ellipse(hdc, cx - r, cy - (int)(r * 0.45f), cx + r, cy + (int)(r * 0.45f));
        SelectObject(hdc, oldb);
        SelectObject(hdc, old);
    }

    /* rayons rotatifs (deux lignes opposées par anneau, effet moulinet) */
    for (int i = 0; i < RINGS; i += 2) {
        float a = g_angle * (float)(i + 1) * 0.5f;
        float r = max_r * (0.10f + 0.11f * i) * pulse;
        HPEN old = (HPEN)SelectObject(hdc, g_pens[i]);
        MoveToEx(hdc, cx, cy, NULL);
        LineTo(hdc, cx + (int)(cosf(a) * r), cy + (int)(sinf(a) * r * 0.45f));
        LineTo(hdc, cx - (int)(cosf(a) * r), cy - (int)(sinf(a) * r * 0.45f));
        SelectObject(hdc, old);
    }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    name, version, description, type,
    init, destroy,
    NULL,           /* pas d'effet audio */
    audio_frames,   /* flux audio (pulsation) */
    render,         /* rendu visuel */
    NULL            /* pas de skin */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
