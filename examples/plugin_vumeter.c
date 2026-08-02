/*
 * Plugin MusicPlayer — "VUMeter" (style Winamp/XMMS)
 * ===================================================
 * Type : visuel. Deux colonnes de LED stéréo (24 LED par canal, échelle
 * -45..0 dB) avec pics qui retombent lentement et indicateur de clipping.
 *
 * Inspiré des visualisations rétro : waog01a (Winamp OpenGL), XMMS.
 */
#include "plugin.h"

#include <windows.h>
#include <math.h>

#define LEDS     24          /* LED par canal */
#define DB_FLOOR -45.0f      /* bas de l'échelle (dB) */
#define DB_CEIL    3.0f      /* haut de l'échelle (dB, au-delà = clip) */

static const mp_host_api* g_host = NULL;

static CRITICAL_SECTION g_lock;
static float g_level_db[2] = { DB_FLOOR, DB_FLOOR };  /* niveau lissé par canal */
static float g_peak_db[2]  = { DB_FLOOR, DB_FLOOR };  /* pic (retombe lentement) */
static unsigned g_frame = 0;                          /* compteur de frames (clip blink) */

static HBRUSH g_led_green, g_led_yellow, g_led_red, g_led_off, g_led_clip;
static HBRUSH g_bg, g_frame_br, g_peak_br, g_label_off;

/* ------------------------------------------------------------------ */
/* Identification                                                      */
/* ------------------------------------------------------------------ */
static const char* name(void)        { return "VUMeter"; }
static const char* version(void)     { return "0.1.0"; }
static const char* description(void) { return "VU mètre stéréo à LED (-45..0 dB, pics, clip)"; }
static unsigned type(void)           { return MP_PLUGIN_VISUAL; }

static float rms_to_db(float rms)
{
    if (rms < 1e-7f) return DB_FLOOR;
    float db = 20.0f * log10f(rms);
    if (db < DB_FLOOR) db = DB_FLOOR;
    return db;
}

/* ------------------------------------------------------------------ */
/* Cycle de vie                                                        */
/* ------------------------------------------------------------------ */
static int init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_host = host;

    g_led_green  = CreateSolidBrush(RGB(24, 200, 60));
    g_led_yellow = CreateSolidBrush(RGB(235, 200, 30));
    g_led_red    = CreateSolidBrush(RGB(235, 50, 40));
    g_led_clip   = CreateSolidBrush(RGB(255, 255, 255));
    g_led_off    = CreateSolidBrush(RGB(34, 40, 52));
    g_bg         = CreateSolidBrush(RGB(14, 16, 24));
    g_frame_br   = CreateSolidBrush(RGB(70, 78, 96));
    g_peak_br    = CreateSolidBrush(RGB(255, 235, 180));
    g_label_off  = CreateSolidBrush(RGB(60, 68, 84));

    InitializeCriticalSection(&g_lock);
    g_level_db[0] = g_level_db[1] = DB_FLOOR;
    g_peak_db[0] = g_peak_db[1] = DB_FLOOR;

    if (g_host && g_host->log)
        g_host->log("VUMeter : init OK (24 LED/canal, -45..+3 dB)");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    DeleteObject(g_led_green); DeleteObject(g_led_yellow); DeleteObject(g_led_red);
    DeleteObject(g_led_clip); DeleteObject(g_led_off);
    DeleteObject(g_bg); DeleteObject(g_frame_br); DeleteObject(g_peak_br);
    DeleteObject(g_label_off);
    DeleteCriticalSection(&g_lock);
    if (g_host && g_host->log) g_host->log("VUMeter : déchargé");
    g_host = NULL;
}

/* ------------------------------------------------------------------ */
/* Flux audio : RMS par canal                                          */
/* ------------------------------------------------------------------ */
static void audio_frames(mp_plugin* self, const float* samples, unsigned frames,
                         unsigned channels, unsigned sample_rate)
{
    (void)self; (void)sample_rate;
    if (channels < 2 || frames == 0) return;

    double sum0 = 0.0, sum1 = 0.0;
    for (unsigned i = 0; i < frames; i++) {
        float l = samples[i * channels];
        float r = samples[i * channels + 1];
        sum0 += (double)l * l;
        sum1 += (double)r * r;
    }
    float db0 = rms_to_db((float)sqrt(sum0 / frames));
    float db1 = rms_to_db((float)sqrt(sum1 / frames));

    EnterCriticalSection(&g_lock);
    for (int c = 0; c < 2; c++) {
        float db = c == 0 ? db0 : db1;
        /* montée rapide, descente lente */
        if (db > g_level_db[c]) g_level_db[c] = db;
        else g_level_db[c] = db + (g_level_db[c] - db) * 0.80f;
        if (db > g_peak_db[c]) g_peak_db[c] = db;
        else g_peak_db[c] = db + (g_peak_db[c] - db) * 0.90f;
    }
    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------------------------ */
/* Rendu : 2 colonnes de LED + pics + clip                             */
/* ------------------------------------------------------------------ */
static void render(mp_plugin* self, void* hdc_v, int width, int height)
{
    (void)self;
    if (width <= 0 || height <= 0) return;
    HDC hdc = (HDC)hdc_v;

    /* fond + cadre */
    RECT all = { 0, 0, width, height };
    FillRect(hdc, &all, g_bg);
    FrameRect(hdc, &all, g_frame_br);

    EnterCriticalSection(&g_lock);
    float lvl[2] = { g_level_db[0], g_level_db[1] };
    float pk[2]  = { g_peak_db[0], g_peak_db[1] };
    LeaveCriticalSection(&g_lock);
    g_frame++;

    int margin = 14;
    int gap_x = 12;
    int top = 16;
    int bottom = height - 18;
    int lh = (bottom - top) / LEDS;          /* hauteur d'une LED */
    if (lh < 3) lh = 3;
    int lw = (width - 2 * margin - gap_x) / 2 / 4;  /* largeur d'une LED (4 = épaisseur) */
    if (lw < 3) lw = 3;
    int led_w = lw * 3;                      /* largeur du bloc de LED */

    for (int c = 0; c < 2; c++) {
        int x0 = margin + c * ((width - 2 * margin - gap_x) / 2 + gap_x);
        x0 += ((width - 2 * margin - gap_x) / 2 - led_w) / 2;

        /* LED allumées : -45..0 dB -> 0..LEDS */
        int lit = (int)((lvl[c] - DB_FLOOR) / (0.0f - DB_FLOOR) * LEDS);
        if (lit < 0) lit = 0;
        if (lit > LEDS) lit = LEDS;
        int peak = (int)((pk[c] - DB_FLOOR) / (0.0f - DB_FLOOR) * LEDS);
        if (peak < 0) peak = 0;
        if (peak > LEDS) peak = LEDS;

        for (int i = 0; i < LEDS; i++) {
            int y = bottom - (i + 1) * lh;
            RECT led = { x0, y, x0 + led_w, y + lh - 2 };
            HBRUSH br = g_led_off;
            if (i < lit) {
                float frac = (float)i / LEDS;
                if (frac < 0.55f) br = g_led_green;
                else if (frac < 0.82f) br = g_led_yellow;
                else br = g_led_red;
            }
            FillRect(hdc, &led, br);
        }

        /* pic : ligne claire (blink si clip) */
        if (peak >= 0) {
            int y = bottom - (peak + 1) * lh;
            if (y < top) y = top;
            RECT pk_rc = { x0 - 2, y, x0 + led_w + 2, y + 2 };
            int clip = (pk[c] > 0.0f);
            FillRect(hdc, &pk_rc, clip && (g_frame & 8) ? g_led_clip : g_peak_br);
        }

        /* label L / R */
        wchar_t lab[2] = { c == 0 ? L'L' : L'R', 0 };
        HFONT f = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hdc, f);
        SetTextColor(hdc, RGB(190, 200, 220));
        SetBkMode(hdc, TRANSPARENT);
        RECT lr = { x0 - 4, 1, x0 + led_w + 4, 14 };
        DrawTextW(hdc, lab, 1, &lr, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, old);
        DeleteObject(f);
    }

    /* graduations dB (sous chaque bloc) */
    SetTextColor(hdc, RGB(110, 120, 140));
    HFONT f2 = CreateFontW(10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT old2 = (HFONT)SelectObject(hdc, f2);
    for (int c = 0; c < 2; c++) {
        int x0 = margin + c * ((width - 2 * margin - gap_x) / 2 + gap_x);
        x0 += ((width - 2 * margin - gap_x) / 2 - led_w) / 2;
        RECT gd = { x0 - 4, height - 14, x0 + led_w + 4, height - 2 };
        DrawTextW(hdc, L"0  -15  -30  -45", -1, &gd, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    SelectObject(hdc, old2);
    DeleteObject(f2);
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
