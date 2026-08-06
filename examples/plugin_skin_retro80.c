/*
 * MusicPlayer — skin rétro années 80.
 * Néon synthwave : rose, cyan, violet sur fond nuit.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Retro 80s"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{ return "Neon synthwave pink & cyan, eighties style"; }
static unsigned pl_type(void) { return MP_PLUGIN_SKIN; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_h = host;
    return 0;
}

static void pl_apply_skin(mp_plugin* self, void* hwnd)
{
    (void)self; (void)hwnd;
    static const mp_skin_colors c = {
        0x1A1033,  /* bg : nuit violette */
        0xF0E6FF,  /* text : lavande clair */
        0x241545,  /* ctrl_bar */
        0x3A2568,  /* ctrl_sep */
        0xFF2E88,  /* accent : rose néon */
        0xFFD700,  /* accent2 : jaune néon */
        0x00E5FF,  /* accent3 : cyan néon */
        0x5A4A85,  /* neutral */
        0x3A2568,  /* track */
        0x8A7AC0,  /* mark */
        0xFFFFFF,  /* knob */
        0x241545,  /* prog_bg */
        0xFF2E88   /* prog_border */
    };
    if (g_h && g_h->skin_set_colors) g_h->skin_set_colors(&c);
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    pl_init, NULL,
    NULL, NULL, NULL, pl_apply_skin,
    NULL, NULL
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
