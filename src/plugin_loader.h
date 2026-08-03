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

/* Diffuse un événement aux plugins SERVICE actifs (MP_SERVICE_*). */
void mp_plugins_service(int event, void* data);

/* Titre (métadonnées) d'un fichier : premier plugin SERVICE actif. */
const char* mp_plugins_get_title(const char* path);

/* Métadonnée d'un fichier ("title", "artist", "album"…) : premier
 * plugin SERVICE actif qui en fournit une ; NULL sinon. */
const char* mp_plugins_get_metadata(const char* path, const char* field);

/* Applique les effets audio des plugins actifs sur un bloc PCM
 * (interleavé f32 stéréo). Appelé par le moteur audio. */
void mp_plugins_audio_process(float* samples, unsigned frames,
                              unsigned channels, unsigned sample_rate);

/* Diffuse un bloc PCM (lecture seule) aux plugins visuels actifs
 * (hook audio_frames). Appelé par le moteur audio après les effets. */
void mp_plugins_audio_frames(const float* samples, unsigned frames,
                             unsigned channels, unsigned sample_rate);

/* Y a-t-il au moins un plugin visuel actif ? (l'UI s'en sert pour
 * basculer en mode rendu + timer rapide). */
int mp_plugins_has_visual(void);

/* Appelle le hook render() des plugins visuels actifs (thread UI). */
void mp_plugins_visual_render(void* hdc, int width, int height);

/* Décharge tous les plugins (appelé à la fermeture). */
void mp_plugins_shutdown(void);

#endif /* MP_PLUGIN_LOADER_H */
