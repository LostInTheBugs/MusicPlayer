#ifndef MP_PLUGIN_LOADER_H
#define MP_PLUGIN_LOADER_H

#include <windows.h>
#include "plugin.h"

typedef struct mp_plugin {
    HMODULE dll;                    /* handle de la DLL chargée */
    const mp_plugin_api* api;       /* API du plugin */
    int enabled;                    /* activé dans le menu ? */
    wchar_t path[MAX_PATH];         /* chemin complet de la DLL */
} mp_plugin;

/* Analyse le répertoire <dir> (UTF-16) et charge chaque DLL exportant
 * mp_plugin_entry. Les plugins déjà chargés sont déchargés d'abord. */
void mp_plugins_scan(const wchar_t* dir, const mp_host_api* host);

/* Nombre de plugins chargés. */
int mp_plugins_count(void);

/* Accès au plugin i (0 <= i < count). */
mp_plugin* mp_plugins_get(int i);

/* Active/désactive un plugin. */
void mp_plugins_set_enabled(int i, int on);

/* Applique les peaux des plugins de type SKIN actifs (appelé par l'UI). */
void mp_plugins_apply_skins(void* hwnd);

/* Applique les effets audio des plugins actifs sur un bloc PCM
 * (interleavé f32 stéréo). Appelé par le moteur audio. */
void mp_plugins_audio_process(float* samples, unsigned frames,
                              unsigned channels, unsigned sample_rate);

/* Décharge tous les plugins (appelé à la fermeture). */
void mp_plugins_shutdown(void);

#endif /* MP_PLUGIN_LOADER_H */
