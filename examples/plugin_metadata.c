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

static const char* pl_get_title(mp_plugin* self, const char* path)
{
    (void)self;
    g_title[0] = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    /* ID3v2 : en-tête 10 octets "ID3" + taille syncsafe (le + gros) */
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
            if (fr[0] == 'T' && fr[1] == 'I' && fr[2] == 'T' && fr[3] == '2') {
                /* octet d'encodage + texte */
                if (fsz > 1 && fseek(f, pos + 11, SEEK_SET) == 0) {
                    unsigned n = fsz - 1;
                    if (n > 511) n = 511;
                    unsigned char* txt = (unsigned char*)malloc((size_t)n + 1);
                    if (txt) {
                        if (fread(txt, 1, (size_t)n, f) == (size_t)n) {
                            txt[n] = 0;
                            while (n > 0 && (txt[n - 1] == 0 || txt[n - 1] == ' '))
                                n--;
                            txt[n] = 0;
                            if (n > 0) {
                                memcpy(g_title, txt, (size_t)n);
                                g_title[n] = 0;
                            }
                        }
                        free(txt);
                    }
                }
                break;
            }
            if (fsz == 0) break;
            pos += 10 + (long)fsz;
        }
    }

    /* ID3v1 : "TAG" + titre sur 30 octets, à la fin du fichier */
    if (!g_title[0] && fseek(f, -128, SEEK_END) == 0) {
        unsigned char t[128];
        if (fread(t, 1, 128, f) == 128 &&
            t[0] == 'T' && t[1] == 'A' && t[2] == 'G') {
            int n = 30;
            while (n > 0 && (t[3 + n - 1] == 0 || t[3 + n - 1] == ' ')) n--;
            if (n > 0) {
                memcpy(g_title, t + 3, (size_t)n);
                g_title[n] = 0;
            }
        }
    }

    fclose(f);
    return g_title[0] ? g_title : NULL;
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
    NULL, pl_get_title    /* service, get_title */
};

const mp_plugin_api* mp_plugin_entry(void) { return &g_api; }
