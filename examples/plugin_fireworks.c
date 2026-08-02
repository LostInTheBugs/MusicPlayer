/*
 * Plugin MusicPlayer — "Fireworks" (feu d'artifice)
 * ==================================================
 * Type : visuel. Les explosions sont déclenchées par la musique :
 * chaque pic d'énergie (beat) lance une fusée qui explose en particules
 * colorées avec gravité et traînées.
 */
#include "plugin.h"

#include <windows.h>
#include <math.h>
#include <stdlib.h>

#define MAX_PARTS   640          /* particules max */
#define PAL_COLORS  360          /* une couleur par degré de teinte */

typedef struct {
    float x, y, px, py;          /* position + position précédente (traînée) */
    float vx, vy;                /* vélocité (px/frame) */
    float life, maxlife;         /* durée de vie */
    int   color;                 /* index palette */
    int   size;                  /* 2 ou 1 (fin de vie) */
} particle_t;

static const mp_host_api* g_host = NULL;

static particle_t g_parts[MAX_PARTS];
static int g_nparts = 0;
static CRITICAL_SECTION g_lock;

static int    g_vw = 400, g_vh = 100;      /* dimensions connues du render */
static float  g_energy = 0.0f;             /* RMS du dernier bloc */
static float  g_energy_avg = 0.0f;         /* moyenne mobile (seuil adaptatif) */
static int    g_cooldown = 0;              /* blocs avant la prochaine explosion */

static HBRUSH g_palette[PAL_COLORS];
static HBRUSH g_bg_bands[10];
static HBRUSH g_star_br;
static struct { int x, y; BYTE v; } g_stars[70];

/* ------------------------------------------------------------------ */
/* Identification                                                      */
/* ------------------------------------------------------------------ */
static const char* name(void)        { return "Fireworks"; }
static const char* version(void)     { return "0.1.0"; }
static const char* description(void) { return "Feu d'artifice synchronisé sur la musique"; }
static unsigned type(void)           { return MP_PLUGIN_VISUAL; }

static void hsv_to_rgb_fw(float h, float s, float v, BYTE* r, BYTE* g, BYTE* b)
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
/* Cycle de vie                                                        */
/* ------------------------------------------------------------------ */
static int init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_host = host;

    for (int i = 0; i < PAL_COLORS; i++) {
        BYTE r, g, b;
        hsv_to_rgb_fw((float)i, 1.0f, 1.0f, &r, &g, &b);
        g_palette[i] = CreateSolidBrush(RGB(r, g, b));
    }
    /* ciel nocturne : dégradé très sombre */
    for (int i = 0; i < 10; i++) {
        float t = (float)i / 9.0f;
        g_bg_bands[i] = CreateSolidBrush(RGB(
            (BYTE)(3 + 6 * t), (BYTE)(3 + 7 * t), (BYTE)(10 + 14 * t)));
    }
    g_star_br = CreateSolidBrush(RGB(200, 210, 235));

    /* étoiles statiques */
    srand(42);
    for (int i = 0; i < 70; i++) {
        g_stars[i].x = rand() % 1000;      /* permille de la largeur */
        g_stars[i].y = rand() % 1000;
        g_stars[i].v = (BYTE)(60 + (rand() % 160));
    }

    InitializeCriticalSection(&g_lock);
    g_nparts = 0;
    g_energy = g_energy_avg = 0.0f;
    g_cooldown = 0;

    if (g_host && g_host->log)
        g_host->log("Fireworks: init OK (beat-triggered explosions)");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    DeleteObject(g_star_br);
    for (int i = 0; i < 10; i++) DeleteObject(g_bg_bands[i]);
    for (int i = 0; i < PAL_COLORS; i++) DeleteObject(g_palette[i]);
    DeleteCriticalSection(&g_lock);
    if (g_host && g_host->log) g_host->log("Fireworks: unloaded");
    g_host = NULL;
}

/* ------------------------------------------------------------------ */
/* Explosion : n particules depuis un point, cône vers le haut         */
/* ------------------------------------------------------------------ */
static void spawn_explosion(void)
{
    int vw = g_vw, vh = g_vh;
    if (vw < 50 || vh < 30) return;

    float cx = (0.12f + 0.76f * ((float)rand() / RAND_MAX)) * vw;
    float cy = (0.15f + 0.35f * ((float)rand() / RAND_MAX)) * vh;
    float base_hue = (float)(rand() % 360);
    int n = 70 + rand() % 80;
    if (n > MAX_PARTS) n = MAX_PARTS;

    for (int i = 0; i < n && g_nparts < MAX_PARTS; i++) {
        particle_t* p = &g_parts[g_nparts++];
        float ang = (float)(rand() % 628) / 100.0f;   /* 0..2pi */
        float sp = (0.5f + 1.6f * ((float)rand() / RAND_MAX));
        p->x = cx; p->y = cy; p->px = cx; p->py = cy;
        p->vx = cosf(ang) * sp * (vw / 400.0f) * 3.0f;
        p->vy = sinf(ang) * sp * (vh / 100.0f) * 2.2f;
        p->life = 0.9f + 1.6f * ((float)rand() / RAND_MAX);
        p->maxlife = p->life;
        p->color = ((int)base_hue + rand() % 50 - 25 + PAL_COLORS) % PAL_COLORS;
        p->size = 2;
    }
}

/* ------------------------------------------------------------------ */
/* Flux audio : détection de beat (énergie > seuil adaptatif)          */
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
    float energy = (float)sqrt(sum / (frames * channels));

    EnterCriticalSection(&g_lock);
    g_energy = energy;
    g_energy_avg = 0.92f * g_energy_avg + 0.08f * energy;
    if (g_cooldown > 0) g_cooldown--;
    if (energy > g_energy_avg * 1.35f && g_cooldown == 0) {
        spawn_explosion();
        g_cooldown = 24;   /* ~240 ms entre deux explosions */
    }
    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------------------------ */
/* Rendu : ciel + étoiles + particules                                 */
/* ------------------------------------------------------------------ */
static void render(mp_plugin* self, void* hdc_v, int width, int height)
{
    (void)self;
    if (width <= 0 || height <= 0) return;
    HDC hdc = (HDC)hdc_v;

    g_vw = width; g_vh = height;

    /* ciel nocturne */
    int band_h = (height + 9) / 10;
    for (int i = 0; i < 10; i++) {
        RECT r = { 0, i * band_h, width, (i + 1) * band_h };
        if (r.top >= height) break;
        if (r.bottom > height) r.bottom = height;
        FillRect(hdc, &r, g_bg_bands[i]);
    }
    /* étoiles */
    for (int i = 0; i < 70; i++) {
        int sx = g_stars[i].x * width / 1000;
        int sy = g_stars[i].y * height / 1000;
        RECT s = { sx, sy, sx + 1, sy + 1 };
        FillRect(hdc, &s, g_star_br);
    }

    /* mise à jour + dessin des particules (dt = 1/30 s) */
    EnterCriticalSection(&g_lock);
    int kept = 0;
    for (int i = 0; i < g_nparts; i++) {
        particle_t* p = &g_parts[i];
        p->life -= 1.0f / 30.0f;
        if (p->life <= 0.0f) continue;

        p->px = p->x; p->py = p->y;
        p->vy += 0.14f;                      /* gravité */
        p->x += p->vx;
        p->y += p->vy;
        if (p->life < p->maxlife * 0.3f) p->size = 1;

        HBRUSH br = g_palette[p->color % PAL_COLORS];
        /* traînée : segment interpolé (3 points) */
        int nseg = 3;
        for (int s = 1; s <= nseg; s++) {
            int tx = (int)(p->px + (p->x - p->px) * s / nseg);
            int ty = (int)(p->py + (p->y - p->py) * s / nseg);
            RECT tr = { tx - 1, ty - 1, tx + 1, ty + 1 };
            FillRect(hdc, &tr, br);
        }
        /* point principal */
        RECT pt = { (int)p->x - p->size, (int)p->y - p->size,
                    (int)p->x + p->size, (int)p->y + p->size };
        FillRect(hdc, &pt, br);

        if (kept != i) g_parts[kept] = g_parts[i];
        kept++;
    }
    g_nparts = kept;
    LeaveCriticalSection(&g_lock);
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    name, version, description, type,
    init, destroy,
    NULL,           /* pas d'effet audio */
    audio_frames,   /* flux audio (beats) */
    render,         /* rendu visuel */
    NULL            /* pas de skin */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
