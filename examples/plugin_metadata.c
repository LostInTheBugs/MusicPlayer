/*
 * MusicPlayer — plugin : métadonnées des fichiers audio.
 *
 * Type SERVICE : lit les balises ID3 (titre) des MP3 — ID3v2 (frames
 * TIT2) puis ID3v1 (les 128 derniers octets). L'interface affiche le
 * titre au lieu du nom de fichier quand il est disponible.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "plugin.h"

static char g_title[512];
static char g_artist[512];
static char g_album[512];
static char g_last_path[MAX_PATH * 3] = "";

/* Lit une frame texte ID3v2 (après l'en-tête de 10 octets) dans dst. */
static void read_text_frame(long pos, long fsz, FILE* f, char* dst)
{
    if (fsz <= 1) return;
    if (fseek(f, pos + 11, SEEK_SET) != 0) return;  /* +10 en-tête +1 encodage */
    unsigned n = (unsigned)fsz - 1;
    if (n > 511) n = 511;
    if (fread(dst, 1, n, f) == n) {
        dst[n] = 0;
        while (n > 0 && (dst[n - 1] == 0 || dst[n - 1] == ' ')) n--;
        dst[n] = 0;
    }
}

static void parse_tags(const char* path)
{
    g_title[0] = g_artist[0] = g_album[0] = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return;

    /* ID3v2 : en-tête 10 octets "ID3" + taille syncsafe */
    unsigned char h[10];
    if (fread(h, 1, 10, f) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
        unsigned sz = ((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) |
                      ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
        long pos = 10;
        long end = 10 + (long)sz;
        while (pos + 10 <= end) {
            unsigned char fr[10];
            if (fseek(f, pos, SEEK_SET) != 0) break;
            if (fread(fr, 1, 10, f) != 10) break;
            unsigned fsz = ((unsigned)fr[4] << 24) | ((unsigned)fr[5] << 16) |
                           ((unsigned)fr[6] << 8) | fr[7];
            if (fr[0] == 'T') {
                char* dst = NULL;
                if (fr[1] == 'I' && fr[2] == 'T' && fr[3] == '2') dst = g_title;
                else if (fr[1] == 'P' && fr[2] == 'E' && fr[3] == '1') dst = g_artist;
                else if (fr[1] == 'A' && fr[2] == 'L' && fr[3] == 'B') dst = g_album;
                if (dst) read_text_frame(pos, (long)fsz, f, dst);
            }
            if (fsz == 0) break;
            pos += 10 + (long)fsz;
        }
    }

    /* ID3v1 : "TAG" + titre(30) + artiste(30) + album(30) */
    if (!g_title[0] && fseek(f, -128, SEEK_END) == 0) {
        unsigned char t[128];
        if (fread(t, 1, 128, f) == 128 &&
            t[0] == 'T' && t[1] == 'A' && t[2] == 'G') {
            for (int k = 0; k < 3; k++) {
                char* dst = k == 0 ? g_title : (k == 1 ? g_artist : g_album);
                int n = 30;
                while (n > 0 && (t[3 + k * 30 + n - 1] == 0 ||
                                 t[3 + k * 30 + n - 1] == ' ')) n--;
                if (n > 0) {
                    memcpy(dst, t + 3 + k * 30, (size_t)n);
                    dst[n] = 0;
                }
            }
        }
    }

    fclose(f);
}

static const char* pl_get_title(mp_plugin* self, const char* path)
{
    (void)self;
    if (!path) return NULL;
    if (strcmp(g_last_path, path) != 0) {
        strncpy(g_last_path, path, sizeof(g_last_path) - 1);
        g_last_path[sizeof(g_last_path) - 1] = 0;
        parse_tags(path);
    }
    return g_title[0] ? g_title : NULL;
}

static const char* pl_get_metadata(mp_plugin* self, const char* path,
                                   const char* field)
{
    (void)self;
    if (!path || !field) return NULL;
    if (strcmp(g_last_path, path) != 0) {
        strncpy(g_last_path, path, sizeof(g_last_path) - 1);
        g_last_path[sizeof(g_last_path) - 1] = 0;
        parse_tags(path);
    }
    if (!strcmp(field, "title"))  return g_title[0]  ? g_title  : NULL;
    if (!strcmp(field, "artist")) return g_artist[0] ? g_artist : NULL;
    if (!strcmp(field, "album"))  return g_album[0]  ? g_album  : NULL;
    return NULL;
}

static const char* pl_name(void)    { return "Metadata (MP3 tags)"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "Reads MP3 ID3 tags (title) to display the track title instead of the file name"; }
static unsigned    pl_type(void)    { return MP_PLUGIN_SERVICE; }

static const mp_plugin_api g_api = {
    MP_PLUGIN_API_VERSION,
    pl_name, pl_version, pl_description, pl_type,
    NULL, NULL,           /* init, destroy */
    NULL, NULL, NULL, NULL,   /* process, audio_frames, render, apply_skin */
    NULL, pl_get_title, pl_get_metadata  /* service, get_title, get_metadata */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
