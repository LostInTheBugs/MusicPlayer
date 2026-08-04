/*
 * MusicPlayer — plugin : serveur web de contrôle à distance.
 *
 * Type SERVICE : la page web (télécommande) est servie depuis le
 * téléphone/tablette sur le même réseau. Activation/désactivation :
 * Settings ▸ Plugins. Configuration : Settings ▸ Web server…
 *
 * Endpoints :
 *   GET  /            → page télécommande (mobile friendly)
 *   GET  /api/state   → JSON (état, playlist, volume, vitesse, sortie)
 *   POST /api/cmd     → corps = play|stop|next|volup|voldown|speedup|
 *                       speeddown|audio|shuffle|playidx=N
 *   GET  /stream      → flux audio WAV (PCM 16 bits stéréo 44,1 kHz)
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "plugin.h"

static const mp_host_api* g_h = NULL;
static volatile LONG g_running = 0;
static SOCKET g_listens[32];
static int    g_n_listens = 0;

/* ------------------------------------------------------------------ */
/* Helpers réseau                                                      */
/* ------------------------------------------------------------------ */
static void send_all(SOCKET s, const char* data, int len)
{
    int off = 0;
    while (off < len) {
        int n = send(s, data + off, len - off, 0);
        if (n <= 0) return;
        off += n;
    }
}

static void http_response_len(SOCKET s, const char* status, const char* type,
                              const char* body, size_t body_len)
{
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
        "Content-Length: %d\r\nCache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n",
        status, type, (int)body_len);
    if (hl < 0) hl = 0;
    if (hl >= (int)sizeof(hdr)) hl = (int)sizeof(hdr) - 1;
    send_all(s, hdr, hl);
    send_all(s, body, (int)body_len);
}

static void http_response(SOCKET s, const char* status, const char* type,
                          const char* body)
{
    http_response_len(s, status, type, body, strlen(body));
}

static void json_escape(const wchar_t* in, char* out, int out_chars)
{
    char utf8[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 0) { out[0] = 0; return; }
    int o = 0;
    for (int i = 0; utf8[i] && o < out_chars - 4; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { out[o++] = ' '; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void json_escape_u8(const char* in, char* out, int out_chars)
{
    int o = 0;
    for (int i = 0; in[i] && o < out_chars - 4; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { out[o++] = ' '; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */
static void api_state(SOCKET s)
{
    char body[11000];
    char items[8000] = "";
    int n = g_h->plist_count();
    int idx = g_h->plist_index();
    if (n > 100) n = 100;
    for (int i = 0; i < n; i++) {
        char name[600];
        /* titre (métadonnées ID3) si disponible, sinon nom du fichier */
        const char* title = NULL;
        if (g_h->get_metadata && g_h->plist_path) {
            const wchar_t* pw = g_h->plist_path(i);
            char pu8[MAX_PATH * 3];
            if (pw &&
                WideCharToMultiByte(CP_UTF8, 0, pw, -1, pu8, sizeof(pu8),
                                    NULL, NULL) > 0)
                title = g_h->get_metadata(pu8, "title");
        }
        if (title && title[0]) json_escape_u8(title, name, sizeof(name));
        else json_escape(g_h->plist_name(i), name, sizeof(name));
        char tmp[700];
        snprintf(tmp, sizeof(tmp), "%s\"%s\"", i ? "," : "", name);
        if (strlen(items) + strlen(tmp) < sizeof(items) - 4) strcat(items, tmp);
    }
    const char* st = "stopped";
    int ms = g_h->get_state();
    if (ms == 1) st = "playing";
    else if (ms == 2) st = "paused";
    const char* ao = "pc";
    int a = g_h->get_audio_out();
    if (a == 1) ao = "phone";
    else if (a == 2) ao = "both";
    char name[600], title[512] = "", artist[512] = "", album[512] = "",
         year[32] = "";
    if (idx >= 0 && idx < n) json_escape(g_h->plist_name(idx), name, sizeof(name));
    else name[0] = 0;
    if (g_h->get_metadata) {
        const char* fn = g_h->get_file_name();
        if (fn && fn[0]) {
            const char* t = g_h->get_metadata(fn, "title");
            const char* ar = g_h->get_metadata(fn, "artist");
            const char* al = g_h->get_metadata(fn, "album");
            const char* yr = g_h->get_metadata(fn, "year");
            if (t && t[0]) json_escape_u8(t, title, sizeof(title));
            if (ar && ar[0]) json_escape_u8(ar, artist, sizeof(artist));
            if (al && al[0]) json_escape_u8(al, album, sizeof(album));
            if (yr && yr[0]) json_escape_u8(yr, year, sizeof(year));
        }
    }
    int bl = snprintf(body, sizeof(body),
        "{\"state\":\"%s\",\"pos\":%.1f,\"dur\":%.1f,\"idx\":%d,\"count\":%d,"
        "\"vol\":%.2f,\"speed\":%.2f,"
        "\"audio\":\"%s\",\"shuffle\":%d,\"dj\":%d,\"name\":\"%s\","
        "\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\",\"year\":\"%s\",\"items\":[%s]}",
        st, g_h->get_position(), g_h->get_duration(), idx, n,
        g_h->get_volume(), g_h->get_speed(), ao, g_h->get_shuffle(),
        g_h->get_dj_mode ? g_h->get_dj_mode() : 0,
        name, title, artist, album, year, items);
    /* snprintf termine toujours par \0 ; on borne la longueur envoyée */
    if (bl < 0) bl = 0;
    if (bl >= (int)sizeof(body)) bl = (int)sizeof(body) - 1;
    http_response_len(s, "200 OK", "application/json", body, (size_t)bl);
}

static void api_cmd(SOCKET s, const char* cmd)
{
    if (!strcmp(cmd, "play"))           g_h->play_pause();
    else if (!strcmp(cmd, "stop"))      g_h->stop();
    else if (!strcmp(cmd, "next"))      g_h->next();
    else if (!strcmp(cmd, "volup"))     g_h->set_volume(g_h->get_volume() + 0.1f);
    else if (!strcmp(cmd, "voldown"))   g_h->set_volume(g_h->get_volume() - 0.1f);
    else if (!strcmp(cmd, "speedup"))   g_h->set_speed(g_h->get_speed() * 1.1f);
    else if (!strcmp(cmd, "speeddown")) g_h->set_speed(g_h->get_speed() / 1.1f);
    else if (!strcmp(cmd, "audio"))
        g_h->set_audio_out((g_h->get_audio_out() + 1) % 3);
    else if (!strncmp(cmd, "playidx=", 8))
        g_h->plist_play(atoi(cmd + 8));
    else if (!strcmp(cmd, "shuffle"))   g_h->shuffle_toggle();
    else if (!strcmp(cmd, "dj") && g_h->dj_toggle) g_h->dj_toggle();
    http_response(s, "200 OK", "text/plain", "ok");
}

/* ------------------------------------------------------------------ */
/* Page web (télécommande)                                             */
/* ------------------------------------------------------------------ */
static const char PAGE_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\">\n"
"<title>MusicPlayer Remote</title>\n"
"<style>\n"
"*{box-sizing:border-box}\n"
"body{margin:0;padding:14px;background:#0e1116;color:#e8eef4;font-family:system-ui,-apple-system,sans-serif;max-width:520px;margin:0 auto}\n"
".top{position:sticky;top:0;background:#0e1116;z-index:5;padding-bottom:2px}\n"
"h1{font-size:17px;margin:0 0 10px;text-align:center;color:#7aa2f7}\n"
".btns{display:flex;flex-wrap:wrap;justify-content:center;gap:8px;margin:8px 0}\n"
".btn{width:70px;height:62px;border:0;border-radius:14px;background:#232b38;color:#fff;cursor:pointer;user-select:none;-webkit-user-select:none;display:flex;align-items:center;justify-content:center}\n"
".btn:active{transform:scale(.94);filter:brightness(1.4)}\n"
".btn.big{width:110px}\n"
"#bPlay{background:#2f6fe4}\n"
"#bStop{background:#c0392b}\n"
"#bNext{background:#1e8449}\n"
"#bAud{width:120px;flex-direction:column;gap:3px;font-size:11px;font-weight:600}\n"
"#bAud svg{width:24px;height:24px}\n"
"#bShuf{background:#232b38}\n"
"#bShuf.on{background:#e67e22}\n"
".btn svg{width:30px;height:30px;fill:currentColor}\n"
".meta{text-align:center;color:#9fb2c6;font-size:13px;margin:6px 0 4px;word-break:break-all}\n"
"#cover{display:block;margin:8px auto;max-width:220px;max-height:220px;border-radius:10px;box-shadow:0 2px 14px rgba(0,0,0,.45)}\n"
".item{padding:9px 12px;border-radius:9px;margin:3px 0;background:#161d27;font-size:14px;display:flex;gap:8px;cursor:pointer}\n"
".item .n{color:#5c6f84;min-width:26px}\n"
".item.cur{background:#2f6fe4;font-weight:600}\n"
".item.cur .n{color:#cfe0ff}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"top\">\n"
"<h1>MusicPlayer</h1>\n"
"<div class=\"btns\">\n"
"  <button class=\"btn big\" id=\"bPlay\"><svg viewBox=\"0 0 24 24\"><path d=\"M8 5v14l11-7z\"/></svg></button>\n"
"  <button class=\"btn\" id=\"bStop\"><svg viewBox=\"0 0 24 24\"><rect x=\"5\" y=\"5\" width=\"14\" height=\"14\"/></svg></button>\n"
"  <button class=\"btn\" id=\"bNext\"><svg viewBox=\"0 0 24 24\"><path d=\"M6 5v14l9-7zM17 5h2v14h-2z\"/></svg></button>\n"
"</div>\n"
"<div class=\"btns\">\n"
"  <button class=\"btn\" id=\"bVolUp\"><svg viewBox=\"0 0 24 24\"><path d=\"M4 9v6h4l5 5V4L8 9H4z\"/><path d=\"M15.5 8.5a4.5 4.5 0 010 7\"/></svg></button>\n"
"  <button class=\"btn\" id=\"bVolDn\"><svg viewBox=\"0 0 24 24\"><path d=\"M4 9v6h4l5 5V4L8 9H4z\"/><path d=\"M15.5 8.5a4.5 4.5 0 010 7\" opacity=\".3\"/></svg></button>\n"
"  <button class=\"btn\" id=\"bSpUp\"><svg viewBox=\"0 0 24 24\"><path d=\"M5 5l7 7-7 7zM12 5l7 7-7 7z\"/></svg></button>\n"
"  <button class=\"btn\" id=\"bSpDn\"><svg viewBox=\"0 0 24 24\"><path d=\"M19 5l-7 7 7 7zM12 5l-7 7 7 7z\"/></svg></button>\n"
"</div>\n"
"<div class=\"btns\">\n"
"  <button class=\"btn\" id=\"bShuf\" title=\"Shuffle\"><svg viewBox=\"0 0 24 24\"><path d=\"M10.59 9.17L5.41 4 4 5.41l5.17 5.17 1.42-1.41zM14.5 4l2.04 2.04L4 18.59 5.41 20 17.96 7.46 20 9.5V4h-5.5zm.33 9.41l-1.41 1.41 3.13 3.13L14.5 20H20v-5.5l-2.04 2.04-3.13-3.13z\"/></svg></button>\n"
"  <button class=\"btn\" id=\"bAud\" title=\"Audio output: PC / Phone / Both\"><svg viewBox=\"0 0 24 24\"><rect x=\"2\" y=\"4\" width=\"20\" height=\"12\" rx=\"1\"/><path d=\"M8 20h8M12 16v4\"/></svg><span>PC</span></button>\n"
"</div>\n"
"<div class=\"meta\">\n"
"  <div class=\"mt\" id=\"mtitle\">&hellip;</div>\n"
"  <div class=\"ma\" id=\"martist\"></div>\n"
"  <div class=\"ma\" id=\"malbum\"></div>\n"
"  <div class=\"ma\" id=\"myear\"></div>\n"
"</div>\n"
"<img id=\"cover\" alt=\"\" hidden>\n"
"</div>\n"
"<div style=\"text-align:center;margin:10px 0\">\n"
"<a href=\"/dj\" style=\"display:inline-block;padding:10px 18px;border-radius:12px;background:#3d1f3f;color:#f77aa2;text-decoration:none;font-weight:700\">🎚️ DJ Mixing</a>\n"
"</div>\n"
"<audio id=\"aud\" preload=\"none\" hidden></audio>\n"
"<div id=\"plist\"></div>\n"
"<script>\n"
"var $=function(id){return document.getElementById(id)};\n"
"var ICON_PLAY='<svg viewBox=\"0 0 24 24\"><path d=\"M8 5v14l11-7z\"/></svg>';\n"
"var ICON_PAUSE='<svg viewBox=\"0 0 24 24\"><path d=\"M6 5h4v14H6zM14 5h4v14h-4z\"/></svg>';\n"
"var AUD_ICONS={\n"
"  pc:'<svg viewBox=\"0 0 24 24\"><rect x=\"2\" y=\"4\" width=\"20\" height=\"12\" rx=\"1\"/><path d=\"M8 20h8M12 16v4\"/></svg>',\n"
"  phone:'<svg viewBox=\"0 0 24 24\"><path d=\"M8 2h8a1 1 0 011 1v18a1 1 0 01-1 1H8a1 1 0 01-1-1V3a1 1 0 011-1zM10 18h4\"/></svg>',\n"
"  both:'<svg viewBox=\"0 0 24 24\"><rect x=\"2\" y=\"3\" width=\"13\" height=\"9\" rx=\"1\"/><path d=\"M5 15h8M9 12v3\"/><path d=\"M17 6h3a2 2 0 012 2v10a2 2 0 01-2 2h-8a2 2 0 01-2-2\"/></svg>'\n"
"};\n"
"var AUD_LABELS={pc:'PC',phone:'Phone',both:'Both'};\n"
"function cmd(c){fetch('/api/cmd',{method:'POST',body:c}).catch(function(){})}\n"
"var aud=$('aud');\n"
"$('bPlay').onclick=function(){\n"
"  var playing=document.body.dataset.playing==='1';\n"
"  cmd('play');\n"
"  if(document.body.dataset.audio==='pc')return;\n"
"  if(playing){aud.pause();}\n"
"  else{if(aud.src.indexOf('/stream')<0){aud.src='/stream';}aud.play().catch(function(){});}\n"
"};\n"
"$('bStop').onclick=function(){cmd('stop');if(aud.src.indexOf('/stream')>=0){aud.pause();}};\n"
"$('bNext').onclick=function(){cmd('next')};\n"
"$('bVolUp').onclick=function(){cmd('volup')};\n"
"$('bVolDn').onclick=function(){cmd('voldown')};\n"
"$('bSpUp').onclick=function(){cmd('speedup')};\n"
"$('bSpDn').onclick=function(){cmd('speeddown')};\n"
"$('bAud').onclick=function(){cmd('audio')};\n"
"$('bShuf').onclick=function(){cmd('shuffle')};\n"
"function plClick(i){cmd('playidx='+i);}\n"
"function tick(){\n"
"  fetch('/api/state').then(function(r){return r.json()}).then(function(s){\n"
"    if(s.dj){window.location='/dj';return;}\n"
"    document.body.dataset.playing=s.state==='playing'?'1':'0';\n"
"    document.body.dataset.audio=s.audio;\n"
"    $('bPlay').innerHTML = s.state==='playing' ? ICON_PAUSE : ICON_PLAY;\n"
"    $('bAud').innerHTML = AUD_ICONS[s.audio] + '<span>' + AUD_LABELS[s.audio] + '</span>';\n"
"    $('bShuf').className = 'btn' + (s.shuffle ? ' on' : '');\n"
"    $('mtitle').textContent=s.title||s.name||'no file';\n"
"    $('martist').textContent=s.artist?('Artist: '+s.artist):'';\n"
"    $('malbum').textContent=s.album?('Album: '+s.album):'';\n"
"    $('myear').textContent=s.year?('Year: '+s.year):'';\n"
"    var cv=$('cover');\n"
"    if(s.name){cv.hidden=false;cv.src='/cover?t='+Date.now();}\n"
"    else{cv.hidden=true;}\n"
"    var h='';\n"
"    for(var i=0;i<s.items.length;i++){\n"
"      h+='<div class=\"item'+(i===s.idx?' cur':'')+'\" onclick=\"plClick('+i+')\"><span class=\"n\">'+(i+1)+'</span><span>'+s.items[i]+'</span></div>';\n"
"    }\n"
"    $('plist').innerHTML=h;\n"
"  }).catch(function(){});\n"
"}\n"
"setInterval(tick,1000);tick();\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ------------------------------------------------------------------ */
/* Mode DJ Mixage : 2 platines indépendantes (décodage FFmpeg interne) */
/* ------------------------------------------------------------------ */
#define DJ_DECKS 2
typedef struct {
    AVFormatContext* fmt;
    AVCodecContext*  dec;
    SwrContext*      swr;
    int              stream;
    int              opened;
    int              eof;
    int              paused;
} dj_deck;
static dj_deck g_decks[DJ_DECKS];

static void dj_close(dj_deck* d)
{
    if (d->swr) swr_free(&d->swr);
    if (d->dec) avcodec_free_context(&d->dec);
    if (d->fmt) avformat_close_input(&d->fmt);
    d->opened = 0; d->eof = 0; d->paused = 0;
}

static int dj_open(dj_deck* d, const char* path)
{
    dj_close(d);
    int ret = -1;
    int stream = -1;
    AVFormatContext* fmt = NULL;
    AVCodecContext*  dec = NULL;
    SwrContext*      swr = NULL;

    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) goto done;
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters* p = fmt->streams[i]->codecpar;
        if (p->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const AVCodec* codec = avcodec_find_decoder(p->codec_id);
        if (!codec) goto done;
        dec = avcodec_alloc_context3(codec);
        if (!dec) goto done;
        if (avcodec_parameters_to_context(dec, p) < 0) goto done;
        if (avcodec_open2(dec, codec, NULL) < 0) goto done;
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, 44100,
                                &dec->ch_layout, dec->sample_fmt,
                                dec->sample_rate, 0, NULL) == 0 && swr)
            swr_init(swr);
        stream = (int)i;
        break;
    }
    if (!dec || stream < 0) goto done;

    d->fmt = fmt; d->dec = dec; d->swr = swr; d->stream = stream;
    fmt = NULL; dec = NULL; swr = NULL;
    d->opened = 1;
    d->eof = 0;
    d->paused = 0;
    ret = 0;
done:
    /* chemins d'erreur : libérer proprement (pas de fuite) */
    if (swr) { swr_free(&swr); swr = NULL; }
    if (dec) { avcodec_free_context(&dec); dec = NULL; }
    if (fmt) { avformat_close_input(&fmt); fmt = NULL; }
    return ret;
}

/* Remplit fbuf (frames stéréo 44,1 kHz) ; retourne le nombre de frames. */
static uint32_t dj_read(dj_deck* d, float* fbuf, uint32_t max_frames)
{
    if (!d->opened || d->eof || d->paused) return 0;
    uint32_t out = 0;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    while (out < max_frames) {
        int r = av_read_frame(d->fmt, pkt);
        if (r < 0) { d->eof = 1; break; }
        if (pkt->stream_index != d->stream) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(d->dec, pkt) == 0) {
            while (out < max_frames &&
                   avcodec_receive_frame(d->dec, fr) == 0) {
                if (d->swr) {
                    float tmp[8192 * 2];
                    uint8_t* out_ptrs[1] = { (uint8_t*)tmp };
                    int got = swr_convert(d->swr, out_ptrs, 8192,
                                          (const uint8_t**)fr->extended_data,
                                          fr->nb_samples);
                    if (got > 0) {
                        uint32_t n = (uint32_t)got;
                        if (out + n > max_frames) n = max_frames - out;
                        memcpy(fbuf + out * 2, tmp,
                               (size_t)n * 2 * sizeof(float));
                        out += n;
                    }
                }
                av_frame_unref(fr);
            }
        }
        av_packet_unref(pkt);
        if (out >= max_frames) break;
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    return out;
}

typedef struct { SOCKET c; int deck; } dj_req;

static DWORD WINAPI dj_stream_thread(void* arg)
{
    dj_req* rq = (dj_req*)arg;
    SOCKET c = rq->c;
    dj_deck* d = &g_decks[rq->deck];
    free(rq);
    const char* hdr =
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
    send_all(c, hdr, (int)strlen(hdr));
    uint8_t wav[44] = {
        'R','I','F','F', 0xff,0xff,0xff,0x7f, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x44,0xac,0,0, 0x10,0xb1,0x02,0, 4,0, 16,0,
        'd','a','t','a', 0xff,0xff,0xff,0x7f
    };
    send_all(c, (const char*)wav, 44);
    static float fbuf[2048 * 2];
    static uint8_t obuf[16384];
    uint32_t idle = 0;
    while (g_running) {
        uint32_t n = dj_read(d, fbuf, 2048);
        if (n == 0) {
            if (d->eof) break;
            if (++idle > 25) {
                memset(obuf, 0, sizeof(obuf));
                if (send(c, (const char*)obuf, sizeof(obuf), 0) <= 0) break;
                idle = 0;
            }
            Sleep(20);
            continue;
        }
        idle = 0;
        for (uint32_t i = 0; i < n * 2; i++) {
            float v = fbuf[i];
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            int16_t s16 = (int16_t)(v * 32767.0f);
            obuf[i * 2]     = (uint8_t)(s16 & 0xff);
            obuf[i * 2 + 1] = (uint8_t)((uint16_t)s16 >> 8);
        }
        if (send(c, (const char*)obuf, (int)(n * 4), 0) <= 0) break;
        Sleep(5);    /* pacing : 46 ms de son par paquet (léger buffer) */
    }
    closesocket(c);
    return 0;
}

/* Page table de mixage : mixeur 2 voies (decks, égaliseur, crossfader) */
static const char PAGE_DJ[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>DJ Mix</title>\n"
"<style>\n"
"*{box-sizing:border-box}\n"
"body{margin:0;padding:12px;background:#101418;color:#e8eef4;font-family:system-ui,-apple-system,sans-serif;max-width:640px;margin:0 auto}\n"
".top{display:flex;justify-content:space-between;align-items:center;margin:2px 0 8px}\n"
".top a{color:#7aa2f7;text-decoration:none;font-size:13px}\n"
"h1{font-size:16px;margin:0;color:#f77aa2}\n"
".decks{display:flex;gap:10px;flex-wrap:wrap}\n"
".deck{flex:1;min-width:240px;background:#1a212b;border-radius:14px;padding:12px}\n"
".deck h2{font-size:13px;margin:0 0 8px;color:#9fb2c6;letter-spacing:1px}\n"
".deck select{width:100%;background:#232b38;color:#fff;border:1px solid #334;border-radius:8px;padding:7px;font-size:12px}\n"
".btns{display:flex;gap:6px;margin-top:8px}\n"
"button{flex:1;padding:9px 0;border:0;border-radius:8px;background:#2f6fe4;color:#fff;cursor:pointer;font-size:14px}\n"
"button.stop{background:#b3403a}\n"
".sl{margin-top:8px;font-size:11px;color:#9fb2c6}\n"
".sl input{width:100%}\n"
".eq{display:flex;gap:8px;margin-top:8px}\n"
".eq div{flex:1;text-align:center;font-size:10px;color:#9fb2c6}\n"
".eq input{width:100%;height:90px}\n"
".xf{margin:14px 0;text-align:center}\n"
".xf input{width:100%}\n"
".xf .lbl{display:flex;justify-content:space-between;font-size:12px;color:#9fb2c6}\n"
".vu{display:flex;gap:2px;height:10px;margin-top:6px;background:#0a0d12;border-radius:5px;overflow:hidden}\n"
".vu span{flex:1;background:#1d2632}\n"
".vu span.on{background:#3ddc84}\n"
"</style></head><body>\n"
"<div class=\"top\"><a href=\"/\">← MusicPlayer</a><h1>🎚️ DJ Mixing</h1><a href=\"#\" onclick=\"fetch('/api/cmd',{method:'POST',body:'dj'});setTimeout(function(){location.href='/'},300);return false\" style=\"color:#e67e22\">Quitter</a></div>\n"
"<div class=\"decks\">\n"
"<div class=\"deck\">\n"
"<h2>DECK A</h2>\n"
"<select id=\"selA\"></select>\n"
"<div class=\"btns\"><button id=\"bA\">▶</button><button class=\"stop\" id=\"sA\">■</button></div>\n"
"<div class=\"vu\" id=\"vuA\"></div>\n"
"<div class=\"sl\">Volume <input type=\"range\" id=\"volA\" min=\"0\" max=\"100\" value=\"100\"></div>\n"
"<div class=\"sl\">Pitch <input type=\"range\" id=\"pitchA\" min=\"-12\" max=\"12\" value=\"0\"> <span id=\"pA\">0%</span></div>\n"
"<div class=\"eq\"><div>Bass<input type=\"range\" id=\"bassA\" min=\"-12\" max=\"12\" value=\"0\"></div><div>Mid<input type=\"range\" id=\"midA\" min=\"-12\" max=\"12\" value=\"0\"></div><div>Treble<input type=\"range\" id=\"treA\" min=\"-12\" max=\"12\" value=\"0\"></div></div>\n"
"<audio id=\"aA\" preload=\"none\"></audio>\n"
"</div>\n"
"<div class=\"deck\">\n"
"<h2>DECK B</h2>\n"
"<select id=\"selB\"></select>\n"
"<div class=\"btns\"><button id=\"bB\">▶</button><button class=\"stop\" id=\"sB\">■</button></div>\n"
"<div class=\"vu\" id=\"vuB\"></div>\n"
"<div class=\"sl\">Volume <input type=\"range\" id=\"volB\" min=\"0\" max=\"100\" value=\"100\"></div>\n"
"<div class=\"sl\">Pitch <input type=\"range\" id=\"pitchB\" min=\"-12\" max=\"12\" value=\"0\"> <span id=\"pB\">0%</span></div>\n"
"<div class=\"eq\"><div>Bass<input type=\"range\" id=\"bassB\" min=\"-12\" max=\"12\" value=\"0\"></div><div>Mid<input type=\"range\" id=\"midB\" min=\"-12\" max=\"12\" value=\"0\"></div><div>Treble<input type=\"range\" id=\"treB\" min=\"-12\" max=\"12\" value=\"0\"></div></div>\n"
"<audio id=\"aB\" preload=\"none\"></audio>\n"
"</div>\n"
"</div>\n"
"<div class=\"xf\"><div class=\"lbl\"><span>A <b id=\"lA\">50%</b></span><span><b id=\"lB\">50%</b> B</span></div>\n"
"<input type=\"range\" id=\"xf\" min=\"0\" max=\"100\" value=\"50\"></div>\n"
"<script>\n"
"var $=function(id){return document.getElementById(id)};\n"
"var aA=$('aA'),aB=$('aB');\n"
"var ctx=new (window.AudioContext||window.webkitAudioContext)();\n"
"function makeChain(a){\n"
"  var s=ctx.createMediaElementSource(a);\n"
"  var bass=ctx.createBiquadFilter();bass.type='lowshelf';bass.frequency.value=250;\n"
"  var mid=ctx.createBiquadFilter();mid.type='peaking';mid.frequency.value=1000;mid.Q.value=0.8;\n"
"  var tre=ctx.createBiquadFilter();tre.type='highshelf';tre.frequency.value=4000;\n"
"  var gvol=ctx.createGain();var gxf=ctx.createGain();\n"
"  s.connect(bass);bass.connect(mid);mid.connect(tre);tre.connect(gvol);gvol.connect(gxf);gxf.connect(ctx.destination);\n"
"  return {bass:bass,mid:mid,tre:tre,gvol:gvol,gxf:gxf};\n"
"}\n"
"var chA=makeChain(aA),chB=makeChain(aB);\n"
"function band(id,f){f.gain.value=parseInt($(id).value,10);}\n"
"$('bassA').oninput=function(){band('bassA',chA.bass)};\n"
"$('midA').oninput=function(){band('midA',chA.mid)};\n"
"$('treA').oninput=function(){band('treA',chA.tre)};\n"
"$('bassB').oninput=function(){band('bassB',chB.bass)};\n"
"$('midB').oninput=function(){band('midB',chB.mid)};\n"
"$('treB').oninput=function(){band('treB',chB.tre)};\n"
"$('volA').oninput=function(){chA.gvol.gain.value=parseInt(this.value,10)/100};\n"
"$('volB').oninput=function(){chB.gvol.gain.value=parseInt(this.value,10)/100};\n"
"$('pitchA').oninput=function(){aA.playbackRate=1+parseInt(this.value,10)/100;$('pA').textContent=this.value+'%'};\n"
"$('pitchB').oninput=function(){aB.playbackRate=1+parseInt(this.value,10)/100;$('pB').textContent=this.value+'%'};\n"
"function playDeck(a,sel){if(!sel.value)return;a.src='/dj/stream'+(a===aA?'A':'B')+'?idx='+sel.value;a.play().catch(function(){});}\n"
"$('bA').onclick=function(){if(aA.paused){playDeck(aA,$('selA'));}else{aA.pause();}};\n"
"$('bB').onclick=function(){if(aB.paused){playDeck(aB,$('selB'));}else{aB.pause();}};\n"
"$('sA').onclick=function(){aA.pause();aA.removeAttribute('src');};\n"
"$('sB').onclick=function(){aB.pause();aB.removeAttribute('src');};\n"
"function xf(){var x=parseInt($('xf').value,10);chA.gxf.gain.value=(100-x)/100;chB.gxf.gain.value=x/100;$('lA').textContent=(100-x)+'%';$('lB').textContent=x+'%';}\n"
"$('xf').oninput=xf;xf();\n"
"fetch('/api/state').then(function(r){return r.json()}).then(function(s){\n"
"  var h='';\n"
"  for(var i=0;i<s.items.length;i++){h+='<option value=\"'+i+'\">'+s.items[i]+'</option>';}\n"
"  $('selA').innerHTML=h;$('selB').innerHTML=h;\n"
"}).catch(function(){});\n"
"</script></body></html>\n";

/* ------------------------------------------------------------------ */
/* Flux audio /stream                                                  */
/* ------------------------------------------------------------------ */
static DWORD WINAPI stream_thread(void* arg)
{
    SOCKET c = (SOCKET)(intptr_t)arg;
    const char* hdr =
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
    send_all(c, hdr, (int)strlen(hdr));

    uint8_t wav[44] = {
        'R','I','F','F', 0xff,0xff,0xff,0x7f, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x44,0xac,0,0, 0x10,0xb1,0x02,0, 4,0, 16,0,
        'd','a','t','a', 0xff,0xff,0xff,0x7f
    };
    send_all(c, (const char*)wav, 44);

    /* tampons propres à CE thread : un thread par connexion → jamais
     * de static partagé (deux clients se corrompraient) */
    float* fbuf = (float*)malloc(1024 * 2 * sizeof(float));
    uint8_t* obuf = (uint8_t*)malloc(4096);
    if (!fbuf || !obuf) {
        free(fbuf);
        free(obuf);
        closesocket(c);
        return 0;
    }
    uint32_t idle = 0;
    float vol = 1.0f;
    while (g_running) {
        uint32_t n = g_h->web_read(fbuf, 1024);
        if (n == 0) {
            if (++idle > 50) {
                memset(obuf, 0, 4096);
                if (send(c, (const char*)obuf, 4096, 0) <= 0) break;
                idle = 0;
            }
            Sleep(20);
            continue;
        }
        idle = 0;
        vol = g_h->get_volume();
        for (uint32_t i = 0; i < n * 2; i++) {
            float v = fbuf[i] * vol;
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            int16_t s16 = (int16_t)(v * 32767.0f);
            obuf[i * 2]     = (uint8_t)(s16 & 0xff);
            obuf[i * 2 + 1] = (uint8_t)((uint16_t)s16 >> 8);
        }
        if (send(c, (const char*)obuf, (int)(n * 4), 0) <= 0) break;
    }
    closesocket(c);
    free(fbuf);
    free(obuf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatch + acceptation                                              */
/* ------------------------------------------------------------------ */
static void dispatch(SOCKET c)
{
    char req[8192];
    int rn = recv(c, req, sizeof(req) - 1, 0);
    if (rn <= 0) { closesocket(c); return; }
    req[rn] = 0;
    char method[8], path[512];
    if (sscanf(req, "%7s %511s", method, path) != 2) {
        closesocket(c);
        return;
    }
    if (!strcmp(path, "/")) {
        http_response(c, "200 OK", "text/html; charset=utf-8", PAGE_HTML);
        closesocket(c);
    } else if (!strcmp(path, "/api/state")) {
        api_state(c);
        closesocket(c);
    } else if (!strcmp(path, "/api/cmd")) {
        const char* body = strstr(req, "\r\n\r\n");
        if (body) body += 4;
        else body = req + rn;
        api_cmd(c, body);
        closesocket(c);
    } else if (!strcmp(path, "/dj")) {
        http_response(c, "200 OK", "text/html; charset=utf-8", PAGE_DJ);
        closesocket(c);
    } else if (!strncmp(path, "/dj/streamA", 11) ||
               !strncmp(path, "/dj/streamB", 11)) {
        /* table de mixage : platine A ou B.
         * path = "/dj/streamA?idx=…" → le deck est à path[10] */
        int deck = path[10] == 'A' ? 0 : 1;
        const char* q = strchr(path, '=');
        int idx = q ? atoi(q + 1) : -1;
        if (idx >= 0 && idx < g_h->plist_count()) {
            const wchar_t* pw = g_h->plist_path(idx);
            char pu8[MAX_PATH * 3];
            if (pw && WideCharToMultiByte(CP_UTF8, 0, pw, -1, pu8,
                                          sizeof(pu8), NULL, NULL) > 0 &&
                dj_open(&g_decks[deck], pu8) == 0) {
                dj_req* rq = (dj_req*)malloc(sizeof(dj_req));
                if (rq) {
                    rq->c = c;
                    rq->deck = deck;
                    HANDLE h = CreateThread(NULL, 0, dj_stream_thread,
                                            rq, 0, NULL);
                    if (h) { CloseHandle(h); return; }
                    free(rq);
                }
            }
        }
        closesocket(c);
    } else if (!strncmp(path, "/stream", 7)) {
        HANDLE h = CreateThread(NULL, 0, stream_thread, (void*)(intptr_t)c, 0, NULL);
        if (h) CloseHandle(h);
        else closesocket(c);
    } else if (!strcmp(path, "/cover")) {
        /* jaquette de la musique en cours */
        size_t clen = 0;
        const unsigned char* cdata = NULL;
        if (g_h->get_cover && g_h->get_file_name())
            cdata = g_h->get_cover(g_h->get_file_name(), &clen);
        if (cdata && clen > 0) {
            const char* ct = "image/jpeg";
            if (clen > 8 && cdata[0] == 0x89 && cdata[1] == 'P' &&
                cdata[2] == 'N' && cdata[3] == 'G')
                ct = "image/png";
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                "Content-Length: %d\r\nCache-Control: no-cache\r\n"
                "Connection: close\r\n\r\n", ct, (int)clen);
            if (hl < 0) hl = 0;
            if (hl >= (int)sizeof(hdr)) hl = (int)sizeof(hdr) - 1;
            send_all(c, hdr, hl);
            send_all(c, (const char*)cdata, (int)clen);
        } else {
            http_response(c, "404 Not Found", "text/plain", "no cover");
        }
        closesocket(c);
    } else {
        http_response(c, "404 Not Found", "text/plain", "not found");
        closesocket(c);
    }
}

static DWORD WINAPI accept_thread(void* arg)
{
    SOCKET s = (SOCKET)(intptr_t)arg;
    while (g_running) {
        SOCKET c = accept(s, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        dispatch(c);
    }
    return 0;
}

static void server_stop(void)
{
    InterlockedExchange(&g_running, 0);
    for (int i = 0; i < g_n_listens; i++) closesocket(g_listens[i]);
    g_n_listens = 0;
    Sleep(120);
}

static int server_start(int port, const char* ips)
{
    server_stop();
    g_n_listens = 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    InterlockedExchange(&g_running, 1);

    char ipbuf[64];
    const char* p = ips ? ips : "";
    int failed = 0;
    do {
        int len = 0;
        while (*p && *p != ';' && len < 63) ipbuf[len++] = *p++;
        ipbuf[len] = 0;
        if (*p == ';') p++;
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) { failed = 1; break; }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)port);
        if (len > 0)
            a.sin_addr.s_addr = inet_addr(ipbuf);
        else
            a.sin_addr.s_addr = htonl(INADDR_ANY);
        int yes = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(s, 8) != 0) {
            closesocket(s);
            failed = 1;
            break;
        }
        g_listens[g_n_listens++] = s;
        HANDLE h = CreateThread(NULL, 0, accept_thread, (void*)(intptr_t)s, 0, NULL);
        if (h) CloseHandle(h);
    } while (*p && g_n_listens < 32);

    if (failed || g_n_listens == 0) {
        for (int i = 0; i < g_n_listens; i++) closesocket(g_listens[i]);
        g_n_listens = 0;
        WSACleanup();
        InterlockedExchange(&g_running, 0);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Plugin                                                              */
/* ------------------------------------------------------------------ */
static const char* pl_name(void)        { return "Web Server"; }
static const char* pl_version(void)     { return "1.0"; }
static const char* pl_description(void)
{ return "Remote control web page (phone/tablet) + audio stream"; }
static unsigned    pl_type(void)        { return MP_PLUGIN_SERVICE; }

static int pl_init(mp_plugin* self, const mp_host_api* host)
{
    (void)self;
    g_h = host;
    /* le serveur est démarré par l'événement MP_SERVICE_WEB_APPLY,
     * envoyé par l'hôte au démarrage et après chaque changement de config */
    return 0;
}

static void pl_destroy(mp_plugin* self)
{
    (void)self;
    server_stop();
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)self; (void)data;
    if (event != MP_SERVICE_WEB_APPLY || !g_h) return;
    if (g_h->web_enabled && g_h->web_enabled()) {
        if (server_start(g_h->web_port(), g_h->web_ips()) == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Web server listening on port %d",
                      g_h->web_port());
            if (g_h->log) g_h->log(msg);
        } else if (g_h->log) {
            g_h->log("Web server: cannot bind the port (in use?)");
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
