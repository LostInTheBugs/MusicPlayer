#ifndef MP_LANG_H
#define MP_LANG_H

#include <windows.h>

/*
 * Système de langues de MusicPlayer.
 *
 * Les traductions vivent dans des fichiers texte "lang/<code>.lang"
 * (UTF-8, format cle=valeur, '#' = commentaire, \n = retour à la ligne).
 * L'anglais est embarqué dans le binaire : c'est la langue de secours
 * si aucun fichier ne correspond.
 */

/* Scanne <dir> pour les fichiers ".lang" et charge la langue <code>
 * (NULL = langue système Windows, fallback "en"). */
void lang_init(const wchar_t* dir, const wchar_t* code);

/* Change de langue à chaud. Retourne 0 si OK. */
int lang_set(const wchar_t* code);

/* Traduction d'une clé (fallback anglais embarqué, puis la clé elle-même).
 * Résultat en UTF-16, buffer interne statique. */
const wchar_t* lang_get(const char* key);

/* Code de la langue courante ("en", "fr", ...). */
const wchar_t* lang_code(void);

/* Nom natif de la langue courante ("English", "Français", ...). */
const wchar_t* lang_native_name(void);

typedef struct {
    wchar_t code[8];    /* "fr" */
    wchar_t name[48];   /* "Français" (nom natif, lu dans le fichier) */
} lang_info;

/* Liste des langues disponibles (anglais embarqué en premier). */
const lang_info* lang_list(int* count);

#endif /* MP_LANG_H */
