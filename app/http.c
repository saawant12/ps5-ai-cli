/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int ui_write(int fd, const void *data, size_t size) {
    const unsigned char *p = data;
    while (size) {
        ssize_t n = send(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

void ui_socket_options(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    /* BSD accept can inherit O_NONBLOCK from the recovering listener. */
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    int yes = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#else
    (void)yes;
#endif
    struct timeval timeout = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int store_header(char *dest, size_t capacity, const char *value) {
    size_t length = strlen(value);
    if (dest[0] || length >= capacity) return -1;
    memcpy(dest, value, length + 1);
    return 0;
}

int ui_read_request(int fd, struct ui_request *r) {
    char buffer[8192];
    size_t used = 0;
    time_t deadline = time(NULL) + 8;
    /* Read exactly through the headers: no first WebSocket frame is lost. */
    while (used < sizeof(buffer) - 1 && time(NULL) <= deadline) {
        ssize_t n = recv(fd, buffer + used, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) return -1;
        if (!buffer[used]) return -1;
        used++;
        if (used >= 4 && !memcmp(buffer + used - 4, "\r\n\r\n", 4)) break;
    }
    if (used < 4 || memcmp(buffer + used - 4, "\r\n\r\n", 4)) return -1;
    buffer[used] = 0;
    char *line = strstr(buffer, "\r\n");
    *line = 0;
    char protocol[16], tail;
    if (sscanf(buffer, "%11s %255s %15s %c", r->method, r->path, protocol, &tail) != 3 ||
        strcmp(protocol, "HTTP/1.1") || r->path[0] != '/') return -1;
    int have_length = 0, have_version = 0;
    for (line += 2; *line != '\r';) {
        char *next = strstr(line, "\r\n");
        if (!next || line[0] == ' ' || line[0] == '\t') return -1;
        *next = 0;
        char *value = strchr(line, ':');
        if (!value) return -1;
        *value++ = 0;
        for (char *p = line; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') return -1;
        while (*value == ' ' || *value == '\t') value++;
        for (char *p = value; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p == 127) return -1;
        size_t len = strlen(value);
        while (len && value[len - 1] == ' ') value[--len] = 0;
#define HEADER(name, field) if (!strcasecmp(line, name)) { if (store_header(r->field, sizeof(r->field), value)) return -1; }
        HEADER("Host", host)
        else HEADER("Origin", origin)
        else HEADER("Cookie", cookie)
        else HEADER("Sec-WebSocket-Key", key)
        else HEADER("Upgrade", upgrade)
        else HEADER("Connection", connection)
        else HEADER("X-PS5-Client", client)
        else if (!strcasecmp(line, "Sec-WebSocket-Version")) {
            if (have_version++ || strcmp(value, "13")) return -1;
            r->version = 13;
        } else if (!strcasecmp(line, "Content-Length")) {
            if (have_length++ || !*value) return -1;
            for (char *p = value; *p; p++) if (!isdigit((unsigned char)*p)) return -1;
            unsigned long n = strtoul(value, NULL, 10);
            if (n >= sizeof(r->body)) return -1;
            r->length = n;
        } else if (!strcasecmp(line, "Transfer-Encoding")) return -1;
#undef HEADER
        line = next + 2;
    }
    if (!r->host[0]) return -1;
    used = 0;
    while (used < r->length && time(NULL) <= deadline) {
        ssize_t n = recv(fd, r->body + used, r->length - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += n;
    }
    if (used != r->length || memchr(r->body, 0, r->length)) return -1;
    r->body[r->length] = 0;
    return 0;
}

void ui_reply(int fd, int status, const char *type, const void *body, size_t length, const char *extra) {
    char header[2048];
    const char *reason = status == 200 ? "OK" : status == 401 ? "Unauthorized" :
        status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 429 ? "Too Many Requests" :
        status == 503 ? "Service Unavailable" : "Bad Request";
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
        "connect-src 'self'; img-src 'self' data:; font-src 'self'; object-src 'none'; "
        "base-uri 'none'; frame-ancestors 'none'; form-action 'self'\r\n%s\r\n",
        status, reason, type, length, extra ? extra : "");
    if (n > 0 && (size_t)n < sizeof(header) && !ui_write(fd, header, n)) ui_write(fd, body, length);
}

int ui_host_allowed(int fd, const char *host, unsigned short port) {
    struct sockaddr_in local;
    socklen_t size = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &size)) return 0;
    char address[INET_ADDRSTRLEN], expected[128];
    if (!inet_ntop(AF_INET, &local.sin_addr, address, sizeof(address))) return 0;
    snprintf(expected, sizeof(expected), "%s:%u", address, port);
    if (!strcmp(expected, host)) return 1;
    if ((ntohl(local.sin_addr.s_addr) >> 24) == 127) {
        snprintf(expected, sizeof(expected), "localhost:%u", port);
        return !strcasecmp(host, expected);
    }
    return 0;
}

int ui_same_origin(const struct ui_request *r) {
    char origin[160];
    snprintf(origin, sizeof(origin), "http://%s", r->host);
    return r->origin[0] && !strcmp(origin, r->origin);
}

int ui_connect(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    ui_socket_options(fd);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) { close(fd); return -1; }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) && errno != EINPROGRESS) {
        close(fd); return -1;
    }
    struct pollfd p = {.fd = fd, .events = POLLOUT};
    int error = 0;
    socklen_t size = sizeof(error);
    if (poll(&p, 1, 1500) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error ||
        fcntl(fd, F_SETFL, flags)) { close(fd); return -1; }
    return fd;
}
