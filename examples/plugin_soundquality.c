/*
 * Sound Quality — plugin AUDIO_EFFECT
 * ===================================
 * Amélioration douce du rendu sonore :
 *   - filtrage des infra-basses (coupe < 45 Hz, biquad high-pass)
 *   - basse boost léger (+2 dB vers 110 Hz)
 *   - présence (+1,5 dB vers 3 kHz)
 *   - limiteur doux (tanh) : pas d'écrêtage brutal, son "chaud"
 *
 * Activable dans Plugins ▸ Effets ▸ Sound Quality.
 */
#include <math.h>
#include <stdio.h>

#include "../src/plugin.h"

#define SR 44100

/* Biquad (formulaire RBJ) */
typedef struct {
    double b0, b1, b2, a1, a2;
    double x1, x2, y1, y2;
} biquad_t;

static void bq_lowshelf(biquad_t* q, double f, double gain_db, double sr)
{
    double A = pow(10.0, gain_db / 40.0);
    double w = 2.0 * 3.14159265358979 * f / sr;
    double cw = cos(w), sw = sin(w);
    double S = 1.0, beta = sqrt(A) / 0.7071;
    double b = 2.0 * sqrt(A) * beta;
    double a0 = (A + 1.0) + (A - 1.0) * cw + b * sw;
    q->b0 = A * ((A + 1.0) - (A - 1.0) * cw + b * sw) / a0;
    q->b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw) / a0;
    q->b2 = A * ((A + 1.0) - (A - 1.0) * cw - b * sw) / a0;
    q->a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw) / a0;
    q->a2 = ((A + 1.0) + (A - 1.0) * cw - b * sw) / a0;
    q->x1 = q->x2 = q->y1 = q->y2 = 0.0;
}

static void bq_highshelf(biquad_t* q, double f, double gain_db, double sr)
{
    double A = pow(10.0, gain_db / 40.0);
    double w = 2.0 * 3.14159265358979 * f / sr;
    double cw = cos(w), sw = sin(w);
    double beta = sqrt(A) / 0.7071;
    double b = 2.0 * sqrt(A) * beta;
    double a0 = (A + 1.0) - (A - 1.0) * cw + b * sw;
    q->b0 = A * ((A + 1.0) + (A - 1.0) * cw + b * sw) / a0;
    q->b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0;
    q->b2 = A * ((A + 1.0) + (A - 1.0) * cw - b * sw) / a0;
    q->a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw) / a0;
    q->a2 = ((A + 1.0) - (A - 1.0) * cw - b * sw) / a0;
    q->x1 = q->x2 = q->y1 = q->y2 = 0.0;
}

static void bq_peaking(biquad_t* q, double f, double gain_db, double qv,
                       double sr)
{
    double A = pow(10.0, gain_db / 40.0);
    double w = 2.0 * 3.14159265358979 * f / sr;
    double cw = cos(w), sw = sin(w);
    double alpha = sw / (2.0 * qv);
    double a0 = 1.0 + alpha / A;
    q->b0 = (1.0 + alpha * A) / a0;
    q->b1 = (-2.0 * cw) / a0;
    q->b2 = (1.0 - alpha * A) / a0;
    q->a1 = (-2.0 * cw) / a0;
    q->a2 = (1.0 - alpha / A) / a0;
    q->x1 = q->x2 = q->y1 = q->y2 = 0.0;
}

static void bq_highpass(biquad_t* q, double f, double qv, double sr)
{
    double w = 2.0 * 3.14159265358979 * f / sr;
    double cw = cos(w), sw = sin(w);
    double alpha = sw / (2.0 * qv);
    double a0 = 1.0 + alpha;
    q->b0 = (1.0 + cw) / 2.0 / a0;
    q->b1 = -(1.0 + cw) / a0;
    q->b2 = (1.0 + cw) / 2.0 / a0;
    q->a1 = (-2.0 * cw) / a0;
    q->a2 = (1.0 - alpha) / a0;
    q->x1 = q->x2 = q->y1 = q->y2 = 0.0;
}

static double bq_run(biquad_t* q, double x)
{
    double y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 -
               q->a1 * q->y1 - q->a2 * q->y2;
    q->x2 = q->x1;
    q->x1 = x;
    q->y2 = q->y1;
    q->y1 = y;
    return y;
}

static biquad_t g_hp, g_bass, g_mid, g_treb;
static int g_inited = 0;
static int g_last_rate = 0;

static void ensure_filters(int rate)
{
    if (rate != g_last_rate) {
        g_last_rate = rate;
        double sr = rate > 0 ? (double)rate : SR;
        bq_highpass(&g_hp, 45.0, 0.7071, sr);
        bq_lowshelf(&g_bass, 110.0, 2.0, sr);
        bq_peaking(&g_mid, 3000.0, 1.5, 0.9, sr);
        bq_highshelf(&g_treb, 8000.0, 1.0, sr);
        g_inited = 1;
    }
}

/* ------------------------------------------------------------------ */
static const char* pl_name(void) { return "Sound Quality"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Warm, punchy sound: sub-bass filter, bass boost, presence, soft limiter"; }
static unsigned pl_type(void) { return MP_PLUGIN_AUDIO_EFFECT; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    (void)self; (void)host;
    return 0;
}

static void pl_destroy(mp_plugin* self) { (void)self; }

static void pl_process(mp_plugin* self, float* samples, unsigned frames,
                       unsigned channels, unsigned sample_rate)
{
    (void)self;
    ensure_filters((int)sample_rate);
    if (!g_inited) return;
    unsigned n = frames * channels;
    if (channels == 2) {
        for (unsigned i = 0; i < n; i += 2) {
            double l = bq_run(&g_hp, samples[i]);
            double r = bq_run(&g_hp, samples[i + 1]);
            l = bq_run(&g_bass, l);
            r = bq_run(&g_bass, r);
            l = bq_run(&g_mid, l);
            r = bq_run(&g_mid, r);
            l = bq_run(&g_treb, l);
            r = bq_run(&g_treb, r);
            /* limiteur doux */
            l = tanh(l * 1.15);
            r = tanh(r * 1.15);
            samples[i] = (float)l;
            samples[i + 1] = (float)r;
        }
    } else {
        for (unsigned i = 0; i < n; i++) {
            double v = bq_run(&g_hp, samples[i]);
            v = bq_run(&g_bass, v);
            v = bq_run(&g_mid, v);
            v = bq_run(&g_treb, v);
            samples[i] = (float)tanh(v * 1.15);
        }
    }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    pl_init, pl_destroy,
    pl_process, NULL, NULL, NULL,  /* process, audio_frames, render, skin */
    NULL, NULL                     /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
