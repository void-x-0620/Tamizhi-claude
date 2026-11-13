/* net.c — TCP sockets and a small HTTP/1.1 client (http only, no TLS) */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "tz_std.h"

/* ---- TCP ---- */
long long tz_tcp_connect(const char *host, long long port) {
    char ps[16]; snprintf(ps, sizeof ps, "%lld", port);
    struct addrinfo hints, *res, *a;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = -1;
    for (a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}
/* listen on every interface; port 0 picks a free one (see tz_net_port) */
long long tz_tcp_listen(long long port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 64) != 0) { close(fd); return -1; }
    return fd;
}
long long tz_net_port(long long fd) {
    struct sockaddr_in sa; socklen_t l = sizeof sa;
    if (getsockname((int)fd, (struct sockaddr *)&sa, &l) != 0) return -1;
    return ntohs(sa.sin_port);
}
long long tz_tcp_accept(long long fd) {
    int c;
    do c = accept((int)fd, NULL, NULL); while (c < 0 && errno == EINTR);
    return c;
}
long long tz_net_send(long long fd, const char *s) {
    size_t n = strlen(s), off = 0;
    while (off < n) {
        ssize_t w = send((int)fd, s + off, n - off, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return (long long)n;
}
/* up to max bytes; "" when the peer closed; illai on error (blocks until data arrives) */
const char *tz_net_recv(long long fd, long long max) {
    if (max < 1) max = 1;
    if (max > (1 << 24)) max = 1 << 24;
    char *b = (char *)malloc((size_t)max + 1);
    if (!b) exit(1);
    ssize_t r;
    do r = recv((int)fd, b, (size_t)max, 0); while (r < 0 && errno == EINTR);
    if (r < 0) { free(b); return NULL; }
    b[r] = '\0';
    return b;
}
/* one line without its line ending; "" at end of stream is indistinguishable from an empty line,
 * so net_recv_line returns illai when the peer has closed */
const char *tz_net_recv_line(long long fd) {
    size_t cap = 128, n = 0; char *b = (char *)malloc(cap);
    if (!b) exit(1);
    for (;;) {
        char c; ssize_t r = recv((int)fd, &c, 1, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (n == 0) { free(b); return NULL; } break; }
        if (c == '\n') break;
        if (n + 2 > cap) { cap *= 2; b = (char *)realloc(b, cap); if (!b) exit(1); }
        b[n++] = c;
    }
    if (n && b[n - 1] == '\r') n--;
    b[n] = '\0';
    return b;
}
long long tz_net_wait(long long fd, long long ms) {      /* 1 readable, 0 timeout */
    struct pollfd p = { (int)fd, POLLIN, 0 };
    int r = poll(&p, 1, (int)ms);
    return r > 0;
}
void tz_net_close(long long fd) { if (fd >= 0) close((int)fd); }
const char *tz_net_peer(long long fd) {                  /* "ip" of the connected peer */
    struct sockaddr_storage ss; socklen_t l = sizeof ss; char b[64] = "";
    if (getpeername((int)fd, (struct sockaddr *)&ss, &l) == 0) {
        if (ss.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, b, sizeof b);
        else if (ss.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, b, sizeof b);
    }
    return tz_dup(b, strlen(b));
}

/* ---- HTTP client ---- */
static long long g_status;
static char *g_headers;
long long tz_http_status(void) { return g_status; }
const char *tz_http_headers(void) { return g_headers ? g_headers : ""; }

/* Fetch one URL. Returns the body (malloc'd) or NULL; sets g_status and g_headers. */
static char *fetch(const char *method, const char *url, const char *body, const char *ctype,
                   const char *extra, char **location) {
    *location = NULL;
    if (strncasecmp(url, "http://", 7) != 0) return NULL;       /* no TLS */
    const char *hp = url + 7;
    const char *slash = strchr(hp, '/');
    size_t hl = slash ? (size_t)(slash - hp) : strlen(hp);
    char host[256], port[16] = "80";
    if (hl == 0 || hl >= sizeof host) return NULL;
    memcpy(host, hp, hl); host[hl] = '\0';
    char *colon = strrchr(host, ':');
    if (colon) { *colon = '\0'; snprintf(port, sizeof port, "%s", colon + 1); }
    const char *path = slash ? slash : "/";
    int fd = (int)tz_tcp_connect(host, atoll(port));
    if (fd < 0) return NULL;
    size_t bl = body ? strlen(body) : 0;
    size_t cap = strlen(path) + strlen(host) + (extra ? strlen(extra) : 0) + (ctype ? strlen(ctype) : 0) + 256;
    char *req = (char *)malloc(cap);
    if (!req) exit(1);
    int k = snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: tamizhi\r\nAccept: */*\r\nConnection: close\r\n",
                     method, path, host);
    if (body) k += snprintf(req + k, cap - (size_t)k, "Content-Type: %s\r\nContent-Length: %zu\r\n",
                            ctype && *ctype ? ctype : "text/plain", bl);
    if (extra && *extra) k += snprintf(req + k, cap - (size_t)k, "%s%s", extra,
                                       extra[strlen(extra) - 1] == '\n' ? "" : "\r\n");
    snprintf(req + k, cap - (size_t)k, "\r\n");
    int ok = tz_net_send(fd, req) >= 0 && (!body || tz_net_send(fd, body) >= 0);
    free(req);
    if (!ok) { close(fd); return NULL; }
    size_t rc = 1 << 14, rn = 0; char *resp = (char *)malloc(rc);
    if (!resp) exit(1);
    for (;;) {
        if (rn + 4096 + 1 > rc) { rc *= 2; resp = (char *)realloc(resp, rc); if (!resp) exit(1); }
        ssize_t r = recv(fd, resp + rn, rc - rn - 1, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        rn += (size_t)r;
        if (rn > (size_t)256 << 20) break;                       /* 256 MiB cap */
    }
    close(fd);
    resp[rn] = '\0';
    char *sep = strstr(resp, "\r\n\r\n");
    if (strncmp(resp, "HTTP/", 5) != 0 || !sep) { free(resp); return NULL; }
    g_status = atoll(strchr(resp, ' ') ? strchr(resp, ' ') + 1 : "0");
    free(g_headers);
    g_headers = tz_dup(resp, (size_t)(sep - resp));
    for (char *h = strchr(g_headers, '\n'); h; h = strchr(h + 1, '\n'))       /* location for redirects */
        if (!strncasecmp(h + 1, "location:", 9)) {
            const char *v = h + 10; while (*v == ' ') v++;
            size_t l = strcspn(v, "\r\n");
            *location = tz_dup(v, l);
        }
    char *b = sep + 4;
    int chunked = 0;
    for (char *h = strchr(g_headers, '\n'); h; h = strchr(h + 1, '\n'))
        if (!strncasecmp(h + 1, "transfer-encoding:", 18) && strcasestr(h, "chunked")) chunked = 1;
    if (!chunked) return tz_dup(b, (size_t)(resp + rn - b)) ;
    char *out = (char *)malloc((size_t)(resp + rn - b) + 1), *o = out;       /* decode chunked */
    if (!out) exit(1);
    while (b < resp + rn) {
        char *e; unsigned long n = strtoul(b, &e, 16);
        if (e == b) break;
        b = strstr(e, "\r\n"); if (!b) break;
        b += 2;
        if (n == 0) break;
        if (b + n > resp + rn) n = (unsigned long)(resp + rn - b);
        memcpy(o, b, n); o += n; b += n + 2;
    }
    *o = '\0';
    free(resp);
    return out;
}
static const char *request(const char *method, const char *url, const char *body,
                           const char *ctype, const char *extra) {
    char *cur = tz_dup(url, strlen(url));
    for (int hop = 0; hop < 6; hop++) {
        char *loc;
        char *r = fetch(method, cur, body, ctype, extra, &loc);
        if (!r) return NULL;
        if ((g_status == 301 || g_status == 302 || g_status == 303 || g_status == 307 || g_status == 308) && loc) {
            if (loc[0] == '/') {                                 /* relative redirect */
                const char *hp = cur + 7; const char *sl = strchr(hp, '/');
                size_t hl = sl ? (size_t)(sl - cur) : strlen(cur);
                char *n = (char *)malloc(hl + strlen(loc) + 1);
                if (!n) exit(1);
                memcpy(n, cur, hl); strcpy(n + hl, loc);
                loc = n;
            }
            cur = loc;
            if (g_status == 303) { method = "GET"; body = NULL; }
            continue;
        }
        return r;
    }
    return NULL;
}
const char *tz_http_get(const char *url) { return request("GET", url, NULL, NULL, NULL); }
const char *tz_http_post(const char *url, const char *body, const char *ctype) {
    return request("POST", url, body, ctype, NULL);
}
const char *tz_http_request(const char *method, const char *url, const char *body,
                            const char *ctype, const char *headers) {
    return request(method, url, body, ctype, headers);
}
