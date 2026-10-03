/* SPDX-License-Identifier: GPL-3.0-or-later
 * The target libc exports stdio but not POSIX getline. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

ssize_t getline(char **line, size_t *capacity, FILE *stream) {
    if (!line || !capacity || !stream) { errno = EINVAL; return -1; }
    size_t used = 0;
    for (;;) {
        if (!*line || used + 1 >= *capacity) {
            size_t next = !*line || *capacity < 128 ? 128 : *capacity * 2;
            if (next <= used + 1 || next > (size_t)PTRDIFF_MAX) { errno = EOVERFLOW; return -1; }
            char *grown = realloc(*line, next);
            if (!grown) return -1;
            *line = grown; *capacity = next;
        }
        int byte = fgetc(stream);
        if (byte == EOF) {
            (*line)[used] = 0;
            return ferror(stream) || !used ? -1 : (ssize_t)used;
        }
        (*line)[used++] = (char)byte;
        if (byte == '\n') { (*line)[used] = 0; return (ssize_t)used; }
    }
}
