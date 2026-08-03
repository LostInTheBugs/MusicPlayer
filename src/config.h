#ifndef MP_CONFIG_H
#define MP_CONFIG_H

/*
 * Configuration persistante de MusicPlayer (config.yml dans %APPDATA%).
 * Sauvegardée à la fermeture, chargée au démarrage.
 */

#include <windows.h>

typedef struct {
    int   volume;              /* 0..200 (%) */
    float speed;               /* 0.5..2.0 */
    char  last_path[1024];     /* dossier playlist ou fichier (UTF-8) */
    char  last_file[1024];     /* fichier en cours de lecture (UTF-8) */
    int   web_enabled;         /* serveur web activé */
    int   web_port;            /* port (0 = premier port libre) */
    int   web_audio;           /* 0 = PC, 1 = téléphone, 2 = les deux */
    char  web_ips[1024];       /* IP écoutées ("ip1;ip2;..." ; vide = toutes) */
    int   shuffle;             /* mode aléatoire */
    int   fs_screens;          /* nb d'écrans pour le plein écran (0 = simple) */
    int   fs_mode1, fs_mode2, fs_mode3; /* contenu écrans 2..4 :
                                         0 = visuel, 1 = playlist,
                                         2 = lyrics, 3 = jaquette */
    int   svc_rest_port;       /* REST API (0 = 8080) */
    char  svc_rest_ips[1024];
    int   svc_upnp_port;       /* DLNA/UPnP (0 = 8081) */
    char  svc_upnp_ips[1024];
    int   svc_rtp_port;        /* RTP/AES67 (0 = 5004) */
    char  svc_rtp_ips[1024];
    int   svc_mr_port;         /* Multiroom (0 = 5004) */
    char  svc_mr_ips[1024];
} app_config;

extern app_config g_cfg;

/* Charge la configuration (valeurs par défaut + migration web.txt). */
void config_load(void);

/* Sauvegarde la configuration. */
void config_save(void);

#endif /* MP_CONFIG_H */
