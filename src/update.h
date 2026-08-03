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
 *   2 = manuel (via Settings ▸ Update…) */
int  mp_update_get_mode(void);
void mp_update_set_mode(int mode);

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

#endif
