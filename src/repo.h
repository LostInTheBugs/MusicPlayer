#ifndef MP_REPO_H
#define MP_REPO_H

/* Plugin repository: browse and download plugins/skins from remote
 * repositories (HTTP). The default repository is the project's own
 * (GitHub raw). Each repository serves a plugins.json index:
 *
 *   { "repo": "...", "plugins": [
 *       { "name": "...", "type": "skin|visual|effect|service|asset",
 *         "version": "...", "file": "bin/skins/xxx.dll",
 *         "desc": "..." }, ...
 *   ] }
 *
 * `file` is relative to the repository root (the raw base URL); the
 * local destination is the same path under the exe directory
 * (bin/skins/… → <exedir>\skins\…).
 */

#define REPO_DEFAULT_BASE \
    L"https://raw.githubusercontent.com/LostInTheBugs/MusicPlayer/master/repo"

#define REPO_PRE_BASE \
    L"https://raw.githubusercontent.com/LostInTheBugs/MusicPlayer/pre-release/repo"

/* Base du repository par défaut selon le canal de mise à jour :
 *   - canal pre-release → branche pre-release (plugins de test)
 *   - canal release     → master (production)
 * La branche pre-release peut ne pas exister sur GitHub (404) : les
 * appels gèrent l'échec (message + repli éventuel sur master). */
const wchar_t* repo_default_base(void);
int repo_is_pre_channel(void);

#define REPO_MAX_PLUGINS 128
#define REPO_MAX_URLS 16

/* Liste des repositories (persistée dans %APPDATA%\MusicPlayer\repos.txt,
 * une URL par ligne). Le repository du projet est ajouté par défaut
 * quand la liste est vide. */
int  repo_list_load(wchar_t urls[][512], int max);
void repo_list_save(const wchar_t urls[][512], int n);

typedef struct repo_plugin {
    char name[64];
    char type[16];       /* skin | visual | effect | service | asset */
    char version[32];
    char file[256];      /* path inside the repository (bin/skins/…) */
    char desc[256];
} repo_plugin;

/* Fetches and parses the index of a repository.
 * json_url: full URL of the plugins.json (base + "/plugins.json").
 * Returns 0 on success and fills *out (caller frees with repo_free),
 * -1 on network error, -2 on parse error. */
int  repo_fetch(const wchar_t* json_url, repo_plugin** out, int* out_n);
void repo_free(repo_plugin* list, int n);

/* Downloads one plugin file into the local plugins/skins folder.
 * base: repository base URL (same one used to build json_url).
 * Returns 0 on success, -1 on failure. */
int  repo_download(const wchar_t* base, const repo_plugin* p);

#endif /* MP_REPO_H */
