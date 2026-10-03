/* Verify the same embedded tool installer used by the app, without starting UI. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "../app/config.h"
extern int ps5_install_runtime_tools(void);
extern int ps5_configure_runtime_tools(void);
int main(void) {
    char path[160];
    snprintf(path, sizeof(path), PS5_AI_STATE "/os-runtime-%ld.log", (long)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) return 1;
    if (fd > 2) close(fd);
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("PS5 bundled runtime installer probe");
    if (ps5_configure_runtime_tools()) { perror("configure tools"); return 1; }
    for (int attempt = 0; attempt < 2; attempt++) {
        if (ps5_install_runtime_tools()) { perror("install tools"); return 1; }
        printf("Native bundled shell + tools install %d: PASS\n", attempt + 1);
    }
    puts("Bundled runtime installation: PASS");
    return 0;
}
