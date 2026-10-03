/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <sys/stat.h>
extern int ps5_install_runtime_image(void);
int ps5_set_executable_path(const char *path) {
    struct stat info;
    return stat(path, &info) || !S_ISREG(info.st_mode) ? -1 : 0;
}
int main(void) {
    if (ps5_install_runtime_image()) { perror("runtime installation"); return 1; }
    return 0;
}
