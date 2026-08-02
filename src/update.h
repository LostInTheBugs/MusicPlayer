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

/* Vérification automatique au démarrage (persistée dans %APPDATA%) */
int  mp_update_auto_enabled(void);
void mp_update_set_auto(int on);

/* Dernière version trouvée (valide après MP_UPDATE_DONE) */
const char* mp_update_latest(void);

#endif
