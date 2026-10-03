/* One-request, read-only diagnostic server. Serves only this project's logs. */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int send_bytes(int fd, const char *data, size_t size) {
    while (size) {
        ssize_t n = send(fd, data, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n;
        size -= (size_t)n;
    }
    return 0;
}

static int is_log_name(const char *name) {
    const char *prefixes[] = {"rust-runtime-", "codex-version-", "os-runtime-",
                             "codex-service-", "codex-terminal-"};
    const char *p = NULL;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t length = strlen(prefixes[i]);
        if (!strncmp(name, prefixes[i], length)) { p = name + length; break; }
    }
    if (!p) return 0;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    return strcmp(p, ".log") == 0;
}

struct log_entry {
    char name[128];
    struct timespec modified;
};

static int newest_first(const void *left, const void *right) {
    const struct log_entry *a = left, *b = right;
    if (a->modified.tv_sec != b->modified.tv_sec)
        return a->modified.tv_sec < b->modified.tv_sec ? 1 : -1;
    if (a->modified.tv_nsec != b->modified.tv_nsec)
        return a->modified.tv_nsec < b->modified.tv_nsec ? 1 : -1;
    return strcmp(b->name, a->name);
}

int main(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return 1;
    int reuse = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) {
        close(server);
        return 1;
    }
    struct sockaddr_in address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(19061);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, 1)) {
        close(server);
        return 1;
    }
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(server, &ready);
    struct timeval timeout = {.tv_sec = 90};
    if (select(server + 1, &ready, NULL, NULL, &timeout) <= 0) {
        close(server);
        return 1;
    }
    int client = accept(server, NULL, NULL);
    close(server);
    if (client < 0) return 1;
    int no_sigpipe = 1;
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
    timeout.tv_sec = 5;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char request[1024] = {0};
    ssize_t received = recv(client, request, sizeof(request) - 1, 0);
    if (received < 14 || strncmp(request, "GET /logs HTTP", 14)) {
        close(client);
        return 1;
    }
    const char *header = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                         "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_bytes(client, header, strlen(header));
    DIR *directory = opendir("/data/ps5-ai-cli");
    if (!directory) {
        const char *error = "Probe log directory is unavailable.\n";
        send_bytes(client, error, strlen(error));
    } else {
        struct dirent *entry;
        struct log_entry recent[16];
        unsigned count = 0;
        while ((entry = readdir(directory))) {
            if (!is_log_name(entry->d_name)) continue;
            if (strlen(entry->d_name) >= sizeof(recent[0].name)) continue;
            char path[512];
            snprintf(path, sizeof(path), "/data/ps5-ai-cli/%s", entry->d_name);
            int file = open(path, O_RDONLY | O_NOFOLLOW);
            if (file < 0) continue;
            struct stat st;
            if (fstat(file, &st) || !S_ISREG(st.st_mode)) {
                close(file);
                continue;
            }
            close(file);
            struct log_entry candidate = {.modified = st.st_mtim};
            snprintf(candidate.name, sizeof(candidate.name), "%s", entry->d_name);
            if (count < 16) recent[count++] = candidate;
            else if (newest_first(&candidate, &recent[count - 1]) < 0) recent[count - 1] = candidate;
            else continue;
            qsort(recent, count, sizeof(recent[0]), newest_first);
        }
        closedir(directory);
        for (unsigned i = 0; i < count; i++) {
            char path[512];
            snprintf(path, sizeof(path), "/data/ps5-ai-cli/%s", recent[i].name);
            int file = open(path, O_RDONLY | O_NOFOLLOW);
            if (file < 0) continue;
            struct stat st;
            if (fstat(file, &st) || !S_ISREG(st.st_mode)) { close(file); continue; }
            send_bytes(client, recent[i].name, strlen(recent[i].name));
            send_bytes(client, "\n", 1);
            char chunk[4096];
            size_t remaining = 65536;
            while (remaining) {
                ssize_t n = read(file, chunk, remaining < sizeof(chunk) ? remaining : sizeof(chunk));
                if (n <= 0 || send_bytes(client, chunk, (size_t)n)) break;
                remaining -= (size_t)n;
            }
            close(file);
            send_bytes(client, "\n", 1);
        }
        if (!count) send_bytes(client, "No probe logs found.\n", 21);
    }
    close(client);
    return 0;
}
