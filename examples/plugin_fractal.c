/*
 * Fractale — plugin VISUAL
 * =========================
 * Une vraie fractale de Mandelbrot (et Julia) qui **zoome au rythme de
 * la musique** : l'énergie des basses est analysée sur le flux audio ;
 * chaque battement pousse le zoom vers l'intérieur de la fractale, qui
 * redescend doucement entre deux battements. Le point focal est la
 * "Sea Horse Valley" de Mandelbrot (-0.7436 + 0.1318i) ; le rendu
 * alterne Mandelbrot / Julia toutes les 15 secondes.
 *
 * Rendu en résolution réduite (moitié) agrandie par StretchDIBits pour
 * rester fluide même à fort zoom.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../src/plugin.h"

static const mp_host_api* g_h = NULL;

/* point focal du zoom : la "seahorse valley" classique (-0.74529+0.11308i),
 * un point de la FRONTIÈRE de l'ensemble : le zoom infini y déroule les
 * spirales de chevaux marins */
static const double g_cx = -0.74529;
static const double g_cy = 0.11308;
static const double g_jx = -0.4, g_jy = 0.6;   /* plan de Julia */

#define BASE_ZOOM  200.0           /* zoom de départ (déjà sur le bord) */
#define ZOOM_LVL_MAX (30 * 1024)   /* 2^30 : limite de précision du double ;
                                      au-delà, tous les pixels ont la même
                                      coordonnée (image figée) */
static volatile LONG g_zoom_lvl = 0;   /* log2(zoom/BASE) × 1024 */
static double g_energy = 0.0;      /* énergie RMS lissée */
static double g_avg = 0.0;         /* niveau moyen (seuil de battement) */
static double g_beat = 0.0;        /* amplitude du battement courant */
static double g_time = 0.0;        /* temps de rendu (s) */

static void hsv2rgb(double h, double s, double v,
                    unsigned char* r, unsigned char* g, unsigned char* b)
{
    double c = v * s;
    double x = c * (1.0 - fabs(fmod(h * 6.0, 2.0) - 1.0));
    double m = v - c;
    double rr, gg, bb;
    int i = (int)(h * 6.0) % 6;
    switch (i) {
    case 0: rr = c; gg = x; bb = 0; break;
    case 1: rr = x; gg = c; bb = 0; break;
    case 2: rr = 0; gg = c; bb = x; break;
    case 3: rr = 0; gg = x; bb = c; break;
    case 4: rr = x; gg = 0; bb = c; break;
    default: rr = c; gg = 0; bb = x; break;
    }
    *r = (unsigned char)((rr + m) * 255.0);
    *g = (unsigned char)((gg + m) * 255.0);
    *b = (unsigned char)((bb + m) * 255.0);
}

/* ------------------------------------------------------------------ */
static const char* pl_name(void) { return "Fractale"; }
static const char* pl_version(void) { return "2026.08.034-c1.002"; }
static const char* pl_description(void)
{ return "Mandelbrot / Julia fractal zooming on the beat of the music"; }
static unsigned pl_type(void) { return MP_PLUGIN_VISUAL; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_h = host;
    g_zoom_lvl = 0;
    g_energy = 0.0;
    g_avg = 0.0;
    g_beat = 0.0;
    g_time = 0.0;
    return 0;
}

static void pl_destroy(mp_plugin* self) { (void)self; }

/* Analyse du flux : l'énergie (RMS) pilote le zoom */
static void pl_audio(mp_plugin* self, const float* samples, unsigned frames,
                     unsigned channels, unsigned sample_rate)
{
    (void)self;
    (void)sample_rate;
    if (!frames || !channels) return;
    double e = 0.0;
    unsigned n = frames * channels;
    for (unsigned i = 0; i < n; i += channels) {
        float v = samples[i];
        e += (double)v * v;
    }
    e = sqrt(e / (double)frames);
    g_energy = g_energy * 0.65 + e * 0.35;
    g_avg = g_avg * 0.9990 + g_energy * 0.0010;
    double delta = g_energy - g_avg * 1.25;
    if (delta > 0.0) {
        g_beat = g_beat * 0.8 + delta * 0.2;
        LONG add = (LONG)(g_beat * 4.0 * 1024.0);
        if (add > 0) {
            /* au zoom maximal, la boucle recommence (zoom infini) */
            if (g_zoom_lvl + add > ZOOM_LVL_MAX)
                InterlockedExchange(&g_zoom_lvl, 0);
            else
                InterlockedExchangeAdd(&g_zoom_lvl, add);
        }
    } else {
        g_beat *= 0.9;
    }
    /* décroissance lente du zoom entre les battements */
    LONG lvl = g_zoom_lvl;
    if (lvl > 0) {
        lvl -= (LONG)(0.5 * 1024.0 / 43.0);   /* ~0.5 log2 par seconde */
        if (lvl < 0) lvl = 0;
        InterlockedExchange(&g_zoom_lvl, lvl);
    }
}

static void pl_render(mp_plugin* self, void* hdc, int width, int height)
{
    (void)self;
    if (width < 40 || height < 30) return;
    HDC dc = (HDC)hdc;

    /* zoom : log2 lissé (atomique, aucun saut) */
    g_time += 1.0 / 30.0;
    LONG lvl = g_zoom_lvl;
    double zoom = BASE_ZOOM * pow(2.0, (double)lvl / 1024.0);
    int mode = ((int)(g_time / 15.0)) % 2;   /* Mandelbrot ↔ Julia */

    /* rendu en demi-résolution, agrandi ensuite */
    int RW = width / 2, RH = height / 2;
    if (RW < 80) RW = width;
    if (RH < 60) RH = height;
    static unsigned char* buf = NULL;
    static int buf_cap = 0;
    int need = RW * RH * 4;
    if (!buf || buf_cap < need) {
        free(buf);
        buf = (unsigned char*)malloc((size_t)need);
        buf_cap = need;
    }
    if (!buf) return;

    double span = 3.5 / zoom;
    double aspect = (double)RW / (double)RH;
    double x0 = g_cx - span * aspect / 2.0;
    double y0 = g_cy - span / 2.0;
    double dx = span * aspect / (double)RW;
    double dy = span / (double)RH;
    /* plus on zoome, plus il faut d'itérations pour résoudre la frontière */
    int MAXIT = 64 + (int)(log2(zoom) * 9.0);
    if (MAXIT > 400) MAXIT = 400;

    for (int y = 0; y < RH; y++) {
        double cy = y0 + (double)y * dy;
        for (int x = 0; x < RW; x++) {
            double cx = x0 + (double)x * dx;
            double zx, zy, px, py;
            if (mode == 1) {
                /* Julia : le plan est le plan z, la constante est fixe */
                zx = cx;
                zy = cy;
                px = g_jx;
                py = g_jy;
            } else {
                zx = 0.0;
                zy = 0.0;
                px = cx;
                py = cy;
            }
            double zx2 = 0.0, zy2 = 0.0;
            int it = 0;
            while (it < MAXIT && zx2 + zy2 < 4.0) {
                double nzx = zx2 - zy2 + px;
                zy = 2.0 * zx * zy + py;
                zx = nzx;
                zx2 = zx * zx;
                zy2 = zy * zy;
                it++;
            }
            unsigned char* p = buf + ((size_t)y * RW + (size_t)x) * 4;
            if (it >= MAXIT) {
                p[0] = p[1] = p[2] = 0;
            } else {
                /* lissage sûr (pas de NaN quand |z| → 0) */
                double r2 = zx2 + zy2;
                double t;
                if (r2 < 1e-12)
                    t = (double)it;
                else
                    t = (double)it + 1.0 - log2(log2(r2));
                /* teinte stable par position + dérive très lente :
                 * les couleurs ne scintillent pas d'une frame à l'autre */
                double hue = fmod(t * 0.09 +
                                  (double)x / (double)RW * 0.08 +
                                  g_time * 0.008, 1.0);
                double val = 0.3 + 0.7 * (t / (double)MAXIT);
                if (val > 1.0) val = 1.0;
                hsv2rgb(hue, 0.85, val, &p[2], &p[1], &p[0]);
            }
            p[3] = 0;
        }
    }

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = RW;
    bi.bmiHeader.biHeight = -RH;   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, 0, 0, width, height, 0, 0, RW, RH,
                  buf, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    pl_init, pl_destroy,
    NULL, pl_audio, pl_render, NULL,  /* process, audio_frames, render, skin */
    NULL, NULL                        /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
