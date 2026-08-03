/*
 * MusicPlayer — skin Winamp.
 * Vert acide sur fond gris foncé, façon Winamp 2.x.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Winamp"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Acid green on dark grey, Winamp style"; }
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
        0x2A2A2A,  /* bg : gris foncé */
        0xC8F06A,  /* text : vert acide */
        0x3A3A3A,  /* ctrl_bar */
        0x1A1A1A,  /* ctrl_sep */
        0x79D700,  /* accent : vert winamp */
        0xD93B3B,  /* accent2 : rouge */
        0xFFC800,  /* accent3 : jaune */
        0x606060,  /* neutral */
        0x4A4A4A,  /* track */
        0x79D700,  /* mark */
        0xD0FF9A,  /* knob */
        0x3A3A3A,  /* prog_bg */
        0x79D700   /* prog_border */
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
