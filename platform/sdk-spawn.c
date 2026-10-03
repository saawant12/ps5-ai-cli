/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sdk-spawn.h"
#include "../vendor/shsrv/elfldr.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef PS5_TOOL_TRACE
#include <stdio.h>
#define SPAWN_TRACE(stage) fprintf(stderr, "spawn %ld: %s\n", (long)getpid(), stage)
#else
#define SPAWN_TRACE(stage) ((void)0)
#endif

static int bounded_vector(char **values, size_t *remaining) {
    if (!values) return 0;
    for (size_t i = 0; i < 4096; i++) {
        if (!values[i]) return 0;
        size_t length = strnlen(values[i], *remaining);
        if (length == *remaining || *remaining - length < sizeof(char *) + 1) break;
        *remaining -= length + sizeof(char *) + 1;
    }
    errno = E2BIG;
    return -1;
}
pid_t ps5_spawn_sdk(const char *path, char **argv, char **envp, const char *cwd,
                    int input, int output, int error, const int *fds,
                    size_t fd_count, int process_mode) {
    SPAWN_TRACE("entry");
    size_t budget = 128 * 1024;
    if (!path || !*path || !argv || !argv[0]) { errno = EINVAL; return -1; }
    if (bounded_vector(argv, &budget) || bounded_vector(envp, &budget)) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat info;
    uint8_t *image = NULL;
    pid_t child = -1;
    if (fstat(fd, &info)) goto done;
    if (!S_ISREG(info.st_mode)) { errno = EACCES; goto done; }
    if (!(info.st_mode & 0111)) { errno = EACCES; goto done; }
    if (info.st_size < 64 || info.st_size > 256 * 1024 * 1024) { errno = ENOEXEC; goto done; }
    SPAWN_TRACE("allocating image");
    image = malloc((size_t)info.st_size);
    if (!image) goto done;
    SPAWN_TRACE("reading image");
    size_t used = 0;
    while (used < (size_t)info.st_size) {
        ssize_t n = read(fd, image + used, (size_t)info.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) { errno = ENOEXEC; goto done; }
        used += (size_t)n;
    }
    if (!ps5_sdk_elf_valid(image, used)) { errno = ENOEXEC; goto done; }
    SPAWN_TRACE("launching validated image");
    child = elfldr_spawn_context(input, output, error, image, argv, envp, cwd,
                                 fds, fd_count, process_mode);
done:;
    int saved = errno;
    free(image);
    close(fd);
    errno = saved;
    return child;
}
