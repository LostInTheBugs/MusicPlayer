# MusicPlayer Languages

Each language is a plain-text `CODE.lang` file in this folder
(`en.lang`, `fr.lang`, ...). **Anyone can add a language without
recompiling**: copy `en.lang` to `your_code.lang` and translate.

## Format rules

- One entry per line: `key=value`
- `#` or `;` at the start of a line = comment
- File in **UTF-8** (with or without BOM)
- `\n` in a value = line break
- `%d`, `%s`, `%hs` markers are format placeholders
  (number, text, version) — **keep them as-is**.
- The first key, `lang_name`, is the language name **in its own
  language** (e.g. `Français`, `Deutsch`, `Ελληνικά`): this is what shows
  in the Language menu.
- The file code (name without extension) must be 2 to 7 characters
  (e.g. `de`, `pt-BR`).

## How it works

1. At startup, MusicPlayer uses the Windows language if available in
   `lang/`, otherwise English.
2. **Language** menu: instant switch, remembered in
   `%APPDATA%\MusicPlayer\lang.txt`.
3. English is embedded in the program: it works even if `en.lang` is deleted.
