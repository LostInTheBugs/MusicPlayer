/*
 * MusicPlayer — API plugins, version 1
 * =====================================
 * Un plugin est une DLL Windows qui exporte la fonction :
 *
 *     const mp_plugin_api* mp_plugin_entry(void);
 *
 * (nom d'export : "mp_plugin_entry", défini par MP_PLUGIN_ENTRY)
 *
 * Types de plugins (combinables) :
 *   - MP_PLUGIN_SKIN         : personnalise l'apparence de la fenêtre
 *   - MP_PLUGIN_AUDIO_EFFECT : traite le flux audio (effets, égaliseur...)
 *   - MP_PLUGIN_VISUAL       : rendu visuel (visualiseur, spectre...)
 *
 * Les plugins sont chargés depuis le sous-répertoire "plugins/" situé à côté
 * de l'exécutable. Voir plugins/README.md pour un exemple complet.
 */
#ifndef MP_PLUGIN_H
#define MP_PLUGIN_H

#include <stddef.h>

#define MP_PLUGIN_API_VERSION 1

typedef enum {
    MP_PLUGIN_SKIN         = 1 << 0,
    MP_PLUGIN_AUDIO_EFFECT = 1 << 1,
    MP_PLUGIN_VISUAL       = 1 << 2
} mp_plugin_type;

typedef struct mp_plugin mp_plugin;

/*
 * API offerte par l'hôte (MusicPlayer) aux plugins.
 * Tous les appels sont sûrs depuis le thread du plugin.
 */
typedef struct mp_host_api {
    int api_version;

    /* Écrit une ligne dans le journal de l'application (musicplayer.log). */
    void (*log)(const char* msg);

    /* État du lecteur : voir mp_state dans player.h */
    int (*get_state)(void);
    double (*get_position)(void);   /* secondes dans le fichier */
    double (*get_duration)(void);   /* durée totale en secondes */
    float  (*get_volume)(void);     /* 0.0 .. 1.0 */
    float  (*get_speed)(void);      /* 0.5 .. 2.0 */
    const char* (*get_file_name)(void);
} mp_host_api;

/*
 * API d'un plugin. Toutes les fonctions sont optionnelles sauf name().
 * Les hooks sont appelés uniquement si le type correspondant est déclaré
 * par type() et si le plugin est activé dans le menu Plugins.
 */
typedef struct mp_plugin_api {
    int api_version;                /* doit valoir MP_PLUGIN_API_VERSION */

    /* --- identification (obligatoire) --- */
    const char* (*name)(void);
    const char* (*version)(void);
    const char* (*description)(void);
    unsigned (*type)(void);         /* combinaison de mp_plugin_type */

    /* --- cycle de vie --- */
    int  (*init)(mp_plugin* self, const mp_host_api* host);
    void (*destroy)(mp_plugin* self);

    /* --- hook AUDIO_EFFECT ---
     * Reçoit chaque bloc PCM interleavé (float 32 bits, canaux stéréo)
     * juste avant sa sortie vers le périphérique. Modifier le buffer
     * sur place. sample_rate = taux réel du périphérique. */
    void (*process)(mp_plugin* self, float* samples, unsigned frames,
                    unsigned channels, unsigned sample_rate);

    /* --- hook VISUAL ---
     * hdc = contexte de dessin de la zone d'affichage du lecteur.
     * Appelé périodiquement (thread UI) quand la lecture est active. */
    void (*render)(mp_plugin* self, void* hdc, int width, int height);

    /* --- hook SKIN ---
     * hwnd = fenêtre principale. Le plugin peut modifier les couleurs,
     * la police, les contrôles... Appelé une fois après le chargement. */
    void (*apply_skin)(mp_plugin* self, void* hwnd);
} mp_plugin_api;

#define MP_PLUGIN_ENTRY "mp_plugin_entry"
typedef const mp_plugin_api* (*mp_plugin_entry_fn)(void);

#endif /* MP_PLUGIN_H */
