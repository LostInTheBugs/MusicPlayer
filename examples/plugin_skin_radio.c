/*
 * MusicPlayer — skin radio vintage.
 * Bois & doré : une vieille radio à lampes.
 */
#include <windows.h>
#include "plugin.h"

static const mp_host_api* g_h = NULL;

static const char* pl_name(void)    { return "Vintage radio"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Wood & gold, vintage tube radio"; }
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
        0x8B5A2B,  /* bg : bois */
        0xFFF3D6,  /* text : ivoire */
        0x7A4A20,  /* ctrl_bar : bois foncé */
        0x5C3615,  /* ctrl_sep */
        0xD4A017,  /* accent : doré */
        0x8B0000,  /* accent2 : rouge sombre */
        0xFFD700,  /* accent3 : or vif */
        0xA07030,  /* neutral */
        0x6B4319,  /* track */
        0xD4A017,  /* mark */
        0xFFE9B0,  /* knob */
        0x7A4A20,  /* prog_bg */
        0xD4A017   /* prog_border */
    };
    if (g_h && g_h->skin_set_colors) g_h->skin_set_colors(&c);
    /* texture de la radio (à côté de la DLL) */
    if (g_h && g_h->skin_set_bg) {
        wchar_t dir[MAX_PATH];
        wcsncpy(dir, self->path, MAX_PATH - 1);
        dir[MAX_PATH - 1] = 0;
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) slash[1] = 0;
        wcscat(dir, L"radio_bg.png");
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
