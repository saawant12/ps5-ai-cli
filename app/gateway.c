/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gateway.h"
#include "config.h"
#include "websocket.h"
#include "http.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef PS5_UI_TESTING
#include <signal.h>
static volatile sig_atomic_t test_drop_listener;
void ps5_ui_test_drop_listener(void) { test_drop_listener = 1; }
#endif
#include "../build/ui-assets.h"

#define CLIENT_LIMIT 16
#define SESSION_LIMIT 16
struct session { char token[65]; time_t expires; };
static struct {
    unsigned short port;
    struct ui_config config;
    char pair_code[16];
    time_t pair_deadline, blocked_until;
    unsigned failures, clients;
    int listener, fixture;
    pthread_mutex_t lock;
    struct session sessions[SESSION_LIMIT];
} server = {.lock = PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t terminal_lock = PTHREAD_MUTEX_INITIALIZER;
static int terminal_client = -1, restarting;

static int equal(const char *a, const char *b, size_t length) {
    unsigned char difference = 0;
    for (size_t i = 0; i < length; i++) difference |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return difference == 0;
}

static void random_token(char out[65]) {
    unsigned char bytes[32];
    arc4random_buf(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) snprintf(out + i * 2, 3, "%02x", bytes[i]);
}

static int session_index(const char *cookie) {
    const char *p = cookie;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 78 && !strncmp(p, "PS5AI_SESSION=", 14)) {
            for (int i = 0; i < SESSION_LIMIT; i++) {
                if (server.sessions[i].expires > time(NULL) && equal(p + 14, server.sessions[i].token, 64)) return i;
            }
        }
        if (!end) break;
        p = end + 1;
    }
    return -1;
}

static void json_reply(int fd, int status, const char *body) {
    ui_reply(fd, status, "application/json", body, strlen(body), NULL);
}

static int local_peer(int fd) {
    struct sockaddr_in peer = {0};
    socklen_t size = sizeof(peer);
    return !getpeername(fd, (struct sockaddr *)&peer, &size) &&
        peer.sin_family == AF_INET && ntohl(peer.sin_addr.s_addr) == INADDR_LOOPBACK;
}

static void pair(int fd, const struct ui_request *r, int local_console) {
    if (!ui_same_origin(r) || strcmp(r->client, "1")) {
        json_reply(fd, 403, "{\"error\":\"Open this page directly on your console's address.\"}"); return;
    }
    if (local_console) {
        /* A URL query or Host header cannot grant local-console access. */
        if (r->length || !local_peer(fd)) {
            json_reply(fd, 403, "{\"error\":\"Use the pairing code on remote devices.\"}"); return;
        }
    }
    pthread_mutex_lock(&server.lock);
    time_t now = time(NULL);
    int status = 200;
    const char *body = "{\"paired\":true}";
    char cookie[256] = {0};
    if (!local_console && now < server.blocked_until) {
        status = 429; body = "{\"error\":\"Too many attempts. Wait 30 seconds and try again.\"}";
    } else if (!local_console && now > server.pair_deadline) {
        status = 403; body = "{\"error\":\"Pairing expired. Open Pair device on the PS5 for a new code.\"}";
    } else if (!local_console && (r->length != strlen(server.pair_code) || !equal(r->body, server.pair_code, r->length))) {
        status = 401; body = "{\"error\":\"That pairing code is incorrect. Check Pair device on the PS5.\"}";
        if (++server.failures >= 5) { server.blocked_until = now + 30; server.failures = 0; }
    } else {
        int slot = -1;
        for (int i = 0; i < SESSION_LIMIT; i++) if (server.sessions[i].expires <= now) { slot = i; break; }
        if (slot < 0) { status = 503; body = "{\"error\":\"All device slots are in use. Unpair a device first.\"}"; }
        else {
            random_token(server.sessions[slot].token);
            server.sessions[slot].expires = now + 8 * 60 * 60;
            server.failures = 0;
            snprintf(cookie, sizeof(cookie), "Set-Cookie: PS5AI_SESSION=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=28800\r\n", server.sessions[slot].token);
        }
    }
    pthread_mutex_unlock(&server.lock);
    ui_reply(fd, status, "application/json", body, strlen(body), cookie);
}

static int websocket_key_valid(const char *key) {
    if (strlen(key) != 24 || strcmp(key + 22, "==")) return 0;
    for (size_t i = 0; i < 22; i++) if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", key[i])) return 0;
    return 1;
}

static int connection_upgrade(const char *value) {
    while (*value) {
        while (*value == ' ' || *value == '\t' || *value == ',') value++;
        const char *end = strchr(value, ',');
        if (!end) end = value + strlen(value);
        const char *trimmed = end;
        while (trimmed > value && (trimmed[-1] == ' ' || trimmed[-1] == '\t')) trimmed--;
        if (trimmed - value == 7 && !strncasecmp(value, "upgrade", 7)) return 1;
        value = *end ? end + 1 : end;
    }
    return 0;
}

static void cli_closed(int client) {
    const unsigned char normal_close[] = {3, 232}; /* 1000: CLI ended. */
    terminal_frame(client, 8, normal_close, sizeof(normal_close));
}

static void relay(int client, const struct ui_request *r, int session, unsigned cols, unsigned rows) {
    if (strcmp(r->method, "GET") || !ui_same_origin(r) || r->length ||
        strcasecmp(r->upgrade, "websocket") || r->version != 13 || !websocket_key_valid(r->key) ||
        !connection_upgrade(r->connection) || cols < 20 || cols > 500 || rows < 5 || rows > 200) {
        json_reply(client, 403, "{\"error\":\"Invalid terminal connection.\"}"); return;
    }
    pthread_mutex_lock(&terminal_lock);
    int terminal = restarting ? -1 : server.config.attach(cols, rows);
    if (terminal >= 0) terminal_client = client;
    pthread_mutex_unlock(&terminal_lock);
    if (terminal < 0) {
        json_reply(client, 503, "{\"error\":\"Terminal unavailable or already controlled by another device.\"}"); return;
    }
    if (terminal_upgrade(client, r)) {
        close(terminal);
        pthread_mutex_lock(&terminal_lock);
        terminal_client = -1; server.config.detach();
        pthread_mutex_unlock(&terminal_lock);
        return;
    }
    unsigned char data[16384];
    char token[65];
    pthread_mutex_lock(&server.lock);
    memcpy(token, server.sessions[session].token, sizeof(token));
    pthread_mutex_unlock(&server.lock);
    struct pollfd sockets[2] = {{.fd = client, .events = POLLIN}, {.fd = terminal, .events = POLLIN}};
    for (;;) {
        pthread_mutex_lock(&server.lock);
        int active = server.sessions[session].expires > time(NULL) && equal(token, server.sessions[session].token, 64);
        pthread_mutex_unlock(&server.lock);
        if (!active) break;
        int ready = poll(sockets, 2, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (sockets[0].revents & POLLIN) {
            size_t length = 0;
            int opcode = terminal_read_frame(client, data, sizeof(data) - 1, &length);
            if (opcode < 0) break;
            if (opcode == 8) { terminal_frame(client, 8, data, length); break; }
            if (opcode == 9) { if (terminal_frame(client, 10, data, length)) break; }
            else if (opcode == 2) {
                size_t sent = 0;
                while (sent < length) {
                    struct pollfd writable = {.fd = terminal, .events = POLLOUT};
                    if (poll(&writable, 1, 1000) <= 0) break;
                    ssize_t n = write(terminal, data + sent, length - sent);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    sent += (size_t)n;
                }
                if (sent != length) break;
            } else if (opcode == 1) {
                unsigned columns, lines; char tail;
                data[length] = 0;
                if (memchr(data, 0, length) || sscanf((char *)data, "resize:%u:%u%c", &columns, &lines, &tail) != 2 ||
                    columns < 20 || columns > 500 || lines < 5 || lines > 200) break;
                server.config.resize(columns, lines);
            }
        }
        if (sockets[1].revents & POLLIN) {
            ssize_t n = read(terminal, data, sizeof(data));
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n <= 0) { cli_closed(client); break; }
            if (terminal_frame(client, 2, data, (size_t)n)) break;
        }
        if (sockets[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
        /* Drain output before closing: a PTY may report readable data and HUP
         * together, then return EIO instead of EOF after its child exits. */
        if (!(sockets[1].revents & POLLIN) &&
            (sockets[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            cli_closed(client); break;
        }
    }
    close(terminal);
    pthread_mutex_lock(&terminal_lock);
    terminal_client = -1;
    server.config.detach();
    pthread_mutex_unlock(&terminal_lock);
}

static void handle(int fd) {
    struct ui_request r = {0};
    if (ui_read_request(fd, &r)) { json_reply(fd, 400, "{\"error\":\"Invalid request.\"}"); return; }
    if (!ui_host_allowed(fd, r.host, server.port)) { json_reply(fd, 403, "{\"error\":\"Use the console's IP address.\"}"); return; }
    if (!strcmp(r.method, "POST") && (!strcmp(r.path, "/api/pair") || !strcmp(r.path, "/api/pair-local"))) {
        pair(fd, &r, !strcmp(r.path, "/api/pair-local")); return;
    }
    pthread_mutex_lock(&server.lock);
    int session = session_index(r.cookie);
    pthread_mutex_unlock(&server.lock);
    if (!strcmp(r.path, "/api/pairing-code") && !strcmp(r.method, "POST")) {
        if (session < 0 || !ui_same_origin(&r) || strcmp(r.client, "1") || r.length || !local_peer(fd)) {
            json_reply(fd, 403, "{\"error\":\"Open Pair device in the PS5 home-screen app.\"}"); return;
        }
        char data[80];
        pthread_mutex_lock(&server.lock);
        snprintf(server.pair_code, sizeof(server.pair_code), "%06u", arc4random_uniform(1000000));
        server.pair_deadline = time(NULL) + 15 * 60;
        server.failures = 0; server.blocked_until = 0;
        snprintf(data, sizeof(data), "{\"code\":\"%s\",\"expires_in\":900}", server.pair_code);
        pthread_mutex_unlock(&server.lock);
        json_reply(fd, 200, data); return;
    }
    if (!strcmp(r.path, "/api/cli/codex/restart") && !strcmp(r.method, "POST")) {
        if (session < 0 || !ui_same_origin(&r) || strcmp(r.client, "1") || r.length) {
            json_reply(fd, 403, "{\"error\":\"Pair this device and open the app directly before restarting.\"}"); return;
        }
        pthread_mutex_lock(&terminal_lock);
        if (restarting || !server.config.restart) {
            pthread_mutex_unlock(&terminal_lock);
            json_reply(fd, 503, "{\"error\":\"CLI restart is unavailable or already in progress.\"}"); return;
        }
        restarting = 1;
        if (terminal_client >= 0) shutdown(terminal_client, SHUT_RDWR);
        pthread_mutex_unlock(&terminal_lock);
        int result = server.config.restart(), error = errno;
        pthread_mutex_lock(&terminal_lock);
        restarting = 0;
        pthread_mutex_unlock(&terminal_lock);
        if (result) {
            char message[256];
            snprintf(message, sizeof(message), "{\"error\":\"Could not restart Codex: %s. Try Restart CLI again after checking the payload.\"}", strerror(error));
            json_reply(fd, 503, message);
        } else json_reply(fd, 200, "{\"restarted\":true}");
        return;
    }
    unsigned cols = 0, rows = 0; char tail;
    if (sscanf(r.path, "/terminal/codex?cols=%u&rows=%u%c", &cols, &rows, &tail) == 2) {
        if (session < 0) json_reply(fd, 401, "{\"error\":\"Pair this device first.\"}");
        else relay(fd, &r, session, cols, rows);
        return;
    }
    if (!strcmp(r.path, "/api/status") && !strcmp(r.method, "GET")) {
        char data[160];
        snprintf(data, sizeof(data), "{\"paired\":%s,\"fixture\":%s,\"version\":\"" PS5_AI_VERSION "\"}", session >= 0 ? "true" : "false", server.fixture ? "true" : "false");
        json_reply(fd, 200, data); return;
    }
    if (!strcmp(r.path, "/api/unpair") && !strcmp(r.method, "POST")) {
        if (!ui_same_origin(&r) || strcmp(r.client, "1") || session < 0) { json_reply(fd, 403, "{\"error\":\"Not paired.\"}"); return; }
        pthread_mutex_lock(&server.lock);
        memset(&server.sessions[session], 0, sizeof(server.sessions[session]));
        pthread_mutex_unlock(&server.lock);
        const char *body = "{\"paired\":false}";
        ui_reply(fd, 200, "application/json", body, strlen(body), "Set-Cookie: PS5AI_SESSION=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0\r\n"); return;
    }
    if (!strcmp(r.method, "GET") && !r.length) {
        const char *path = (!strcmp(r.path, "/") || !strcmp(r.path, "/?console=1")) ? "/index.html" : r.path;
        for (size_t i = 0; i < sizeof(ui_assets) / sizeof(ui_assets[0]); i++) {
            if (!strcmp(path, ui_assets[i].path)) {
                ui_reply(fd, 200, ui_assets[i].type, ui_assets[i].data, ui_assets[i].size, NULL); return;
            }
        }
    }
    json_reply(fd, 404, "{\"error\":\"Not found.\"}");
}

static void *client_main(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    handle(fd);
    close(fd);
    pthread_mutex_lock(&server.lock);
    server.clients--;
    pthread_mutex_unlock(&server.lock);
    return NULL;
}

static int start_client(pthread_t *thread, int *argument) {
    /* CLI installation and the native loader run from a client worker. The
     * console's default pthread stack is not a portable capacity guarantee. */
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error) return error;
    error = pthread_attr_setstacksize(&attributes, 1024 * 1024);
    if (!error) error = pthread_create(thread, &attributes, client_main, argument);
    pthread_attr_destroy(&attributes);
    return error;
}

static int open_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(server.port)};
    address.sin_addr.s_addr = htonl(server.config.loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK) ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 16)) {
        int error = errno; close(fd); errno = error; return -1;
    }
    return fd;
}

/* Network changes can leave a live descriptor that no longer accepts TCP.
 * A bounded loopback probe also detects that state when poll reports no error.
 * Resource exhaustion and a busy backlog are inconclusive, not restart reasons. */
static int listener_healthy(int fd) {
    int accepting = 0;
    socklen_t size = sizeof(accepting);
    int checked = getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &size);
    if ((!checked && !accepting) || (checked && (errno == EBADF || errno == ENOTSOCK))) return 0;
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0) return -1;
    if (fcntl(probe, F_SETFD, FD_CLOEXEC) || fcntl(probe, F_SETFL, O_NONBLOCK)) {
        close(probe); return -1;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(server.port)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int result = connect(probe, (struct sockaddr *)&address, sizeof(address));
    int error = result ? errno : 0;
    if (result && error == EINPROGRESS) {
        struct pollfd ready = {.fd = probe, .events = POLLOUT};
        result = poll(&ready, 1, 200);
        size = sizeof(error);
        if (result <= 0 || getsockopt(probe, SOL_SOCKET, SO_ERROR, &error, &size)) {
            close(probe); return -1;
        }
    }
    close(probe);
    if (!error) return 1;
    return error == ECONNREFUSED || error == ENETDOWN || error == ENETUNREACH ||
        error == EHOSTUNREACH || error == EINVAL || error == EBADF ? 0 : -1;
}

static void retire_listener(void) {
    close(server.listener);
    server.listener = -1;
    /* Wake a relay left behind by the old network. Its CLI and pairing remain. */
    pthread_mutex_lock(&terminal_lock);
    if (terminal_client >= 0) shutdown(terminal_client, SHUT_RDWR);
    pthread_mutex_unlock(&terminal_lock);
}

static void *accept_main(void *unused) {
    (void)unused;
    time_t next_probe = 0;
    for (;;) {
        if (server.listener < 0) {
            server.listener = open_listener();
            if (server.listener < 0) { poll(NULL, 0, 1000); continue; }
            next_probe = 0;
        }
#ifdef PS5_UI_TESTING
        if (test_drop_listener) {
            test_drop_listener = 0;
            /* Replace it with a valid socket that is no longer listening. */
            int broken = socket(AF_INET, SOCK_STREAM, 0);
            if (broken >= 0) { (void)dup2(broken, server.listener); close(broken); }
            next_probe = 0;
        }
#endif
        struct pollfd pending = {.fd = server.listener, .events = POLLIN};
        int ready = poll(&pending, 1, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (pending.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            retire_listener(); continue;
        }
        struct timespec clock;
        time_t now = clock_gettime(CLOCK_MONOTONIC, &clock) ? next_probe : clock.tv_sec;
        if (now >= next_probe) {
            next_probe = now + 5;
            if (listener_healthy(server.listener) == 0) { retire_listener(); continue; }
        }
        if (!ready) continue;
        int fd = accept(server.listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                poll(NULL, 0, 1000); continue;
            }
            retire_listener(); continue;
        }
        ui_socket_options(fd);
        pthread_mutex_lock(&server.lock);
        int full = server.clients >= CLIENT_LIMIT;
        if (!full) server.clients++;
        pthread_mutex_unlock(&server.lock);
        if (full) { json_reply(fd, 503, "{\"error\":\"Too many connections.\"}"); close(fd); continue; }
        int *argument = malloc(sizeof(int));
        pthread_t thread;
        if (argument) *argument = fd;
        if (!argument || start_client(&thread, argument)) {
            free(argument); close(fd);
            pthread_mutex_lock(&server.lock); server.clients--; pthread_mutex_unlock(&server.lock);
        } else pthread_detach(thread);
    }
    return NULL;
}

int ps5_ui_start(const struct ui_config *config) {
    if (!config || !config->pair_code || strlen(config->pair_code) != 6 || !config->attach || !config->resize || !config->detach) { errno = EINVAL; return -1; }
    server.port = config->port;
    server.config = *config;
    server.fixture = config->fixture;
    memcpy(server.pair_code, config->pair_code, 7);
    server.pair_deadline = time(NULL) + 15 * 60;
    server.listener = open_listener();
    if (server.listener < 0) return -1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, accept_main, NULL)) { close(server.listener); return -1; }
    pthread_detach(thread);
    return 0;
}
