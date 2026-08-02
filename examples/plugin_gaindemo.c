/*
 * Plugin de démonstration MusicPlayer — "GainDemo"
 * Type : effet audio. Atténue le signal de moitié et journalise son
 * activité via l'API hôte. Sert d'exemple de référence pour écrire
 * de nouveaux plugins (voir plugins/README.md et src/plugin.h).
 */
#include "plugin.h"

static const mp_host_api* g_host = NULL;

static const char* name(void)        { return "GainDemo"; }
static const char* version(void)     { return "0.1.0"; }
static const char* description(void) { return "Effet démo : atténue le son de moitié"; }

static unsigned type(void) { return MP_PLUGIN_AUDIO_EFFECT; }

static int init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_host = host;
    if (g_host && g_host->log)
        g_host->log("GainDemo : init OK");
    return 0;
}

static void destroy(mp_plugin* self)
{
    (void)self;
    if (g_host && g_host->log) g_host->log("GainDemo : déchargé");
    g_host = NULL;
}

static void process(mp_plugin* self, float* samples, unsigned frames,
                    unsigned channels, unsigned sample_rate)
{
    (void)self; (void)sample_rate;
    unsigned n = frames * channels;
    for (unsigned i = 0; i < n; i++)
        samples[i] *= 0.5f;
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    name, version, description, type,
    init, destroy,
    process,      /* effet audio */
    NULL,         /* pas de visuel */
    NULL          /* pas de skin */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
