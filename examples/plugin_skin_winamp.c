/*
 * MusicPlayer — skin Winamp.
 * Vert acide sur gris foncé, style Winamp.
 */
#include <windows.h>
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
    (void)hwnd;
    static const mp_skin_colors c = {
        0x3C3F41,  /* bg : gris acier */
        0x00E000,  /* text : vert acide */
        0x4A4D50,  /* ctrl_bar : gris */
        0x2B2D2F,  /* ctrl_sep */
        0x00E000,  /* accent : vert acide */
        0xE24238,  /* accent2 : rouge */
        0xE8A33D,  /* accent3 : orange */
        0x7A7D80,  /* neutral */
        0x2F3133,  /* track */
        0x00E000,  /* mark */
        0x9A9DA0,  /* knob */
        0x1E1F21,  /* prog_bg */
        0x00E000   /* prog_border */
    };
    if (g_h && g_h->skin_set_colors) g_h->skin_set_colors(&c);
    /* texture façon Winamp (à côté de la DLL) */
    if (g_h && g_h->skin_set_bg) {
        wchar_t dir[MAX_PATH];
        wcsncpy(dir, self->path, MAX_PATH - 1);
        dir[MAX_PATH - 1] = 0;
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) slash[1] = 0;
        wcscat(dir, L"winamp_bg.png");
        char u8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, dir, -1, u8, sizeof(u8), NULL, NULL);
        g_h->skin_set_bg(u8);
    }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    pl_init, NULL,
    NULL, NULL, NULL, pl_apply_skin,
    NULL, NULL
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
