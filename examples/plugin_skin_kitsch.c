/*
 * MusicPlayer — skin kitsch.
 * Rose bonbon, violet & doré : assumé et décalé.
 */
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Kitsch"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Candy pink, purple & gold — gloriously kitsch"; }
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
        0xFFE4EC,  /* bg : rose bonbon pâle */
        0x8E2A63,  /* text : fuchsia foncé */
        0xFFD1E0,  /* ctrl_bar */
        0xF0A8C0,  /* ctrl_sep */
        0xFF4FA3,  /* accent : rose vif */
        0xB44BC8,  /* accent2 : violet */
        0xFFC800,  /* accent3 : doré */
        0xD98AAF,  /* neutral */
        0xF5B8CC,  /* track */
        0xC77BA0,  /* mark */
        0xFFFFFF,  /* knob */
        0xFFD1E0,  /* prog_bg */
        0xFF4FA3   /* prog_border */
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
