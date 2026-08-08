/*
 * Transcribe (Whisper) — plugin SERVICE
 * =====================================
 * Offline speech-to-text transcription via whisper.cpp (whisper-cli.exe).
 *
 *   GET  /health                   → {"ok":1}
 *   GET  /models                   → available .ggml models
 *   POST /transcribe               → {"path","lang","model"} → full result
 *   GET  /transcripts              → saved transcripts list
 *   GET  /transcript?file=<urlenc> → saved transcript content
 *   GET  /progress                 → current task state
 *
 * Pipeline: source → ffmpeg (WAV 16 kHz mono PCM) → whisper-cli.exe (-oj)
 * → JSON → %APPDATA%\MusicPlayer\transcripts\<hash>.json
 *
 * One file at a time: if /progress says busy, the client must wait.
 */
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/plugin.h"
#include "http_util.h"

#define TRANSCRIBE_PORT 8083
#define MAX_OUT 16777216      /* 16 Mo max pour un JSON de sortie */
#define EXTRACT_TIMEOUT 300   /* s */
#define WHISPER_TIMEOUT 600   /* s */

static const mp_host_api* g_h = NULL;
static volatile LONG g_running = 0;
static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_thread = NULL;

/* état de la tâche courante (pour /progress) */
static volatile LONG g_busy = 0;
static char g_stage[64] = "";
static char g_source[MAX_PATH * 2] = "";

/* ------------------------------------------------------------------ */
static void log_line(const char* msg)
{
    if (g_h && g_h->log) g_h->log(msg);
}

static void json_escape_a(const char* in, char* out, int max)
{
    int o = 0;
    for (int i = 0; in[i] && o < max - 4; i++) {
        if ((unsigned char)in[i] >= 32 && in[i] != '"' && in[i] != '\\') {
            out[o++] = in[i];
        } else {
            o += snprintf(out + o, max - o, "\\u%04x",
                          (unsigned char)in[i]);
        }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
static void http_headers(SOCKET c, int code, const char* ctype, int len)
{
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, code == 200 ? "OK" : "Not Found",
        ctype ? ctype : "application/json", len);
    http_send_all(c, hdr, (int)strlen(hdr));
}

static void send_body(SOCKET c, const char* body, const char* ctype)
{
    http_headers(c, 200, ctype, (int)strlen(body));
    send(c, body, (int)strlen(body), 0);
}

static void send_404(SOCKET c)
{
    const char* b = "{\"error\":\"not found\"}";
    http_headers(c, 404, "application/json", (int)strlen(b));
    send(c, b, (int)strlen(b), 0);
}

/* ------------------------------------------------------------------ */
/* JSON helpers (space tolerant — "key": "v" or "key":"v")             */
/* ------------------------------------------------------------------ */
static const char* json_str(const char* body, const char* key,
                            char* out, int outsz)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    const char* e = strchr(p, '"');
    if (!e) return NULL;
    int n = (int)(e - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, (size_t)n);
    out[n] = 0;
    return out;
}

static double json_num(const char* body, const char* key, double def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return def;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atof(p);
}

/* ------------------------------------------------------------------ */
/* FNV-1a 64-bit hash → hex string (file naming, no external lib)     */
/* ------------------------------------------------------------------ */
static void hash_hex(const char* s, char* out, int outsz)
{
    unsigned long long h = 1469598103934665603ULL;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    snprintf(out, outsz, "%016llx", h);
}

/* ------------------------------------------------------------------ */
/* Dossiers                                                            */
/* ------------------------------------------------------------------ */
static int appdata_path(const wchar_t* sub, wchar_t* out, int out_chars)
{
    wchar_t base[MAX_PATH];
    /* S_OK == 0 : le test était INVERSÉ (!resultat) → appdata jamais
     * trouvé → ffmpeg.exe / whisper-cli.exe / modèles dans %APPDATA%
     * jamais localisés (bug depuis la création du plugin) */
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, base) != S_OK)
        return -1;
    _snwprintf(out, out_chars, L"%ls\\MusicPlayer\\%ls", base, sub);
    return 0;
}

static void mkdirs(const wchar_t* dir)
{
    wchar_t tmp[MAX_PATH];
    _snwprintf(tmp, MAX_PATH, L"%ls", dir);
    for (wchar_t* p = tmp + 1; *p; p++) {
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(tmp, NULL);
}

static void store_dir(wchar_t* out, int out_chars)
{
    if (appdata_path(L"transcripts", out, out_chars) != 0)
        _snwprintf(out, out_chars, L"%ls\\transcripts",
                   L"C:\\Users\\Public\\MusicPlayer");
    mkdirs(out);
}

static void models_dir(wchar_t* out, int out_chars)
{
    if (appdata_path(L"whisper-models", out, out_chars) != 0)
        _snwprintf(out, out_chars, L"%ls\\whisper-models",
                   L"C:\\Users\\Public\\MusicPlayer");
    mkdirs(out);
}

/* ------------------------------------------------------------------ */
/* Recherche des exécutables                                           */
/* ------------------------------------------------------------------ */
static int find_exe(const wchar_t* names[], int n_names, wchar_t* out,
                    int out_chars)
{
    wchar_t base[MAX_PATH];
    wchar_t appdir[MAX_PATH];
    GetModuleFileNameW(NULL, base, MAX_PATH);
    wchar_t* sl = wcsrchr(base, L'\\');
    if (sl) *sl = 0;
    _snwprintf(appdir, MAX_PATH, L"%ls", base);

    /* 1) %APPDATA%\MusicPlayer\whisper\  (ou ffmpeg\) */
    if (appdata_path(names[0], out, out_chars) == 0)
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
            return 0;

    /* 2) core_plugins\whisper-cli.exe */
    _snwprintf(out, out_chars, L"%ls\\core_plugins\\%ls", appdir,
               names[1]);
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
        return 0;

    /* 3) ..\whisper-cli.exe */
    _snwprintf(out, out_chars, L"%ls\\..\\%ls", appdir, names[1]);
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
        return 0;

    /* 4) à côté de l'exe */
    _snwprintf(out, out_chars, L"%ls\\%ls", appdir, names[1]);
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
        return 0;

    (void)n_names;
    return -1;
}

static int whisper_exe(wchar_t* out, int out_chars)
{
    const wchar_t* names[] = {
        L"whisper\\whisper-cli.exe", L"whisper-cli.exe"
    };
    return find_exe(names, 2, out, out_chars);
}

static int ffmpeg_exe(wchar_t* out, int out_chars)
{
    const wchar_t* names[] = {
        L"ffmpeg\\ffmpeg.exe", L"ffmpeg.exe"
    };
    return find_exe(names, 2, out, out_chars);
}

/* ------------------------------------------------------------------ */
/* Sous-processus : lance une commande, capture stdout+stderr          */
/* Retour : 0 = ok (exit 0), -1 = lancement/échec, -2 = timeout        */
/* ------------------------------------------------------------------ */
typedef struct {
    HANDLE pipe;
    char* buf;
    int cap;
    int len;
} reader_t;

static DWORD WINAPI pipe_reader(LPVOID arg)
{
    reader_t* r = (reader_t*)arg;
    for (;;) {
        if (r->len + 4096 >= r->cap) {
            int ncap = r->cap * 2;
            if (ncap > MAX_OUT) break;
            char* nb = (char*)realloc(r->buf, ncap);
            if (!nb) break;
            r->buf = nb;
            r->cap = ncap;
        }
        DWORD rd = 0;
        if (!ReadFile(r->pipe, r->buf + r->len,
                      (DWORD)(r->cap - r->len - 1), &rd, NULL) || rd == 0)
            break;
        r->len += (int)rd;
        r->buf[r->len] = 0;
    }
    return 0;
}

static int run_proc(wchar_t* cmdline, int timeout_s, char** out_buf,
                    int* out_len)
{
    HANDLE hOutR = NULL, hOutW = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) return -1;
    SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOutW;
    si.hStdError = hOutW;

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hOutR);
        CloseHandle(hOutW);
        return -1;
    }
    CloseHandle(hOutW);

    reader_t rd;
    rd.pipe = hOutR;
    rd.cap = 16384;
    rd.len = 0;
    rd.buf = (char*)malloc(rd.cap);
    if (!rd.buf) {
        CloseHandle(hOutR);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return -1;
    }
    rd.buf[0] = 0;

    HANDLE rt = CreateThread(NULL, 0, pipe_reader, &rd, 0, NULL);
    DWORD wait = WaitForSingleObject(pi.hProcess, (DWORD)timeout_s * 1000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (rt) {
        WaitForSingleObject(rt, 2000);
        CloseHandle(rt);
    }
    CloseHandle(hOutR);

    *out_buf = rd.buf;
    *out_len = rd.len;
    if (wait == WAIT_TIMEOUT) return -2;
    return code == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Extraction audio : source → WAV 16 kHz mono PCM                     */
/* ------------------------------------------------------------------ */
static int extract_audio(const wchar_t* input, const wchar_t* wav,
                         char** err_out)
{
    wchar_t fexe[MAX_PATH];
    if (ffmpeg_exe(fexe, MAX_PATH) != 0) {
        if (err_out) *err_out = _strdup("ffmpeg.exe not found");
        return -1;
    }
    wchar_t cmd[4096];
    _snwprintf(cmd, 4096,
               L"\"%ls\" -y -i \"%ls\" -ar 16000 -ac 1 -c:a pcm_s16le \"%ls\"",
               fexe, input, wav);
    char* out = NULL;
    int len = 0;
    int rc = run_proc(cmd, EXTRACT_TIMEOUT, &out, &len);
    if (rc == 0) {
        /* vérifie le header RIFF du FICHIER WAV produit — la sortie du
         * process est textuelle (logs ffmpeg), le check sur le pipe ne
         * peut jamais passer */
        HANDLE hf = CreateFileW(wav, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            char hdr[12];
            DWORD rd = 0;
            BOOL ok = ReadFile(hf, hdr, sizeof(hdr), &rd, NULL) &&
                      rd == sizeof(hdr);
            CloseHandle(hf);
            if (!ok || memcmp(hdr, "RIFF", 4) != 0 ||
                memcmp(hdr + 8, "WAVE", 4) != 0)
                rc = -1;
        } else {
            rc = -1;   /* pas de fichier produit : extraction ratée */
        }
    }
    if (rc != 0 && err_out) {
        char msg[512];
        if (rc == -2) {
            snprintf(msg, sizeof(msg), "Audio extraction timed out");
        } else if (out && out[0]) {
            snprintf(msg, sizeof(msg), "Audio extraction failed: %.400s",
                     out);
        } else {
            snprintf(msg, sizeof(msg), "Audio extraction failed");
        }
        *err_out = _strdup(msg);
    }
    free(out);
    return rc == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Whisper : lance whisper-cli.exe, lit <out>.json (le -oj écrit le    */
/* JSON dans le fichier de sortie)                                     */
/* ------------------------------------------------------------------ */
static int read_file_alloc(const wchar_t* path, char** out, int* out_len)
{
    HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) return -1;
    DWORD sz = GetFileSize(hf, NULL);
    if (sz == INVALID_FILE_SIZE || sz > MAX_OUT) {
        CloseHandle(hf);
        return -1;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { CloseHandle(hf); return -1; }
    DWORD rd = 0;
    BOOL ok = ReadFile(hf, buf, sz, &rd, NULL) && rd == sz;
    CloseHandle(hf);
    if (!ok) { free(buf); return -1; }
    buf[sz] = 0;
    *out = buf;
    *out_len = (int)sz;
    return 0;
}

static int whisper_run(const wchar_t* model, const wchar_t* wav,
                       const wchar_t* lang, const wchar_t* out_dir,
                       const wchar_t* out_name, char** json_out,
                       char** stderr_out)
{
    wchar_t wexe[MAX_PATH];
    if (whisper_exe(wexe, MAX_PATH) != 0) {
        if (stderr_out) *stderr_out = _strdup("whisper-cli.exe not found");
        return -1;
    }
    wchar_t of[MAX_PATH];
    _snwprintf(of, MAX_PATH, L"%ls\\%ls", out_dir, out_name);
    wchar_t cmd[4096];
    _snwprintf(cmd, 4096,
               L"\"%ls\" -m \"%ls\" -f \"%ls\" -l %ls -oj -of \"%ls\"",
               wexe, model, wav, lang, of);
    char* out = NULL;
    int len = 0;
    int rc = run_proc(cmd, WHISPER_TIMEOUT, &out, &len);
    if (stderr_out) *stderr_out = out ? _strdup(out) : _strdup("");
    if (rc != 0) {
        free(out);
        return rc;   /* -1 échec, -2 timeout */
    }
    /* le JSON est écrit dans <of>.json par -oj */
    wchar_t jpath[MAX_PATH];
    _snwprintf(jpath, MAX_PATH, L"%ls.json", of);
    char* jbuf = NULL;
    int jlen = 0;
    if (read_file_alloc(jpath, &jbuf, &jlen) != 0) {
        free(out);
        return -1;
    }
    free(out);
    *json_out = jbuf;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Transcription : chemin source → hash → fichier <hash>.json          */
/* ------------------------------------------------------------------ */
static void index_update(const char* source, const char* hash)
{
    wchar_t dir[MAX_PATH];
    store_dir(dir, MAX_PATH);
    wchar_t ipath[MAX_PATH];
    _snwprintf(ipath, MAX_PATH, L"%ls\\transcripts_index.json", dir);
    char* content = NULL;
    int len = 0;
    char esc[2048];
    json_escape_a(source, esc, sizeof(esc));
    if (read_file_alloc(ipath, &content, &len) == 0) {
        /* remplace l'entrée existante (même source) ou ajoute */
        char pat[2200];
        snprintf(pat, sizeof(pat), "{\"source\":\"%s\",", esc);
        char* hit = strstr(content, pat);
        if (hit) {
            char* e = strchr(hit, '}');
            if (e) {
                char newent[2300];
                snprintf(newent, sizeof(newent),
                         "{\"source\":\"%s\",\"hash\":\"%s\"}", esc, hash);
                int pos = (int)(hit - content);
                int elen = (int)(e - hit + 1);
                char* nb = (char*)malloc((size_t)len - elen +
                                         strlen(newent) + 1);
                if (nb) {
                    memcpy(nb, content, (size_t)pos);
                    strcpy(nb + pos, newent);
                    strcpy(nb + pos + strlen(newent), content + pos + elen);
                    free(content);
                    content = nb;
                    len = (int)strlen(nb);
                }
            }
        } else {
            /* ajoute avant le ] final */
            char* br = strrchr(content, ']');
            if (br) {
                char newent[2300];
                snprintf(newent, sizeof(newent),
                         "%s{\"source\":\"%s\",\"hash\":\"%s\"}]",
                         len > 2 ? "," : "", esc, hash);
                int pos = (int)(br - content);
                char* nb = (char*)malloc((size_t)pos + strlen(newent) + 1);
                if (nb) {
                    memcpy(nb, content, (size_t)pos);
                    strcpy(nb + pos, newent);
                    free(content);
                    content = nb;
                    len = (int)strlen(nb);
                }
            }
        }
    } else {
        char* nb = (char*)malloc(4096);
        if (nb) {
            snprintf(nb, 4096,
                     "{\"entries\":[{\"source\":\"%s\",\"hash\":\"%s\"}]}",
                     esc, hash);
            content = nb;
            len = (int)strlen(nb);
        }
    }
    if (content) {
        HANDLE hf = CreateFileW(ipath, GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(hf, content, (DWORD)len, &wr, NULL);
            CloseHandle(hf);
        }
        free(content);
    }
}

static int index_find(const char* source, char* hash_out, int hash_sz)
{
    hash_out[0] = 0;
    wchar_t dir[MAX_PATH];
    store_dir(dir, MAX_PATH);
    wchar_t ipath[MAX_PATH];
    _snwprintf(ipath, MAX_PATH, L"%ls\\transcripts_index.json", dir);
    char* content = NULL;
    int len = 0;
    if (read_file_alloc(ipath, &content, &len) != 0) return -1;
    char esc[2048];
    json_escape_a(source, esc, sizeof(esc));
    char pat[2200];
    snprintf(pat, sizeof(pat), "{\"source\":\"%s\",\"hash\":\"", esc);
    char* hit = strstr(content, pat);
    if (hit) {
        const char* h = hit + strlen(pat);
        const char* he = strchr(h, '"');
        if (he) {
            int hl = (int)(he - h);
            if (hl >= hash_sz) hl = hash_sz - 1;
            memcpy(hash_out, h, (size_t)hl);
            hash_out[hl] = 0;
        }
    }
    free(content);
    return hash_out[0] ? 0 : -1;
}

static int transcript_save(const char* source, const char* lang,
                           const char* model, double duration_s,
                           const char* segments_json,
                           const char* full_text, char* hash_out,
                           int hash_sz)
{
    char hash[64];
    hash_hex(source, hash, sizeof(hash));
    if (hash_out) snprintf(hash_out, hash_sz, "%s", hash);

    SYSTEMTIME st;
    GetSystemTime(&st);
    char date[64];
    snprintf(date, sizeof(date), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);

    char esc_src[2048], esc_txt[65536];
    json_escape_a(source, esc_src, sizeof(esc_src));
    json_escape_a(full_text ? full_text : "", esc_txt, sizeof(esc_txt));

    int cap = (int)strlen(segments_json) + 8192;
    char* body = (char*)malloc((size_t)cap);
    if (!body) return -1;
    snprintf(body, cap,
             "{\"source\":\"%s\",\"date\":\"%s\",\"lang\":\"%s\","
             "\"model\":\"%s\",\"duration_s\":%.1f,"
             "\"segments\":%s,\"full_text\":\"%s\"}",
             esc_src, date, lang, model, duration_s,
             segments_json ? segments_json : "[]", esc_txt);

    wchar_t dir[MAX_PATH];
    store_dir(dir, MAX_PATH);
    wchar_t fpath[MAX_PATH];
    _snwprintf(fpath, MAX_PATH, L"%ls\\%hs.json", dir, hash);
    HANDLE hf = CreateFileW(fpath, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, 0, NULL);
    int rc = -1;
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        if (WriteFile(hf, body, (DWORD)strlen(body), &wr, NULL)) rc = 0;
        CloseHandle(hf);
    }
    free(body);
    if (rc == 0) index_update(source, hash);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Conversion du JSON whisper → segments internes + full_text          */
/* ------------------------------------------------------------------ */
static int parse_whisper_json(const char* json, char** segments_out,
                              char** full_out, double* duration_out)
{
    /* {"transcription":[{"timestamps":{...},"offsets":{"from":0,
     *  "to":5200},"text":"..."},...]} */
    char* segs = (char*)malloc(16384);
    char* full = (char*)malloc(65536);
    if (!segs || !full) {
        free(segs);
        free(full);
        return -1;
    }
    int so = 0, fo = 0;
    segs[0] = '[';
    so = 1;
    full[0] = 0;
    double max_to = 0.0;
    int first = 1;

    const char* cur = json;
    while ((cur = strstr(cur, "\"transcription\"")) != NULL) {
        cur = strchr(cur, '[');
        if (!cur) break;
        cur++;
        while ((cur = strchr(cur, '{')) != NULL) {
            /* extrait l'objet (gère l'imbrication { } ) */
            int depth = 0;
            const char* e = cur;
            for (; *e; e++) {
                if (*e == '{') depth++;
                else if (*e == '}') {
                    depth--;
                    if (depth == 0) { e++; break; }
                }
            }
            if (*e == 0) break;
            char* obj = (char*)malloc((size_t)(e - cur) + 1);
            if (!obj) break;
            memcpy(obj, cur, (size_t)(e - cur));
            obj[e - cur] = 0;

            const char* of = strstr(obj, "\"offsets\"");
            double from = 0.0, to = 0.0;
            char txt[8192] = "";
            if (of) {
                const char* f = strstr(of, "\"from\"");
                if (f) {
                    f = strchr(f, ':');
                    if (f) from = atof(f + 1);
                }
                const char* t = strstr(of, "\"to\"");
                if (t) {
                    t = strchr(t, ':');
                    if (t) to = atof(t + 1);
                }
            }
            json_str(obj, "text", txt, sizeof(txt));
            /* trim */
            char* ts = txt;
            while (*ts == ' ') ts++;
            int tl = (int)strlen(ts);
            while (tl > 0 && (ts[tl - 1] == ' ' || ts[tl - 1] == '\n'))
                ts[--tl] = 0;
            if (ts[0]) {
                if (!first) {
                    if (so < 16380) { segs[so++] = ','; segs[so] = 0; }
                    if (fo < 65530) { full[fo++] = ' '; full[fo] = 0; }
                }
                first = 0;
                char esc[8192];
                json_escape_a(ts, esc, sizeof(esc));
                int need = (int)strlen(esc) + 64;
                if (so + need < 16384) {
                    int n = snprintf(segs + so, 16384 - so,
                                     "{\"start\":%.1f,\"end\":%.1f,"
                                     "\"text\":\"%s\"}",
                                     from / 1000.0, to / 1000.0, esc);
                    so += n;
                }
                if (fo + (int)strlen(ts) < 65500) {
                    int n = snprintf(full + fo, 65536 - fo, "%s", ts);
                    fo += n;
                }
                if (to > max_to) max_to = to;
            }
            free(obj);
            cur = e;
        }
        break;
    }
    if (so > 0) { segs[so] = ']'; segs[so + 1] = 0; }
    else { segs[0] = '['; segs[1] = ']'; segs[2] = 0; }
    *segments_out = segs;
    *full_out = full;
    *duration_out = max_to / 1000.0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* POST /transcribe                                                    */
/* ------------------------------------------------------------------ */
static void handle_transcribe(SOCKET c, const char* body)
{
    char path[MAX_PATH * 2] = "", lang[32] = "auto", model[64] = "medium";
    json_str(body, "path", path, sizeof(path));
    json_str(body, "lang", lang, sizeof(lang));
    json_str(body, "model", model, sizeof(model));

    if (!path[0]) {
        send_body(c, "{\"ok\":0,\"error\":\"path required\"}",
                  "application/json");
        return;
    }
    wchar_t wpath[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 2);
    /* les URL http(s) (épisodes de podcasts en streaming) sont acceptées :
     * ffmpeg les lit directement — pas de contrôle de fichier local */
    int is_url = _strnicmp(path, "http://", 7) == 0 ||
                 _strnicmp(path, "https://", 8) == 0;
    if (!is_url && GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) {
        send_body(c, "{\"ok\":0,\"error\":\"File not found\"}",
                  "application/json");
        return;
    }
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        send_body(c, "{\"ok\":0,\"error\":\"Transcription in progress\"}",
                  "application/json");
        return;
    }

    snprintf(g_source, sizeof(g_source), "%s", path);
    char* resp = NULL;
    int rc = 0;

    /* dossier temporaire */
    char hash[64];
    hash_hex(path, hash, sizeof(hash));
    wchar_t tmpbase[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpbase);
    wchar_t tmpdir[MAX_PATH];
    _snwprintf(tmpdir, MAX_PATH,
               L"%lsmusicplayer_transcribe\\%hs_%lld",
               tmpbase, hash, (long long)GetTickCount64());
    mkdirs(tmpdir);

    wchar_t wav[MAX_PATH];
    _snwprintf(wav, MAX_PATH, L"%ls\\audio.wav", tmpdir);

    /* 1) extraction audio */
    snprintf(g_stage, sizeof(g_stage), "extracting");
    log_line("Transcribe: extracting audio...");
    char* err = NULL;
    if (extract_audio(wpath, wav, &err) != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "{\"ok\":0,\"error\":\"%s\"}", err ? err : "extract failed");
        free(err);
        err = NULL;   /* évite le DOUBLE FREE dans cleanup (corrompait
                       * la réponse envoyée : memory garbage au lieu du
                       * JSON d'erreur quand l'extraction échoue) */
        resp = _strdup(msg);
        rc = -1;
        goto cleanup;
    }
    log_line("Transcribe: audio extracted");

    /* 2) modèle */
    wchar_t mdir[MAX_PATH];
    models_dir(mdir, MAX_PATH);
    wchar_t mpath[MAX_PATH];
    _snwprintf(mpath, MAX_PATH, L"%ls\\ggml-%hs.bin", mdir, model);
    if (GetFileAttributesW(mpath) == INVALID_FILE_ATTRIBUTES) {
        /* le modèle demandé est absent : on prend le premier modèle
         * disponible du dossier (n'importe quelle taille) */
        wchar_t pat[MAX_PATH];
        _snwprintf(pat, MAX_PATH, L"%ls\\ggml-*.bin", mdir);
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileW(pat, &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            FindClose(hf);
            _snwprintf(mpath, MAX_PATH, L"%ls\\%ls", mdir, fd.cFileName);
            log_line("Transcribe: requested model missing, using first "
                     "available model from the models folder");
        } else {
            char msg[1024];
            snprintf(msg, sizeof(msg),
                     "{\"ok\":0,\"error\":\"Model not found - download a "
                     "whisper.cpp model (e.g. ggml-medium.bin) into "
                     "%%APPDATA%%\\\\MusicPlayer\\\\whisper-models\\\\\"}");
            resp = _strdup(msg);
            rc = -1;
            goto cleanup;
        }
    }

    /* 3) whisper */
    snprintf(g_stage, sizeof(g_stage), "transcribing");
    log_line("Transcribe: running whisper...");
    char* jbuf = NULL;
    char* serr = NULL;
    wchar_t wlang[32];
    MultiByteToWideChar(CP_UTF8, 0, lang[0] ? lang : "auto", -1,
                        wlang, 32);
    int wrc = whisper_run(mpath, wav, wlang, tmpdir, L"out",
                          &jbuf, &serr);
    if (wrc == -2) {
        resp = _strdup("{\"ok\":0,\"error\":\"Transcription timed out\"}");
        rc = -1;
        goto cleanup;
    }
    if (wrc != 0 || !jbuf) {
        char msg[2048];
        snprintf(msg, sizeof(msg),
                 "{\"ok\":0,\"error\":\"Whisper process failed\","
                 "\"stderr\":\"%s\"}",
                 serr && serr[0] ? serr : "");
        resp = _strdup(msg);
        rc = -1;
        goto cleanup;
    }

    /* 4) parse + sauvegarde */
    char* segs = NULL;
    char* full = NULL;
    double dur = 0.0;
    if (parse_whisper_json(jbuf, &segs, &full, &dur) != 0) {
        resp = _strdup("{\"ok\":0,\"error\":\"Whisper output parse failed\"}");
        rc = -1;
        goto cleanup;
    }
    wchar_t mname[128];
    _snwprintf(mname, 128, L"ggml-%hs.bin", model);
    char mname_u8[128];
    WideCharToMultiByte(CP_UTF8, 0, mname, -1, mname_u8,
                        sizeof(mname_u8), NULL, NULL);
    char hash2[64] = "";
    if (transcript_save(path, lang, mname_u8, dur, segs, full, hash2,
                        sizeof(hash2)) != 0) {
        resp = _strdup("{\"ok\":0,\"error\":\"Transcript save failed\"}");
        rc = -1;
        goto cleanup;
    }
    log_line("Transcribe: done");
    {
        char escf[65536];
        json_escape_a(full ? full : "", escf, sizeof(escf));
        int cap = (int)strlen(segs) + 7000;
        resp = (char*)malloc((size_t)cap);
        if (resp) {
            snprintf(resp, cap,
                     "{\"ok\":1,\"hash\":\"%s\",\"lang\":\"%s\","
                     "\"model\":\"ggml-%s.bin\",\"duration_s\":%.1f,"
                     "\"segments\":%s,\"full_text\":\"%s\"}",
                     hash2, lang, model, dur, segs, escf);
        }
        rc = 0;
    }
    free(segs);
    free(full);

cleanup:
    /* nettoie le dossier temporaire */
    {
        wchar_t jp[MAX_PATH];
        _snwprintf(jp, MAX_PATH, L"%ls\\out.json", tmpdir);
        DeleteFileW(jp);
        DeleteFileW(wav);
        RemoveDirectoryW(tmpdir);
    }
    free(err);
    InterlockedExchange(&g_busy, 0);
    g_stage[0] = 0;
    g_source[0] = 0;
    if (resp) {
        send_body(c, resp, "application/json");
        free(resp);
    } else {
        send_body(c, "{\"ok\":0,\"error\":\"internal error\"}",
                  "application/json");
    }
    (void)rc;
}

/* ------------------------------------------------------------------ */
/* GET /models                                                         */
/* ------------------------------------------------------------------ */
static void handle_models(SOCKET c)
{
    wchar_t dir[MAX_PATH];
    models_dir(dir, MAX_PATH);
    char out[8192];
    int o = 0;
    o += snprintf(out + o, sizeof(out) - o, "{\"models\":[");
    wchar_t pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%ls\\ggml-*.bin", dir);
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pat, &fd);
    int first = 1;
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                char name[512];
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name,
                                    sizeof(name), NULL, NULL);
                o += snprintf(out + o, sizeof(out) - o, "%s"
                              "{\"name\":\"%s\",\"size_mb\":%llu}",
                              first ? "" : ",",
                              name,
                              (unsigned long long)(fd.nFileSizeHigh
                                  ? 0xffffffffULL
                                  : fd.nFileSizeLow / 1000000ULL));
                first = 0;
            }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    char dirc[1024];
    WideCharToMultiByte(CP_UTF8, 0, dir, -1, dirc, sizeof(dirc), NULL, NULL);
    o += snprintf(out + o, sizeof(out) - o, "],\"dir\":\"%s\"}", dirc);
    send_body(c, out, "application/json");
}

/* ------------------------------------------------------------------ */
/* GET /transcripts                                                    */
/* ------------------------------------------------------------------ */
static void handle_transcripts(SOCKET c)
{
    wchar_t dir[MAX_PATH];
    store_dir(dir, MAX_PATH);
    char* out = (char*)malloc(262144);
    if (!out) {
        send_body(c, "[]", "application/json");
        return;
    }
    int o = 0;
    out[o++] = '[';
    wchar_t pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%ls\\*.json", dir);
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pat, &fd);
    int first = 1;
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!_wcsicmp(fd.cFileName, L"transcripts_index.json"))
                continue;
            wchar_t fp[MAX_PATH];
            _snwprintf(fp, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            char* content = NULL;
            int len = 0;
            if (read_file_alloc(fp, &content, &len) != 0) continue;
            char src[1024] = "", date[64] = "", lang[32] = "",
                 model[128] = "";
            double dur = 0.0;
            json_str(content, "source", src, sizeof(src));
            json_str(content, "date", date, sizeof(date));
            json_str(content, "lang", lang, sizeof(lang));
            json_str(content, "model", model, sizeof(model));
            dur = json_num(content, "duration_s", 0.0);
            free(content);
            char hash[128];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, hash,
                                sizeof(hash), NULL, NULL);
            char* dot = strchr(hash, '.');
            if (dot) *dot = 0;
            int n = snprintf(out + o, 262144 - o,
                             "%s{\"hash\":\"%s\",\"source\":\"%s\","
                             "\"date\":\"%s\",\"lang\":\"%s\","
                             "\"model\":\"%s\",\"duration_s\":%.1f}",
                             first ? "" : ",", hash, src, date, lang,
                             model, dur);
            o += n;
            first = 0;
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    out[o++] = ']';
    out[o] = 0;
    send_body(c, out, "application/json");
    free(out);
}

/* ------------------------------------------------------------------ */
/* GET /transcript?file=<urlenc>                                       */
/* ------------------------------------------------------------------ */
static void url_decode(const char* in, char* out, int outsz)
{
    int o = 0;
    for (const char* p = in; *p && o < outsz - 1; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char h[3] = { p[1], p[2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

static void handle_transcript(SOCKET c, const char* path)
{
    const char* eq = strchr(path, '=');
    if (!eq) {
        send_body(c, "{\"ok\":0,\"error\":\"file parameter required\"}",
                  "application/json");
        return;
    }
    char file[MAX_PATH * 2];
    url_decode(eq + 1, file, sizeof(file));
    char hash[64];
    if (index_find(file, hash, sizeof(hash)) != 0) {
        send_body(c, "{\"ok\":0,\"error\":\"No transcript for this file\"}",
                  "application/json");
        return;
    }
    wchar_t dir[MAX_PATH];
    store_dir(dir, MAX_PATH);
    wchar_t fp[MAX_PATH];
    _snwprintf(fp, MAX_PATH, L"%ls\\%hs.json", dir, hash);
    char* content = NULL;
    int len = 0;
    if (read_file_alloc(fp, &content, &len) != 0) {
        send_body(c, "{\"ok\":0,\"error\":\"Transcript file missing\"}",
                  "application/json");
        return;
    }
    send_body(c, content, "application/json");
    free(content);
}

/* ------------------------------------------------------------------ */
/* GET /progress                                                       */
/* ------------------------------------------------------------------ */
static void handle_progress(SOCKET c)
{
    char body[2048];
    if (g_busy) {
        char src[2048];
        json_escape_a(g_source, src, sizeof(src));
        snprintf(body, sizeof(body),
                 "{\"busy\":true,\"stage\":\"%s\",\"source\":\"%s\"}",
                 g_stage, src);
    } else {
        snprintf(body, sizeof(body), "{\"busy\":false}");
    }
    send_body(c, body, "application/json");
}

/* ------------------------------------------------------------------ */
static void handle_client(SOCKET c)
{
    char req[16384];
    int got = http_read_request(c, req, sizeof(req));
    if (got < 8) { closesocket(c); return; }
    char method[16] = "", path[1024] = "";
    sscanf(req, "%15s %1023s", method, path);

    if (!strcmp(method, "POST") && !strcmp(path, "/transcribe")) {
        if (!http_post_is_json(req)) {
            send_body(c, "{\"ok\":0,\"error\":\"forbidden\"}",
                      "application/json");
        } else {
            const char* body = strstr(req, "\r\n\r\n");
            if (!body) body = strstr(req, "\n\n");
            handle_transcribe(c, body ? body + (body[0] == '\r' ? 4 : 2)
                                      : "");
        }
    } else if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
        send_body(c, "{\"ok\":1}", "application/json");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/models")) {
        handle_models(c);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/transcripts")) {
        handle_transcripts(c);
    } else if (!strcmp(method, "GET") &&
               !strncmp(path, "/transcript", 11)) {
        handle_transcript(c, path);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/progress")) {
        handle_progress(c);
    } else {
        send_404(c);
    }
    closesocket(c);
}

static DWORD WINAPI client_thread(LPVOID arg)
{
    SOCKET c = (SOCKET)(SIZE_T)arg;
    handle_client(c);
    return 0;
}

static DWORD WINAPI accept_loop(LPVOID arg)
{
    (void)arg;
    while (g_running) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        HANDLE t = CreateThread(NULL, 0, client_thread,
                                (LPVOID)(SIZE_T)c, 0, NULL);
        if (t) CloseHandle(t);
        else closesocket(c);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
static int server_start(void)
{
    if (g_running) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { WSACleanup(); return -1; }
    int yes = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(TRANSCRIBE_PORT);
    if (bind(g_listen, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }
    listen(g_listen, 8);
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, accept_loop, NULL, 0, NULL);
    return 0;
}

static void server_stop(void)
{
    if (!g_running) return;
    InterlockedExchange(&g_running, 0);
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    WSACleanup();
}

/* ------------------------------------------------------------------ */
static const char* pl_name(void) { return "Transcribe (Whisper)"; }
static const char* pl_version(void)
{
#ifdef MP_BUILD_VERSION
    return MP_BUILD_VERSION;
#else
    return "1.0";
#endif
}
static const char* pl_description(void)
{
    return "Offline speech-to-text transcription via whisper.cpp "
           "(port 8083)";
}
static unsigned pl_type(void) { return MP_PLUGIN_SERVICE; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_h = host;
    return 0;
}

static void pl_destroy(mp_plugin* self)
{
    (void)self;
    server_stop();
    g_h = NULL;
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)data;
    if (!g_h) return;
    if (event != MP_SERVICE_WEB_APPLY && event != MP_SERVICE_CLICK) return;
    if (self->enabled) {
        if (!g_running) {
            if (server_start() == 0) {
                log_line("Transcribe (Whisper) listening on port 8083");
            } else {
                log_line("Transcribe (Whisper): port unavailable");
            }
        }
    } else {
        server_stop();
    }
}

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    pl_init, pl_destroy,
    NULL, NULL, NULL, NULL,   /* process, audio_frames, render, apply_skin */
    pl_service, NULL          /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
