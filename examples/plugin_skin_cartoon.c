/*
 * MusicPlayer — skin cartoon.
 * Couleurs vives et joyeuses, style dessin animé.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Cartoon"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{ return "Bright & cheerful colors, cartoon style"; }
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
        0xC8F0FF,  /* bg : bleu ciel */
        0x1B3A5C,  /* text : bleu marine */
        0xA8E4FF,  /* ctrl_bar */
        0x6FC4E8,  /* ctrl_sep */
        0x2E9BFF,  /* accent : bleu vif */
        0xFF5252,  /* accent2 : rouge pomme */
        0xFFC400,  /* accent3 : jaune soleil */
        0x5BC8A0,  /* neutral : vert menthe */
        0x8FD8F5,  /* track */
        0x3FA8D8,  /* mark */
        0xFFFFFF,  /* knob */
        0xA8E4FF,  /* prog_bg */
        0x2E9BFF   /* prog_border */
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
