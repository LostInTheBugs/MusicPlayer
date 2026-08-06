/*
 * MusicPlayer — skin rétro années 60.
 * Pastel crème & orange, style sixties.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Retro 60s"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{ return "Pastel cream & orange, sixties style"; }
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
        0xF5F0E6,  /* bg : crème */
        0x4A3F35,  /* text : brun foncé */
        0xEDE4D0,  /* ctrl_bar */
        0xD8C9A8,  /* ctrl_sep */
        0xE07B39,  /* accent : orange vif */
        0xC0392B,  /* accent2 : rouge */
        0xE67E22,  /* accent3 : orange doux */
        0xA8997F,  /* neutral */
        0xE0D5BC,  /* track */
        0xB0A188,  /* mark */
        0xFFFFFF,  /* knob */
        0xEDE4D0,  /* prog_bg */
        0xD8C9A8   /* prog_border */
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
