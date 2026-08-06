/*
 * MusicPlayer — skin épuré (minimal).
 * Blanc, noir et un trait d'accent : le look moderne minimaliste.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Clean"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{ return "Minimal white & black, modern clean look"; }
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
        0xFAFAFA,  /* bg : blanc */
        0x111111,  /* text : noir */
        0xFFFFFF,  /* ctrl_bar */
        0xE0E0E0,  /* ctrl_sep */
        0x111111,  /* accent : noir */
        0xE53935,  /* accent2 : rouge discret */
        0x111111,  /* accent3 */
        0x9E9E9E,  /* neutral */
        0xEEEEEE,  /* track */
        0x9E9E9E,  /* mark */
        0xFFFFFF,  /* knob */
        0xF0F0F0,  /* prog_bg */
        0xCCCCCC   /* prog_border */
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
