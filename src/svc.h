#ifndef MP_SVC_H
#define MP_SVC_H

/* Démarrage automatique du moteur (musicplayer-core.exe) au login :
 * clé HKCU\...\CurrentVersion\Run — AUCUN droit administrateur requis.
 * Le moteur lancé au login affiche une icône dans la zone de
 * notification (clic droit : client / page web / quitter).
 * Utilisé par Settings ▸ Interface et par cc_start (le client se
 * connecte au moteur s'il tourne, au lieu de le relancer). */

/* 0 = ok, 1 = déjà dans cet état (pas une erreur), -1 = échec. */
int  svc_install(void);    /* active l'autostart au login */
int  svc_uninstall(void);  /* désactive l'autostart */
int  svc_start(void);      /* lance le moteur maintenant */
int  svc_stop(void);       /* arrête le moteur (processus) */

/* 1 = autostart activé, 0 = non, -1 = erreur */
int  svc_installed(void);
/* 1 = moteur en cours d'exécution, 0 = sinon, -1 = erreur */
int  svc_running(void);

/* État lisible : "Autostart : activé — moteur en cours d'exécution"… */
void svc_status_text(char* out, int outsz);

#endif /* MP_SVC_H */
