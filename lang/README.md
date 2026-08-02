# Langues de MusicPlayer

Chaque langue = un fichier texte `CODE.lang` dans ce dossier
(`en.lang`, `fr.lang`, ...). **N'importe qui peut ajouter une langue**
sans recompiler : copiez `en.lang` en `votre_code.lang` et traduisez.

## Règles du format

- Une entrée par ligne : `cle=valeur`
- `#` ou `;` en début de ligne = commentaire
- Fichier en **UTF-8** (avec ou sans BOM)
- `\n` dans une valeur = retour à la ligne
- Les marqueurs `%d`, `%s`, `%hs` sont des emplacements de formatage
  (nombre, texte, version) — **conservez-les tels quels**.
- La première clé, `lang_name`, est le nom de la langue **dans sa propre
  langue** (ex. `Français`, `Deutsch`, `Ελληνικά`) : c'est ce qui s'affiche
  dans le menu Language.
- Le code du fichier (nom sans extension) doit faire 2 à 7 caractères
  (ex. `de`, `pt-BR`).

## Comment ça marche

1. Au démarrage, MusicPlayer utilise la langue de Windows si elle est
   disponible dans `lang/`, sinon l'anglais.
2. Menu **Language** (ou **Langue**) : bascule instantanée, mémorisée
   dans `%APPDATA%\MusicPlayer\lang.txt`.
3. L'anglais est embarqué dans le programme : il fonctionne même si
   `en.lang` est supprimé.
