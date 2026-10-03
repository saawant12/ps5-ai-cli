/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "install.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static int calls;
static int prepare(void) { return 0; }
static int register_app(void) { calls++; return getenv("PS5_AI_TEST_FAIL") ? -1 : 0; }
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int state = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (state < 0) return 2;
    int result = ps5_ai_launcher_ensure_at(state, argv[2], prepare, register_app);
    close(state);
    printf("result=%d registrations=%d\n", result, calls);
    return result < 0;
}
