/*
 * MusicPlayer — serveur web de contrôle à distance.
 *
 * HTTP minimal (Winsock) sur un thread dédié :
 *   GET  /            → page télécommande (mobile friendly)
 *   GET  /api/state   → JSON (état, playlist, volume, vitesse, sortie)
 *   POST /api/cmd     → corps = play|stop|next|volup|voldown|speedup|speeddown
 *   GET  /stream      → flux audio WAV (PCM 16 bits stéréo 44,1 kHz)
 *
 * Le flux /stream est servi par un thread par connexion : il lit le ring
 * de diffusion du moteur (mp_web_read) au rythme du décodage, applique le
 * volume courant et convertit en int16.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "player.h"
#include "server.h"

/* interface playlist exposée par main.c */
extern int  web_plist_count(void);
extern const wchar_t* web_plist_name(int i);
extern int  web_plist_index(void);
extern void web_plist_next(void);
extern void web_playlist_play(int i);
extern void web_shuffle_toggle(void);
extern int  web_plist_shuffle(void);
extern void web_set_audio_out(int mode);

static volatile LONG g_running = 0;
static SOCKET g_listen = INVALID_SOCKET;
static HWND   g_hwnd = NULL;
static int    g_port = 8000;

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

static void http_response(SOCKET s, const char* status, const char* type,
                          const char* body)
{
    char hdr[512];
    int hl = _snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
        "Content-Length: %d\r\nCache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n",
        status, type, (int)strlen(body));
    send_all(s, hdr, hl);
    send_all(s, body, (int)strlen(body));
}

/* Conversion wchar_t → UTF-8 avec échappement JSON (guillemets, \ et <0x20) */
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

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */
static void api_state(SOCKET s)
{
    char body[9000];
    char items[6500] = "";
    int n = web_plist_count();
    int idx = web_plist_index();
    if (n > 100) n = 100;
    for (int i = 0; i < n; i++) {
        char name[600];
        json_escape(web_plist_name(i), name, sizeof(name));
        char tmp[700];
        _snprintf(tmp, sizeof(tmp), "%s\"%s\"", i ? "," : "", name);
        if (strlen(items) + strlen(tmp) < sizeof(items) - 4) strcat(items, tmp);
    }
    const char* st = "stopped";
    mp_state ms = mp_get_state();
    if (ms == MP_STATE_PLAYING) st = "playing";
    else if (ms == MP_STATE_PAUSED) st = "paused";
    const char* ao = "pc";
    int a = mp_get_audio_out();
    if (a == 1) ao = "phone";
    else if (a == 2) ao = "both";
    char name[600];
    if (idx >= 0 && idx < n) json_escape(web_plist_name(idx), name, sizeof(name));
    else name[0] = 0;
    _snprintf(body, sizeof(body),
        "{\"state\":\"%s\",\"idx\":%d,\"count\":%d,\"vol\":%.2f,\"speed\":%.2f,"
        "\"audio\":\"%s\",\"shuffle\":%d,\"name\":\"%s\",\"items\":[%s]}",
        st, idx, n, mp_get_volume(), mp_get_speed(), ao, web_plist_shuffle(),
        name, items);
    http_response(s, "200 OK", "application/json", body);
}

static void api_cmd(SOCKET s, const char* cmd)
{
    if (!strcmp(cmd, "play"))          mp_play_pause();
    else if (!strcmp(cmd, "stop"))     mp_stop();
    else if (!strcmp(cmd, "next"))     web_plist_next();
    else if (!strcmp(cmd, "volup"))    mp_set_volume(mp_get_volume() + 0.1f);
    else if (!strcmp(cmd, "voldown"))  mp_set_volume(mp_get_volume() - 0.1f);
    else if (!strcmp(cmd, "speedup"))  mp_set_speed(mp_get_speed() * 1.1f);
    else if (!strcmp(cmd, "speeddown"))mp_set_speed(mp_get_speed() / 1.1f);
    else if (!strcmp(cmd, "audio"))
        web_set_audio_out((mp_get_audio_out() + 1) % 3);   /* cycle PC→tél→les 2 */
    else if (!strncmp(cmd, "playidx=", 8))
        web_playlist_play(atoi(cmd + 8));                  /* clic sur un morceau */
    else if (!strcmp(cmd, "shuffle"))
        web_shuffle_toggle();
    http_response(s, "200 OK", "text/plain", "ok");
}

/* ------------------------------------------------------------------ */
/* Page web (télécommande)                                             */
/* Icônes SVG inline : rendu identique sur tous les navigateurs.       */
/* Bouton "Son téléphone" dédié : le flux audio se lit indépendamment  */
/* du bouton lecture (qui contrôle l'application).                     */
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
"<div class=\"meta\" id=\"meta\">&hellip;</div>\n"
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
"    document.body.dataset.playing=s.state==='playing'?'1':'0';\n"
"    document.body.dataset.audio=s.audio;\n"
"    $('bPlay').innerHTML = s.state==='playing' ? ICON_PAUSE : ICON_PLAY;\n"
"    $('bAud').innerHTML = AUD_ICONS[s.audio] + '<span>' + AUD_LABELS[s.audio] + '</span>';\n"
"    $('bShuf').className = 'btn' + (s.shuffle ? ' on' : '');\n"
"    $('meta').textContent=(s.state==='playing'?'Playing':'Paused')+' &middot; vol '+Math.round(s.vol*100)+'% &middot; &times;'+s.speed.toFixed(2)+' &mdash; '+(s.name||'no file');\n"
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
/* Flux audio /stream                                                  */
/* ------------------------------------------------------------------ */
static DWORD WINAPI stream_thread(void* arg)
{
    SOCKET c = (SOCKET)(intptr_t)arg;
    const char* hdr =
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
    send_all(c, hdr, (int)strlen(hdr));

    /* entête RIFF/WAVE : PCM 16 bits stéréo 44100 Hz (tailles "infinies") */
    uint8_t wav[44] = {
        'R','I','F','F', 0xff,0xff,0xff,0x7f, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x44,0xac,0,0, 0x10,0xb1,0x02,0, 4,0, 16,0,
        'd','a','t','a', 0xff,0xff,0xff,0x7f
    };
    send_all(c, (const char*)wav, 44);

    static float fbuf[1024 * 2];
    static uint8_t obuf[4096];
    uint32_t idle = 0;
    float vol = 1.0f;
    while (g_running) {
        uint32_t n = mp_web_read(fbuf, 1024);
        if (n == 0) {
            if (++idle > 50) {           /* ~1 s sans données : silence */
                memset(obuf, 0, sizeof(obuf));
                if (send(c, (const char*)obuf, sizeof(obuf), 0) <= 0) break;
                idle = 0;
            }
            Sleep(20);
            continue;
        }
        idle = 0;
        vol = mp_get_volume();
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
    return 0;
}

/* ------------------------------------------------------------------ */
/* Thread principal : accept + dispatch                                */
/* ------------------------------------------------------------------ */
static DWORD WINAPI server_thread(void* arg)
{
    (void)arg;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        InterlockedExchange(&g_running, 0);
        return 1;
    }
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == INVALID_SOCKET) { WSACleanup(); InterlockedExchange(&g_running, 0); return 1; }
    int yes = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)g_port);
    if (bind(g_listen, (struct sockaddr*)&a, sizeof(a)) != 0 ||
        listen(g_listen, 8) != 0) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        InterlockedExchange(&g_running, 0);
        return 1;
    }
    while (g_running) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        char req[8192];
        int rn = recv(c, req, sizeof(req) - 1, 0);
        if (rn <= 0) { closesocket(c); continue; }
        req[rn] = 0;
        char method[8], path[512];
        if (sscanf(req, "%7s %511s", method, path) != 2) {
            closesocket(c);
            continue;
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
        } else if (!strncmp(path, "/stream", 7)) {
            HANDLE h = CreateThread(NULL, 0, stream_thread, (void*)(intptr_t)c, 0, NULL);
            if (h) CloseHandle(h);
            else closesocket(c);
        } else {
            http_response(c, "404 Not Found", "text/plain", "not found");
            closesocket(c);
        }
    }
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    WSACleanup();
    return 0;
}

/* ------------------------------------------------------------------ */
/* API publique                                                        */
/* ------------------------------------------------------------------ */
int server_start(int port, HWND hwnd)
{
    if (g_running) server_stop();
    g_hwnd = hwnd;
    g_port = port;
    g_listen = INVALID_SOCKET;
    InterlockedExchange(&g_running, 1);
    HANDLE h = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    if (!h) {
        InterlockedExchange(&g_running, 0);
        return -1;
    }
    CloseHandle(h);
    /* attend que le bind soit fait (pour détecter un port occupé) */
    for (int i = 0; i < 150 && g_running; i++) {
        if (g_listen != INVALID_SOCKET) break;
        Sleep(20);
    }
    return g_listen != INVALID_SOCKET ? 0 : -1;
}

void server_stop(void)
{
    InterlockedExchange(&g_running, 0);
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    Sleep(120);
}

int server_is_running(void) { return (int)g_running; }

/* Premier port libre à partir de 8000 (utilisé comme port par défaut). */
int server_find_free_port(void)
{
    WSADATA wsa;
    int port = 8000;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 8000;
    for (int p = 8000; p < 65535; p++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) break;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((u_short)p);
        if (bind(s, (struct sockaddr*)&a, sizeof(a)) == 0) {
            port = p;
            closesocket(s);
            break;
        }
        closesocket(s);
    }
    WSACleanup();
    return port;
}
