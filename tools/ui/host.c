/* SPDX-License-Identifier: GPL-3.0-or-later
 * Local terminal transport test. Runs only an explicitly supplied command.
 */
#include "../../app/gateway.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
static int master = -1, attached;
static pid_t child = -1;
static char **command;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void stop(int sig) {
    (void)sig;
    if (child > 0) kill(-child, SIGHUP);
    _exit(0);
}
static void resize_terminal(unsigned cols, unsigned rows) {
    struct winsize window = {.ws_col = cols, .ws_row = rows};
    if (master >= 0) ioctl(master, TIOCSWINSZ, &window);
}
static int attach_terminal(unsigned cols, unsigned rows) {
    pthread_mutex_lock(&lock);
    if (attached) { pthread_mutex_unlock(&lock); errno = EBUSY; return -1; }
    int reconnect = master >= 0;
    if (master < 0) {
        struct winsize window = {.ws_col = cols, .ws_row = rows};
        child = forkpty(&master, NULL, NULL, &window);
        if (child == 0) {
            setenv("TERM", "xterm-256color", 1);
            execvp(command[0], command);
            _exit(127);
        }
        if (child < 0) { pthread_mutex_unlock(&lock); return -1; }
        fcntl(master, F_SETFD, FD_CLOEXEC);
        fcntl(master, F_SETFL, O_NONBLOCK);
    }
    int copy = dup(master);
    if (copy >= 0 && fcntl(copy, F_SETFD, FD_CLOEXEC) < 0) { close(copy); copy = -1; }
    if (copy >= 0) {
        attached = 1; resize_terminal(cols, rows);
        if (reconnect) kill(child, SIGWINCH);
    }
    pthread_mutex_unlock(&lock);
    return copy;
}
static void detach_terminal(void) {
    pthread_mutex_lock(&lock); attached = 0; pthread_mutex_unlock(&lock);
}
int main(int argc, char **argv) {
    const char *code = getenv("PS5_UI_PAIR_CODE");
    if (!code || argc < 2) { fputs("Set PS5_UI_PAIR_CODE and supply the local test command.\n", stderr); return 1; }
    command = argv + 1;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop); signal(SIGINT, stop);
    unsigned port = getenv("PS5_UI_PORT") ? (unsigned)strtoul(getenv("PS5_UI_PORT"), NULL, 10) : 8035;
    if (port < 1024 || port > 65535) return 1;
    struct ui_config config = {.port = port, .pair_code = code, .fixture = 1, .loopback_only = 1,
        .attach = attach_terminal, .resize = resize_terminal, .detach = detach_terminal};
    if (ps5_ui_start(&config)) { perror("gateway"); return 1; }
    printf("Local terminal test on http://127.0.0.1:%u\n", port); fflush(stdout);
    for (;;) pause();
}
