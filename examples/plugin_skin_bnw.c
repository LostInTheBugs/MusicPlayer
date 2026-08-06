/*
 * MusicPlayer — skin noir & blanc.
 * Monochrome élégant.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Noir & Blanc"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{ return "Elegant monochrome black & white"; }
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
        0xFFFFFF,  /* bg : blanc */
        0x000000,  /* text : noir */
        0xF2F2F2,  /* ctrl_bar */
        0xC8C8C8,  /* ctrl_sep */
        0x000000,  /* accent : noir */
        0x444444,  /* accent2 : gris foncé */
        0x888888,  /* accent3 : gris */
        0xB8B8B8,  /* neutral */
        0xE4E4E4,  /* track */
        0x9C9C9C,  /* mark */
        0xFFFFFF,  /* knob */
        0xEFEFEF,  /* prog_bg */
        0x777777   /* prog_border */
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
