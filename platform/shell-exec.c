/* SPDX-License-Identifier: GPL-3.0-or-later
 * Single-threaded command proxy. Kept out of Rust's process-wide exec symbols:
 * it allocates, and must never be called from a multithreaded pre_exec hook.
 */
#include "sdk-spawn.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int ps5_shell_execve(const char *path, char *const argv[], char *const envp[]) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return -1;
    int fds[64];
    size_t count = 0;
    int limit = getdtablesize();
    if (limit < 0) return -1;
    for (int fd = 3; fd < limit; fd++) {
        int flags = fcntl(fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC)) continue;
        if (count == sizeof(fds) / sizeof(*fds)) { errno = E2BIG; return -1; }
        fds[count++] = fd;
    }
#ifdef PS5_TOOL_TRACE
    fprintf(stderr, "proxy %ld loading %s\n", (long)getpid(), path);
#endif
    /* This process now owns the loader's traced child and its final wait.
     * Do not run the caller shell's SIGCHLD handler during that handshake.
     * Restore it on failure so command lookup and script fallback still work. */
    struct sigaction saved_chld, default_chld;
    memset(&default_chld, 0, sizeof(default_chld));
    default_chld.sa_handler = SIG_DFL;
    sigemptyset(&default_chld.sa_mask);
    if (sigaction(SIGCHLD, &default_chld, &saved_chld)) return -1;
    pid_t child = ps5_spawn_sdk(path, (char **)argv, (char **)envp, cwd,
                               0, 1, 2, fds, count, 0);
#ifdef PS5_TOOL_TRACE
    fprintf(stderr, "proxy %ld child %ld error %d\n", (long)getpid(), (long)child, child < 0 ? errno : 0);
#endif
    if (child < 0) {
        int error = errno;
        sigaction(SIGCHLD, &saved_chld, NULL);
        errno = error;
        return -1;
    }
    /* The loaded child owns its stdio. Closing our copies matters for pipelines
     * such as a writer | head: the writer must be able to observe EPIPE. */
    close(0); close(1);
#ifndef PS5_TOOL_TRACE
    close(2);
#endif
    for (size_t i = 0; i < count; i++) close(fds[i]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
#ifdef PS5_TOOL_TRACE
    fprintf(stderr, "proxy %ld waited %ld status %d\n", (long)getpid(), (long)waited, status);
#endif
    if (waited < 0) _exit(125);
    if (WIFEXITED(status)) _exit(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        signal(sig, SIG_DFL);
        sigset_t unblocked;
        sigemptyset(&unblocked); sigaddset(&unblocked, sig);
        sigprocmask(SIG_UNBLOCK, &unblocked, NULL);
        kill(getpid(), sig);
        _exit(128 + sig);
    }
    _exit(125);
}

/* sbase's env/find/xargs use execvp. Only their link uses this wrapper; the
 * loader's system execve must continue to enter the console's signed image. */
int __wrap_execvp(const char *name, char *const argv[]) {
    if (strchr(name, '/')) return ps5_shell_execve(name, argv, environ);
    const char *path = getenv("PATH");
    if (!path) { errno = ENOENT; return -1; }
    int saved = ENOENT;
    do {
        const char *end = strchr(path, ':');
        size_t length = end ? (size_t)(end - path) : strlen(path);
        char candidate[PATH_MAX];
        size_t name_size = strlen(name) + 1;
        if (length + (length ? 1 : 0) + name_size <= sizeof(candidate)) {
            memcpy(candidate, path, length);
            if (length) candidate[length++] = '/';
            memcpy(candidate + length, name, name_size);
            ps5_shell_execve(candidate, argv, environ);
            if (errno != ENOENT && errno != ENOTDIR) saved = errno;
        } else saved = ENAMETOOLONG;
        path = end ? end + 1 : NULL;
    } while (path);
    errno = saved;
    return -1;
}
