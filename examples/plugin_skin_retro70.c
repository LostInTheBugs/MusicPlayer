/*
 * MusicPlayer — skin rétro années 70.
 * Brun, avocat & orange : l'esprit seventies.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Retro 70s"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{ return "Brown, avocado & orange, seventies vibes"; }
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
        0xF4E9D7,  /* bg : ivoire chaud */
        0x5B4A32,  /* text : brun */
        0xE8D5B5,  /* ctrl_bar */
        0xC9B088,  /* ctrl_sep */
        0x8A9A5B,  /* accent : vert avocat */
        0xC1440E,  /* accent2 : orange brûlé */
        0xE08A1E,  /* accent3 : orange */
        0xA99572,  /* neutral */
        0xDDC9A6,  /* track */
        0xB29B72,  /* mark */
        0xFFFBF2,  /* knob */
        0xE8D5B5,  /* prog_bg */
        0xC9B088   /* prog_border */
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
