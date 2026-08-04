#ifndef MP_SVC_H
#define MP_SVC_H

/* Gestion du service Windows « MusicPlayerCore » (musicplayer-core.exe
 * --service) depuis le client : install / uninstall / start / stop /
 * status. Utilisé par Settings ▸ Interface et par cc_start (le client
 * se connecte au service s'il tourne, au lieu de lancer le moteur). */

#define MP_SVC_NAME L"MusicPlayerCore"
#define MP_SVC_DISPLAY L"MusicPlayer Core (moteur client/serveur)"

/* 0 = ok, 1 = déjà installé/démarré (pas une erreur), -1 = échec. */
int  svc_install(void);
int  svc_uninstall(void);
int  svc_start(void);
int  svc_stop(void);

/* 1 = installé, 0 = non, -1 = erreur */
int  svc_installed(void);
/* 1 = en cours d'exécution, 0 = sinon, -1 = erreur/non installé */
int  svc_running(void);

/* État lisible : "Running", "Stopped", "Not installed"… */
void svc_status_text(char* out, int outsz);

#endif /* MP_SVC_H */
