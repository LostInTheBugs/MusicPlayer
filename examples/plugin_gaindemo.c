/*
 * MusicPlayer — plugin : booster de volume sonore.
 * Type : effet audio. Amplifie le signal (+25 %) avec écrêtage.
 * Activation : Settings ▸ Plugins.
 */
#include "plugin.h"

static const mp_host_api* g_host = NULL;

static const char* pl_name(void)    { return "Volume booster"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{ return "Boosts the sound level (audio effect, +25 %)"; }
static unsigned pl_type(void) { return MP_PLUGIN_AUDIO_EFFECT; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_host = host;
    if (g_host && g_host->log) g_host->log("Volume booster: init OK");
    return 0;
}

static void pl_destroy(mp_plugin* self)
{
    (void)self;
    if (g_host && g_host->log) g_host->log("Volume booster: unloaded");
    g_host = NULL;
}

static void pl_process(mp_plugin* self, float* samples, unsigned frames,
                       unsigned channels, unsigned sample_rate)
{
    (void)self; (void)sample_rate;
    unsigned n = frames * channels;
    for (unsigned i = 0; i < n; i++) {
        float v = samples[i] * 1.25f;
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        samples[i] = v;
    }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    pl_init, pl_destroy,
    pl_process,       /* effet audio */
    NULL,             /* pas de flux audio (visuel) */
    NULL,             /* pas de rendu visuel */
    NULL,             /* pas de skin */
    NULL, NULL        /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
