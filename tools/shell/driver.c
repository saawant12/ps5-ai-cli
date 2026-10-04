/* Exercise the command proxy's signal and exit contracts with a native child. */
#include "../../platform/sdk-spawn.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern int ps5_shell_execve(const char *, char *const [], char *const []);
extern char **environ;
static volatile sig_atomic_t notified;
static void handler(int signal_number) { notified = signal_number; }

pid_t ps5_spawn_sdk(const char *path, char **argv, char **envp, const char *cwd,
                   int input, int output, int error, const int *fds,
                   size_t count, int process_mode) {
    (void)argv; (void)envp; (void)cwd; (void)input; (void)output; (void)error;
    (void)fds; (void)count; (void)process_mode;
    struct sigaction current;
    if (sigaction(SIGCHLD, NULL, &current) || current.sa_handler != SIG_DFL) {
        errno = EINVAL;
        return -1;
    }
    if (!strcmp(path, "failure")) { errno = ENOEXEC; return -1; }
    pid_t child = fork();
    if (!child) {
        if (!strcmp(path, "signal")) {
            raise(SIGTERM);
            _exit(100);
        }
        if (write(STDOUT_FILENO, "child ran\n", 10) != 10) _exit(101);
        _exit(37);
    }
    return child;
}

int main(int argc, char **argv) {
    if (argc != 2) return 90;
    struct sigaction original = {0}, restored;
    original.sa_handler = !strcmp(argv[1], "ignored") ? SIG_IGN : handler;
    original.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&original.sa_mask);
    sigaddset(&original.sa_mask, SIGUSR1);
    if (sigaction(SIGCHLD, &original, NULL)) return 91;
    char *args[] = {argv[1], NULL};
    int result = ps5_shell_execve(argv[1], args, environ);
    if (strcmp(argv[1], "failure") || result != -1 || errno != ENOEXEC) return 92;
    if (sigaction(SIGCHLD, NULL, &restored) || restored.sa_handler != handler ||
        !(restored.sa_flags & SA_NOCLDSTOP) || !sigismember(&restored.sa_mask, SIGUSR1)) return 93;
    raise(SIGCHLD);
    return notified == SIGCHLD ? 0 : 94;
}
