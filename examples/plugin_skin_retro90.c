/*
 * MusicPlayer — skin rétro années 90.
 * Gris & teal : le look Windows 95.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Retro 90s"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Grey & teal, Windows 95 style"; }
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
        0xC0C0C0,  /* bg : gris argent */
        0x1A1A1A,  /* text : noir */
        0xD4D0C8,  /* ctrl_bar */
        0x808080,  /* ctrl_sep */
        0x008080,  /* accent : teal */
        0xC00000,  /* accent2 : rouge foncé */
        0x008080,  /* accent3 */
        0x808080,  /* neutral */
        0xA0A0A0,  /* track */
        0x606060,  /* mark */
        0xFFFFFF,  /* knob */
        0xD4D0C8,  /* prog_bg */
        0x808080   /* prog_border */
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
