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
    if (!fgets(input, sizeof(input), stdin) || strcmp(input, "input roundtrip\n")) return 82;
    fputs("stdout roundtrip\n", stdout);
    fputs("stderr roundtrip\n", stderr);
    return 37;
}
