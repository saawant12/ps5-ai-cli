/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../app/cli-process.h"
#include "../../platform/sdk-spawn.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static const char *executable;
static int spawns;
static pid_t last_child;
int ps5_terminal_reserve_stdio(void) { return 0; }
int ps5_install_runtime_image(void) { return 0; }
int ps5_install_runtime_tools(void) { return 0; }
pid_t ps5_spawn_sdk(const char *path, char **args, char **env, const char *cwd,
    int input, int output, int error, const int *fds, size_t count, int mode) {
    (void)path; (void)args; (void)env; (void)cwd;
    assert(mode == 2 && count == 1 && fds[0] > 2);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        if (setsid() < 0 || dup2(input, 0) < 0 || dup2(output, 1) < 0 || dup2(error, 2) < 0) _exit(91);
        for (int fd = 3; fd < 4096; fd++) if (fd != fds[0]) close(fd);
        if (fcntl(fds[0], F_SETFD, 0)) _exit(93);
        char descriptor[24]; snprintf(descriptor, sizeof(descriptor), "%d", fds[0]);
        execl(executable, executable, "child", descriptor, NULL);
        _exit(92);
    }
    spawns++; last_child = pid;
    return pid;
}
static void receive(int fd, const char *wanted) {
    char output[256] = {0}; size_t used = 0;
    for (int i = 0; i < 100 && !strstr(output, wanted); i++) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        assert(poll(&p, 1, 50) >= 0);
        ssize_t n = read(fd, output+used, sizeof(output)-used-1);
        if (n > 0) used += (size_t)n;
        else assert(n < 0 && (errno == EAGAIN || errno == EINTR));
    }
    assert(strstr(output, wanted));
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "child")) {
        alarm(15);
        signal(SIGTERM, SIG_IGN); signal(SIGWINCH, SIG_IGN);
        (void)write(1, "READY\n", 6);
        char input[32];
        for (;;) {
            ssize_t n = read(0, input, sizeof(input));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return 0;
            if (n >= 4 && !memcmp(input, "exit", 4)) return 7;
            (void)write(1, "OK\n", 3);
        }
    }
    executable = argv[0];
    pid_t unrelated = fork(); assert(unrelated >= 0);
    if (!unrelated) { alarm(15); for (;;) pause(); }
    assert(!ps5_cli_prepare());
    int first = ps5_cli_attach(100, 30); assert(first > 2);
    receive(first, "READY");
    pid_t original = last_child;
    assert(ps5_cli_attach(100, 30) == -1 && errno == EBUSY);
    assert(write(first, "hi", 2) == 2); receive(first, "OK");
    close(first); ps5_cli_detach();
    /* A child ignoring SIGTERM must still be replaced; unrelated PIDs survive. */
    assert(!ps5_cli_restart());
    assert(spawns == 2 && last_child != original && !kill(unrelated, 0));
    int status; assert(waitpid(original, &status, WNOHANG) == -1 && errno == ECHILD);
    int second = ps5_cli_attach(80, 24); assert(second > 2); receive(second, "READY");
    assert(write(second, "exit", 4) == 4);
    close(second); ps5_cli_detach();
    for (int i = 0; i < 100; i++) { ps5_cli_poll(); usleep(10000); }
    assert(ps5_cli_attach(80, 24) == -1 && errno == ESRCH && spawns == 2);
    /* Restart is explicit after an exit; reconnect must not respawn the CLI. */
    assert(!ps5_cli_restart());
    int third = ps5_cli_attach(80, 24); assert(third > 2); receive(third, "READY");
    assert(write(third, "exit", 4) == 4);
    close(third); ps5_cli_detach();
    for (int i = 0; i < 100; i++) { ps5_cli_poll(); usleep(10000); }
    assert(!kill(unrelated, 0));
    kill(unrelated, SIGTERM); assert(waitpid(unrelated, &status, 0) == unrelated);
    puts("CLI restart, ownership, reaping and explicit relaunch passed");
    return 0;
}
