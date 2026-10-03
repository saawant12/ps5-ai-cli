/* Tiny child program used only by the process-launch diagnostic. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--wait-tree")) {
        pid_t child = fork();
        if (child < 0) return 84;
        if (!child) for (;;) pause();
        fputs("ready\n", stdout);
        fflush(stdout);
        for (;;) pause();
    }
    if (argc == 2 && !strcmp(argv[1], "--wait")) {
        fputs("ready\n", stdout);
        fflush(stdout);
        for (;;) pause();
    }
    char cwd[PATH_MAX];
    char input[64] = {0};
    const char *expected_cwd = getenv("PS5_PROBE_CWD");
    const char *value = getenv("PS5_PROBE_VALUE");
    if (argc != 4 || strcmp(argv[1], "argument with spaces") ||
        strcmp(argv[2], "quote'\"literal") || !expected_cwd ||
        !value || strcmp(value, "environment roundtrip") ||
        !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, expected_cwd)) return 80;
    int fd = atoi(argv[3]);
    errno = 0;
    if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) return 81;
    const char *attachment = getenv("PS5_PROBE_ATTACHMENT");
    if (attachment) {
        int attached = atoi(attachment);
        if (fcntl(attached, F_GETFD) < 0 || write(attached, "attached\n", 9) != 9) return 83;
    }
    if (!fgets(input, sizeof(input), stdin) || strcmp(input, "input roundtrip\n")) return 82;
    fputs("stdout roundtrip\n", stdout);
    fputs("stderr roundtrip\n", stderr);
    return 37;
}
