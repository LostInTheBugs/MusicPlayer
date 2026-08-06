/* src/update.h — vérification de mises à jour (GitHub Releases) */
#ifndef MP_UPDATE_H
#define MP_UPDATE_H

#include <windows.h>

/* Message posté à la fenêtre quand la vérification est terminée.
 * wParam = manuel (1) ou automatique (0) ; lParam = état :
 *   0 = à jour, 1 = nouvelle version, 2 = erreur réseau. */
#define MP_UPDATE_DONE (WM_APP + 7)

/* Lance une vérification en arrière-plan (thread). Le résultat est
 * posté à `hwnd` via MP_UPDATE_DONE. `manual` = 1 si l'utilisateur
 * a cliqué sur le menu (affichera le résultat même si à jour). */
void mp_update_check_async(HWND hwnd, int manual);

/* Mode de mise à jour (persisté dans %APPDATA%\\upd.txt) :
 *   0 = désactivé, 1 = automatique (vérifie au démarrage),
 *   2 = manuel (via Settings ▸ Update…),
 *   3 = autonome (vérifie toutes les heures, applique et redémarre) */
int  mp_update_get_mode(void);
void mp_update_set_mode(int mode);

/* Type de mises à jour proposées :
 *   0 = toutes, 1 = correctives seulement (versions -cX) */
int  mp_update_get_type(void);
void mp_update_set_type(int type);

/* Délai avant d'appliquer / de signaler une nouvelle version
 * (jours) : 0 = aucun, 1, 7, 30. Défaut : 7. */
int  mp_update_get_lag(void);
int  mp_update_get_plugins(void);
void mp_update_set_plugins(int on);
int  mp_update_get_channel(void);
void mp_update_set_channel(int ch);
void mp_update_set_lag(int days);

/* Équivalents simplifiés (compatibilité) : auto = mode 1 ; set_auto(on)
 * bascule entre automatique (1) et manuel (2). */
int  mp_update_auto_enabled(void);
void mp_update_set_auto(int on);

/* Dernière version trouvée (valide après MP_UPDATE_DONE) */
const char* mp_update_latest(void);

/* Ignore une version : elle ne sera plus proposée aux vérifications
 * suivantes (seules les versions postérieures le seront). */
void mp_update_skip(const char* version);

/* Télécharge le zip de la release `tag` vers `out_path` (UTF-16).
 * Retourne 0 si OK, -1 en cas d'erreur réseau ou d'écriture. */
int mp_update_download(const char* tag, const wchar_t* out_path);

/* Mode autonome : télécharge la dernière version et la déploie via un
 * script (attente de fermeture, extraction, relance). Retourne 0 si le
 * script a été lancé (l'appelant doit alors quitter le processus). */
int mp_update_apply_and_restart(void);

#endif
