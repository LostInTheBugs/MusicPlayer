#ifndef MP_CORE_PLAYLIST_H
#define MP_CORE_PLAYLIST_H

#include <windows.h>

/* Playlist du moteur (core). Extraite de main.c — le client (UI) et le
 * core (moteur) partagent le même comportement de playlist. */

#define PLAYLIST_MAX 512

extern wchar_t* g_plist[PLAYLIST_MAX];
extern wchar_t* g_plist_title[PLAYLIST_MAX];
extern int  g_plist_n;
extern int  g_plist_idx;
extern int  g_cd_mode;      /* 1 = lecture CD audio (MCI) */

/* Vide la playlist (libère les chemins). */
void core_plist_clear(void);

/* Ajoute un chemin (copié). */
void core_plist_add(const wchar_t* path);
void core_plist_add2(const wchar_t* path, const wchar_t* title);

/* Scan récursif d'un dossier (mp3/mp4), tri alphabétique, puis lit le
 * premier morceau. Retourne 0 si OK, -1 si aucun fichier. */
int core_plist_open_folder(const wchar_t* dir);

/* Lit l'index i (0 = OK, -1 = erreur). */
int core_plist_play_index(int i);

/* Morceau suivant / précédent (aléatoire si activé, boucle). */
void core_plist_next(void);
void core_plist_prev(void);

/* Enchaînement automatique en fin de morceau (appelé par le timer). */
void core_plist_tick(void);

/* Mode aléatoire. */
void core_plist_set_shuffle(int on);
int  core_plist_get_shuffle(void);

/* Verrou global de la playlist (threads HTTP + timer). */
void core_plist_lock(void);
void core_plist_unlock(void);

#endif /* MP_CORE_PLAYLIST_H */

/* log du moteur (fichier logs/musicplayer-core.log) */
void core_log(const char* msg);
