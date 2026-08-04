/*
 * MusicPlayer — API plugins, version 2
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
 *   - MP_PLUGIN_SERVICE      : fonctionnalité transverse (serveur web,
 *                              métadonnées des fichiers...)
 *
 * Les plugins sont chargés depuis le sous-répertoire "plugins/" situé à côté
 * de l'exécutable. L'activation se fait dans Settings ▸ Plugins : un plugin
 * désactivé n'apparaît pas dans le menu Plugins.
 */
#ifndef MP_PLUGIN_H
#define MP_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

/* API plugins MusicPlayer — version 2 */
#define MP_PLUGIN_API_VERSION 2

typedef enum {
    MP_PLUGIN_SKIN         = 1 << 0,
    MP_PLUGIN_AUDIO_EFFECT = 1 << 1,
    MP_PLUGIN_VISUAL       = 1 << 2,
    MP_PLUGIN_SERVICE      = 1 << 3   /* web, métadonnées… */
} mp_plugin_type;

typedef struct mp_plugin mp_plugin;

/* Palette de couleurs d'un skin (format COLORREF : 0x00BBGGRR) */
typedef struct mp_skin_colors {
    unsigned long bg;          /* fond de la fenêtre */
    unsigned long text;        /* texte principal */
    unsigned long ctrl_bar;    /* fond de la barre de contrôles */
    unsigned long ctrl_sep;    /* séparateur de la barre */
    unsigned long accent;      /* boutons lecture/suivant + volume 0-100 % */
    unsigned long accent2;     /* bouton stop */
    unsigned long accent3;     /* shuffle actif + volume 100-200 % */
    unsigned long neutral;     /* boutons neutres (plein écran, shuffle off) */
    unsigned long track;       /* fond du curseur de volume */
    unsigned long mark;        /* marque des 100 % */
    unsigned long knob;        /* curseur de volume */
    unsigned long prog_bg;     /* fond de la barre de progression */
    unsigned long prog_border; /* bordure de la progression */
} mp_skin_colors;

/* Événements transverses (hook service) */
#define MP_SERVICE_WEB_APPLY 1   /* reconfigurer le serveur web */
#define MP_SERVICE_CLICK    2    /* clic sur le plugin dans le menu Plugins */

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

    /* --- contrôle de la lecture (services) --- */
    void (*play_pause)(void);
    void (*stop)(void);
    void (*next)(void);
    void (*set_volume)(float v);
    void (*set_speed)(float s);
    void (*set_audio_out)(int mode);   /* 0 = PC, 1 = téléphone, 2 = les deux */
    int  (*get_audio_out)(void);
    void (*shuffle_toggle)(void);
    int  (*get_shuffle)(void);

    /* --- mode DJ Mixing (synchronisé avec la page web) --- */
    int  (*get_dj_mode)(void);
    void (*dj_toggle)(void);

    /* --- playlist --- */
    int  (*plist_count)(void);
    const wchar_t* (*plist_name)(int i);   /* nom de fichier seul */
    int  (*plist_index)(void);
    void (*plist_play)(int i);

    /* --- fenêtre principale --- */
    void* (*main_window)(void);

    /* --- serveur web : configuration --- */
    int  (*web_enabled)(void);
    int  (*web_port)(void);
    int  (*web_audio)(void);
    const char* (*web_ips)(void);          /* "ip1;ip2;..." ; vide = toutes */
    int  (*web_find_free_port)(void);      /* premier port libre dès 8000 */

    /* --- services réseau : configuration (Settings ▸ Network…) ---
     * Port et IPs d'un service ("rest", "upnp", "rtp", "multiroom").
     * Port <= 0 → défaut du service. IPs vide = toutes les interfaces. */
    int  (*svc_port)(const char* name);
    const char* (*svc_ips)(const char* name);

    /* --- flux audio de diffusion (stream du serveur web) --- */
    uint32_t (*web_read)(float* dst, uint32_t frames);

    /* --- skins : palette de couleurs --- */
    void (*skin_set_colors)(const mp_skin_colors* colors);

    /* --- skins : image de fond (PNG/JPEG/BMP) ---
     * Chemin UTF-8 de l'image affichée en fond de la fenêtre
     * principale (étirée). Chaîne vide = aucune image. */
    void (*skin_set_bg)(const char* path_utf8);

    /* --- skins : zone du visualiseur ---
     * Coordonnées (relatives à la fenêtre) de la zone du visualiseur.
     * Sans appel, la zone par défaut est utilisée. */
    void (*skin_set_visual_rect)(int x, int y, int w, int h);

    /* --- skins : disposition de la fenêtre ---
     * menu_visible : 1 = barre de menus en haut (défaut), 0 = cachée
     *   (menu accessible par clic droit).
     * ctrl_top : 1 = boutons de contrôle en haut (à la place du menu),
     *   0 = en bas (défaut). */
    void (*skin_set_layout)(int menu_visible, int ctrl_top);

    /* --- métadonnées & jaquette des fichiers (plugins SERVICE) --- */
    const char* (*get_metadata)(const char* path, const char* field);
    const unsigned char* (*get_cover)(const char* path, size_t* len);
    const wchar_t* (*plist_path)(int i);

    /* --- skins : palette courante (lecture) ---
     * Renvoie la palette du skin actif (pour les fenêtres liées au
     * thème, ex. l'equalizer). NULL si indisponible. */
    const mp_skin_colors* (*get_skin_colors)(void);
} mp_host_api;

/*
 * API d'un plugin. Toutes les fonctions sont optionnelles sauf name().
 * Les hooks sont appelés uniquement si le type correspondant est déclaré
 * par type() et si le plugin est activé (Settings ▸ Plugins).
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

    /* --- hook VISUAL : flux audio (lecture seule) ---
     * Reçoit chaque bloc PCM interleavé (float 32) tel qu'il est joué,
     * APRÈS les effets audio. Destiné aux plugins visuels pour analyser
     * le signal (spectre, oscilloscope...). Ne PAS modifier le buffer.
     * Appelé depuis le thread audio → faire une copie rapide. */
    void (*audio_frames)(mp_plugin* self, const float* samples, unsigned frames,
                         unsigned channels, unsigned sample_rate);

    /* --- hook VISUAL ---
     * hdc = contexte de dessin de la zone d'affichage du lecteur.
     * Appelé périodiquement (~30 FPS, thread UI) quand la lecture est
     * active. w/h = taille de la zone en pixels. */
    void (*render)(mp_plugin* self, void* hdc, int width, int height);

    /* --- hook SKIN ---
     * hwnd = fenêtre principale. Le plugin peut modifier les couleurs,
     * la police, les contrôles... Appelé une fois après le chargement. */
    void (*apply_skin)(mp_plugin* self, void* hwnd);

    /* --- hook SERVICE ---
     * Événements transverses (MP_SERVICE_* : reconfiguration du serveur
     * web...). data : selon l'événement (NULL pour MP_SERVICE_WEB_APPLY). */
    void (*service)(mp_plugin* self, int event, void* data);

    /* --- hook SERVICE : métadonnées ---
     * Renvoie le titre (balises ID3…) d'un fichier audio, ou NULL si
     * inconnu. Buffer statique du plugin (valable jusqu'au prochain
     * appel). Utilisé par l'UI pour afficher le titre. */
    const char* (*get_title)(mp_plugin* self, const char* path);

    /* --- hook SERVICE : métadonnées ---
     * Renvoie une métadonnée d'un fichier audio ("title", "artist",
     * "album"...), ou NULL si inconnue. Buffer statique du plugin. */
    const char* (*get_metadata)(mp_plugin* self, const char* path,
                                const char* field);
} mp_plugin_api;

/* Instance d'un plugin chargé (rempli par le chargeur : mp_plugins_scan) */
struct mp_plugin {
    void* dll;                      /* handle de la DLL chargée */
    const mp_plugin_api* api;       /* API du plugin */
    int enabled;                    /* actif (case/radio du menu Plugins) */
    int visible;                    /* affiché dans le menu Plugins ? */
    wchar_t path[MAX_PATH];         /* chemin complet de la DLL */
};

#define MP_PLUGIN_ENTRY "mp_plugin_entry"
typedef const mp_plugin_api* (*mp_plugin_entry_fn)(void);

#endif /* MP_PLUGIN_H */
