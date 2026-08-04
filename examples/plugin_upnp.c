/*
 * DLNA/UPnP — plugin SERVICE
 * ==========================
 * Serveur média UPnP AV (DLNA) minimal :
 *   - découverte SSDP (M-SEARCH + NOTIFY alive sur 239.255.255.250:1900)
 *   - description du device : GET /rootDesc.xml (port 8081)
 *   - ContentDirectory (Browse) : la playliste en DIDL-Lite
 *   - streaming : GET /media/N → WAV PCM 44,1 kHz stéréo
 *
 * Les appareils DLNA de la maison (TV, ampli, téléphone avec BubbleUPnP…)
 * découvrent "MusicPlayer" et peuvent lire les pistes de la playliste.
 * Activable dans Plugins ▸ Services ▸ DLNA/UPnP.
 */
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/plugin.h"

#define UPnP_PORT 8081
#define UDN       "uuid:6f1a2b3c-4d5e-4f60-9a1b-2c3d4e5f6071"

static const mp_host_api* g_h = NULL;
static volatile LONG g_running = 0;
static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_http_thread = NULL;
static SOCKET g_ssdp_sock = INVALID_SOCKET;
static HANDLE g_ssdp_thread = NULL;

static int net_port(void)
{
    return g_h && g_h->svc_port ? g_h->svc_port("upnp") : UPnP_PORT;
}

static void log_line(const char* msg)
{
    if (g_h && g_h->log) g_h->log(msg);
}

static void get_local_ip(char* ip, int max)
{
    /* première IP non bouclage via UDP connect */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    ip[0] = 0;
    if (s != INVALID_SOCKET) {
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = inet_addr("239.255.255.250");
        dst.sin_port = htons(1900);
        if (connect(s, (struct sockaddr*)&dst, sizeof(dst)) == 0) {
            struct sockaddr_in local;
            int len = sizeof(local);
            getsockname(s, (struct sockaddr*)&local, &len);
            snprintf(ip, max, "%s", inet_ntoa(local.sin_addr));
        }
        closesocket(s);
    }
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                 */
/* ------------------------------------------------------------------ */
static void http_headers(SOCKET c, int code, const char* ctype, int len)
{
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, code == 200 ? "OK" : "Not Found",
        ctype ? ctype : "text/xml", len);
    send(c, hdr, (int)strlen(hdr), 0);
}

static void send_body(SOCKET c, const char* body, const char* ctype)
{
    http_headers(c, 200, ctype, (int)strlen(body));
    send(c, body, (int)strlen(body), 0);
}

static void root_desc(SOCKET c)
{
    char ip[64];
    get_local_ip(ip, sizeof(ip));
    char xml[4096];
    snprintf(xml, sizeof(xml),
        "<?xml version=\"1.0\"?>\r\n"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
        "<specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
        "<device>\r\n"
        "<deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>\r\n"
        "<friendlyName>MusicPlayer</friendlyName>\r\n"
        "<manufacturer>LostInTheBugs</manufacturer>\r\n"
        "<modelName>MusicPlayer</modelName>\r\n"
        "<UDN>" UDN "</UDN>\r\n"
        "<serviceList><service>\r\n"
        "<serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>\r\n"
        "<serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId>\r\n"
        "<SCPDURL>/scpd.xml</SCPDURL>\r\n"
        "<controlURL>/ctl/ContentDirectory</controlURL>\r\n"
        "<eventSubURL>/evt/ContentDirectory</eventSubURL>\r\n"
        "</service></serviceList>\r\n"
        "</device>\r\n"
        "</root>\r\n", ip);
    send_body(c, xml, "text/xml");
}

static void scpd(SOCKET c)
{
    const char* xml =
        "<?xml version=\"1.0\"?>\r\n"
        "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
        "<actionList><action><name>Browse</name><argumentList>\r\n"
        "<argument><name>ObjectID</name><direction>in</direction>"
        "<relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>\r\n"
        "<argument><name>BrowseFlag</name><direction>in</direction>"
        "<relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument>\r\n"
        "<argument><name>Filter</name><direction>in</direction>"
        "<relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument>\r\n"
        "<argument><name>StartingIndex</name><direction>in</direction>"
        "<relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument>\r\n"
        "<argument><name>RequestedCount</name><direction>in</direction>"
        "<relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>\r\n"
        "<argument><name>SortCriteria</name><direction>in</direction>"
        "<relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument>\r\n"
        "<argument><name>Result</name><direction>out</direction>"
        "<relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>\r\n"
        "<argument><name>NumberReturned</name><direction>out</direction>"
        "<relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>\r\n"
        "<argument><name>TotalMatches</name><direction>out</direction>"
        "<relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>\r\n"
        "<argument><name>UpdateID</name><direction>out</direction>"
        "<relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument>\r\n"
        "</argumentList></action></actionList>\r\n"
        "<serviceStateTable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ObjectID</name>"
        "<dataType>string</dataType></stateVariable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_BrowseFlag</name>"
        "<dataType>string</dataType></stateVariable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Filter</name>"
        "<dataType>string</dataType></stateVariable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Index</name>"
        "<dataType>ui4</dataType></stateVariable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Count</name>"
        "<dataType>ui4</dataType></stateVariable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SortCriteria</name>"
        "<dataType>string</dataType></stateVariable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Result</name>"
        "<dataType>string</dataType></stateVariable>\r\n"
        "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_UpdateID</name>"
        "<dataType>ui4</dataType></stateVariable>\r\n"
        "</serviceStateTable>\r\n"
        "</scpd>\r\n";
    send_body(c, xml, "text/xml");
}

static void xml_escape(const wchar_t* in, char* out, int max)
{
    int o = 0;
    for (int i = 0; in[i] && o < max - 16; i++) {
        if (in[i] == L'&')      { memcpy(out + o, "&amp;", 5); o += 5; }
        else if (in[i] == L'<') { memcpy(out + o, "&lt;", 4); o += 4; }
        else if (in[i] == L'>') { memcpy(out + o, "&gt;", 4); o += 4; }
        else if (in[i] == L'"') { memcpy(out + o, "&quot;", 6); o += 6; }
        else if (in[i] < 128)   { out[o++] = (char)in[i]; }
        else                    { o += snprintf(out + o, max - o, "&#%u;",
                                                 (unsigned)in[i]); }
    }
    out[o] = 0;
}

static void soap_browse(SOCKET c)
{
    char ip[64];
    get_local_ip(ip, sizeof(ip));
    char didl[16384];
    int count = g_h->plist_count();
    int off = 0;
    for (int i = 0; i < count && off < (int)sizeof(didl) - 512; i++) {
        const wchar_t* p = g_h->plist_path(i);
        const wchar_t* base = p;
        const wchar_t* bs = wcsrchr(p, L'\\');
        const wchar_t* fs = wcsrchr(p, L'/');
        if (bs && (!fs || bs > fs)) base = bs + 1;
        else if (fs) base = fs + 1;
        char t[256];
        xml_escape(base, t, sizeof(t));
        off += snprintf(didl + off, sizeof(didl) - off,
            "<item id=\"%d\" parentID=\"0\" restricted=\"1\">"
            "<dc:title>%s</dc:title>"
            "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
            "<res protocolInfo=\"http-get:*:audio/x-wav:*\">"
            "http://%s:%d/media/%d</res>"
            "</item>", i + 1, t, ip, net_port(), i);
    }
    didl[off] = 0;
    char body[17000];
    snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:BrowseResponse "
        "xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
        "<Result>&lt;DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\"&gt;"
        "%s&lt;/DIDL-Lite&gt;</Result>"
        "<NumberReturned>%d</NumberReturned><TotalMatches>%d</TotalMatches>"
        "<UpdateID>0</UpdateID>"
        "</u:BrowseResponse></s:Body></s:Envelope>", didl, count, count);
    send_body(c, body, "text/xml");
}

static void media_stream(SOCKET c, int idx)
{
    if (idx >= 0 && idx < g_h->plist_count())
        g_h->plist_play(idx);   /* la piste demandée devient le flux */
    /* WAV 44,1 kHz stéréo 16 bits */
    unsigned char hdr[44] = { 0 };
    unsigned rate = 44100;
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16;
    hdr[20] = 1; hdr[21] = 0;
    hdr[22] = 2; hdr[23] = 0;
    hdr[24] = rate & 0xff; hdr[25] = (rate >> 8) & 0xff;
    hdr[26] = (rate >> 16) & 0xff; hdr[27] = (rate >> 24) & 0xff;
    unsigned bps = rate * 4;
    hdr[28] = bps & 0xff; hdr[29] = (bps >> 8) & 0xff;
    hdr[30] = (bps >> 16) & 0xff; hdr[31] = (bps >> 24) & 0xff;
    hdr[32] = 4;
    hdr[34] = 16;
    memcpy(hdr + 36, "data", 4);
    /* header sans Content-Length connue : on envoie en flux continu */
    const char* h =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/x-wav\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    send(c, h, (int)strlen(h), 0);
    /* tampons propres à CE client (un client_thread par connexion) */
    float* fbuf = (float*)malloc(1024 * 2 * sizeof(float));
    unsigned char* obuf = (unsigned char*)malloc(4096);
    if (!fbuf || !obuf) {
        free(fbuf);
        free(obuf);
        return;
    }
    while (g_running) {
        uint32_t n = g_h->web_read(fbuf, 1024);
        if (!n) { Sleep(20); continue; }
        for (uint32_t i = 0; i < n * 2; i++) {
            float v = fbuf[i];
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            short s16 = (short)(v * 32767.0f);
            obuf[i * 2] = (unsigned char)(s16 & 0xff);
            obuf[i * 2 + 1] = (unsigned char)((s16 >> 8) & 0xff);
        }
        char lenstr[32];
        snprintf(lenstr, sizeof(lenstr), "%x\r\n", (int)(n * 4));
        send(c, lenstr, (int)strlen(lenstr), 0);
        if (send(c, (const char*)obuf, (int)(n * 4), 0) <= 0) break;
        send(c, "\r\n", 2, 0);
    }
    free(fbuf);
    free(obuf);
    send(c, "0\r\n\r\n", 5, 0);
}

static void handle_client(SOCKET c)
{
    char req[8192];
    int got = 0;
    while (got < (int)sizeof(req) - 1) {
        int n = recv(c, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0) break;
        got += n;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[got] = 0;
    if (got < 8) { closesocket(c); return; }
    char method[16] = "", path[1024] = "";
    sscanf(req, "%15s %1023s", method, path);
    if (!strcmp(path, "/rootDesc.xml")) {
        root_desc(c);
    } else if (!strcmp(path, "/scpd.xml")) {
        scpd(c);
    } else if (!strcmp(path, "/ctl/ContentDirectory")) {
        soap_browse(c);
    } else if (!strncmp(path, "/media/", 7)) {
        int idx = atoi(path + 7);
        media_stream(c, idx);
    } else {
        http_headers(c, 404, "text/xml", 0);
    }
    closesocket(c);
}

static DWORD WINAPI client_thread(LPVOID arg)
{
    SOCKET c = (SOCKET)(SIZE_T)arg;
    handle_client(c);
    return 0;
}

static DWORD WINAPI http_loop(LPVOID arg)
{
    (void)arg;
    while (g_running) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        HANDLE t = CreateThread(NULL, 0, client_thread, (LPVOID)(SIZE_T)c, 0, NULL);
        if (t) CloseHandle(t);
        else closesocket(c);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SSDP                                                                 */
/* ------------------------------------------------------------------ */
static void ssdp_reply(const char* st, const char* usn)
{
    char ip[64];
    get_local_ip(ip, sizeof(ip));
    char msg[1024];
    snprintf(msg, sizeof(msg),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "DATE: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s:%d/rootDesc.xml\r\n"
        "SERVER: MusicPlayer/1.0 UPnP/1.0 DLNADOC/1.50\r\n"
        "ST: %s\r\n"
        "USN: %s\r\n"
        "\r\n", ip, net_port(), st, usn);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    dst.sin_addr.s_addr = inet_addr("239.255.255.250");
    sendto(g_ssdp_sock, msg, (int)strlen(msg), 0,
           (struct sockaddr*)&dst, sizeof(dst));
}

static void ssdp_notify(const char* nts)
{
    char ip[64];
    get_local_ip(ip, sizeof(ip));
    char msg[1024];
    snprintf(msg, sizeof(msg),
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: http://%s:%d/rootDesc.xml\r\n"
        "NT: %s\r\n"
        "NTS: %s\r\n"
        "SERVER: MusicPlayer/1.0 UPnP/1.0 DLNADOC/1.50\r\n"
        "USN: %s\r\n"
        "\r\n", ip, net_port(),
        "upnp:rootdevice", nts,
        UDN "::upnp:rootdevice");
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    dst.sin_addr.s_addr = inet_addr("239.255.255.250");
    sendto(g_ssdp_sock, msg, (int)strlen(msg), 0,
           (struct sockaddr*)&dst, sizeof(dst));
}

static DWORD WINAPI ssdp_loop(LPVOID arg)
{
    (void)arg;
    g_ssdp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_ssdp_sock == INVALID_SOCKET) return 0;
    BOOL reuse = TRUE;
    setsockopt(g_ssdp_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1900);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_ssdp_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        /* bind échoué (autre client UPnP) : on envoie quand même */
    }
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(g_ssdp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               (const char*)&mreq, sizeof(mreq));

    log_line("UPnP: SSDP active (239.255.255.250:1900)");
    unsigned last_alive = 0;
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_ssdp_sock, &rfds);
        struct timeval tv = { 1, 0 };
        int r = select(0, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            char buf[2048];
            struct sockaddr_in src;
            int slen = sizeof(src);
            int n = recvfrom(g_ssdp_sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr*)&src, &slen);
            if (n > 0) {
                buf[n] = 0;
                if (strstr(buf, "M-SEARCH")) {
                    ssdp_reply("upnp:rootdevice", UDN "::upnp:rootdevice");
                    ssdp_reply("urn:schemas-upnp-org:device:MediaServer:1",
                               UDN "::urn:schemas-upnp-org:device:MediaServer:1");
                    ssdp_reply("urn:schemas-upnp-org:service:ContentDirectory:1",
                               UDN "::urn:schemas-upnp-org:service:ContentDirectory:1");
                    ssdp_reply("ssdp:all", UDN "::upnp:rootdevice");
                }
            }
        }
        unsigned now = GetTickCount() / 1000;
        if (now - last_alive >= 30) {
            last_alive = now;
            ssdp_notify("ssdp:alive");
        }
    }
    closesocket(g_ssdp_sock);
    g_ssdp_sock = INVALID_SOCKET;
    return 0;
}

/* ------------------------------------------------------------------ */
static int upnp_start(void)
{
    if (g_running) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { WSACleanup(); return -1; }
    int yes = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    /* IP configurée (Settings ▸ Network…) : sinon toutes les interfaces */
    const char* ips = g_h->svc_ips ? g_h->svc_ips("upnp") : "";
    if (ips && ips[0]) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s", ips);
        char* tok = strtok(tmp, ";");
        addr.sin_addr.s_addr = inet_addr(tok ? tok : "0.0.0.0");
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    addr.sin_port = htons((unsigned short)net_port());
    if (bind(g_listen, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }
    listen(g_listen, 8);
    InterlockedExchange(&g_running, 1);
    g_http_thread = CreateThread(NULL, 0, http_loop, NULL, 0, NULL);
    g_ssdp_thread = CreateThread(NULL, 0, ssdp_loop, NULL, 0, NULL);
    return 0;
}

static void upnp_stop(void)
{
    if (!g_running) return;
    InterlockedExchange(&g_running, 0);
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    if (g_ssdp_sock != INVALID_SOCKET) {
        closesocket(g_ssdp_sock);
        g_ssdp_sock = INVALID_SOCKET;
    }
    if (g_http_thread) {
        WaitForSingleObject(g_http_thread, 2000);
        CloseHandle(g_http_thread);
        g_http_thread = NULL;
    }
    if (g_ssdp_thread) {
        WaitForSingleObject(g_ssdp_thread, 2000);
        CloseHandle(g_ssdp_thread);
        g_ssdp_thread = NULL;
    }
    WSACleanup();
}

/* ------------------------------------------------------------------ */
static const char* pl_name(void) { return "DLNA/UPnP Media Server"; }
static const char* pl_version(void) { return "1.0"; }
static const char* pl_description(void)
{ return "UPnP AV media server (SSDP + ContentDirectory) exposing the playlist to DLNA devices"; }
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
    upnp_stop();
    g_h = NULL;
}

static void pl_service(mp_plugin* self, int event, void* data)
{
    (void)data;
    if (!g_h) return;
    if (event != MP_SERVICE_WEB_APPLY && event != MP_SERVICE_CLICK) return;
    if (self->enabled) {
        if (!g_running) {
            if (upnp_start() == 0) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                          "DLNA/UPnP: server on port %d (MusicPlayer)",
                          net_port());
                log_line(msg);
            } else {
                log_line("DLNA/UPnP: port unavailable");
            }
        }
    } else {
        upnp_stop();
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
