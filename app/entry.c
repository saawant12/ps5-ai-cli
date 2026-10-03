/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "gateway.h"
#include "native-terminal.h"
#include "../launcher/install.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef PS5_DEV_PAIRING
#include "../build/terminal-pair.h"
#endif
extern int ps5_install_runtime_image(void);
extern int ps5_install_trust_store(void);
extern int __real_main(int argc, char **argv);
struct notification { char reserved[45]; char message[3075]; };
extern int sceKernelSendNotificationRequest(int, struct notification *, size_t, int);
static void notify(const char *text) {
    struct notification n = {0};
    snprintf(n.message, sizeof(n.message), "%s", text);
    sceKernelSendNotificationRequest(0, &n, sizeof(n), 0);
}
static int directory(const char *path) {
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    close(fd); return 0;
}
int __wrap_main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (directory(PS5_AI_STATE) || directory(PS5_AI_STATE "/home") || directory(PS5_AI_STATE "/workspace") || directory(PS5_AI_STATE "/home/.codex") || directory(PS5_AI_STATE "/tmp")) return 1;
    /* Configure this process before starting any threads. */
    if (setenv("HOME", PS5_AI_STATE "/home", 1) || setenv("CODEX_HOME", PS5_AI_STATE "/home/.codex", 1) ||
        setenv("TERM", "xterm-256color", 1) || chdir(PS5_AI_STATE "/workspace")) return 1;
    if (setenv("TMPDIR", PS5_AI_STATE "/tmp", 1) || ps5_install_trust_store()) return 1;
    signal(SIGPIPE, SIG_IGN);
    char code[9]; snprintf(code, sizeof(code), "%08u", arc4random_uniform(100000000));
#ifdef PS5_DEV_PAIRING
    memcpy(code, PS5_DEV_PAIR_CODE, sizeof(code));
#endif
    if (ps5_terminal_prepare()) return 1;
    struct ui_config config = {.port = PS5_AI_PORT, .pair_code = code,
        .attach = ps5_terminal_attach, .resize = ps5_terminal_resize, .detach = ps5_terminal_detach};
    if (ps5_ui_start(&config)) {
        notify("PS5 AI CLI could not start. Its port may already be in use. No shortcut files were changed.");
        return 1;
    }
    /* Bind before installation: a second launch cannot mutate the app files. */
    int state = open(PS5_AI_STATE, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    int installed = state >= 0 ? ps5_ai_launcher_ensure(state) : -1;
    char log_path[160];
    snprintf(log_path, sizeof(log_path), PS5_AI_STATE "/codex-terminal-%ld.log", (long)getpid());
    int log = open(log_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (log >= 0) {
        char record[512];
        int length = snprintf(record, sizeof(record), "Terminal gateway ready. Shortcut result=%d method=%s error=%s\n", installed,
            ps5_ai_launcher_registration_method(), ps5_ai_launcher_last_error());
        if (length > 0 && (size_t)length < sizeof(record)) {
            size_t done = 0;
            while (done < (size_t)length) {
                ssize_t n = write(log, record + done, (size_t)length - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                done += (size_t)n;
            }
        }
        close(log);
    }
    if (state >= 0) close(state);
    char message[256];
    snprintf(message, sizeof(message), "PS5 AI CLI :%d | Pair: %s | %s", PS5_AI_PORT, code,
        installed >= 0 ? "Open the PS5 AI CLI icon" : "Shortcut unavailable; browser terminal is running");
    notify(message);
    if (ps5_terminal_wait()) return 1;
    if (ps5_install_runtime_image()) {
        char failure[192];
        snprintf(failure, sizeof(failure), "PS5 AI CLI runtime setup failed: %s. Check its installed ELF in Payload Manager.", strerror(errno));
        notify(failure);
        perror("PS5 AI CLI could not install its runtime image");
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    char *args[] = {"codex", "--no-alt-screen", "-c", "cli_auth_credentials_store=\"file\"",
        "-c", "mcp_oauth_credentials_store=\"file\"", NULL};
    return __real_main((int)(sizeof(args)/sizeof(args[0])-1), args);
}
