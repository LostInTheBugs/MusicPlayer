/*
 * MusicPlayer — skin années 2000.
 * Bleu glacier & argent, le glossy des années 2000.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "2000s"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{ return "Glacier blue & silver, glossy 2000s look"; }
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
        0xE8F0F8,  /* bg : bleu glacier */
        0x1F3A5F,  /* text : bleu nuit */
        0xDCE8F4,  /* ctrl_bar */
        0xA8C0D8,  /* ctrl_sep */
        0x2E86C1,  /* accent : bleu */
        0xE74C3C,  /* accent2 : rouge */
        0xF39C12,  /* accent3 : or */
        0x7F9DB8,  /* neutral */
        0xC4D6E8,  /* track */
        0x88A8C4,  /* mark */
        0xFFFFFF,  /* knob */
        0xDCE8F4,  /* prog_bg */
        0xA8C0D8   /* prog_border */
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
