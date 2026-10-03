/* SPDX-License-Identifier: GPL-3.0-or-later
 * Content-addressed runtime assets. Existing files must match exactly; foreign
 * files are never replaced. Each new file becomes visible only after fsync.
 */
#include "config.h"
#include "../platform/sdk-spawn.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../build/runtime-tools.h"

#define RUNTIME_NAME "tools-" PS5_RUNTIME_ID
#define RUNTIME_PATH PS5_AI_STATE "/runtime/" RUNTIME_NAME
static const char owner[] = "PS5 AI CLI runtime " PS5_RUNTIME_ID "\n";

static int exact_file(int parent, const char *name, const unsigned char *bytes,
                      size_t size, int executable) {
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat info;
    int result = -1;
    if (fstat(fd, &info)) goto done;
    if (!S_ISREG(info.st_mode) || info.st_size < 0 || (size_t)info.st_size != size ||
        (executable && !(info.st_mode & 0100))) { errno = EEXIST; goto done; }
    unsigned char block[8192];
    size_t offset = 0;
    while (offset < size) {
        size_t wanted = size - offset < sizeof(block) ? size - offset : sizeof(block);
        ssize_t count = read(fd, block, wanted);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) goto done;
        if (!count || memcmp(block, bytes + offset, (size_t)count)) { errno = EEXIST; goto done; }
        offset += (size_t)count;
    }
    result = 0;
done:;
    int error = errno;
    close(fd);
    errno = error;
    return result;
}

static int install_file(int parent, const char *name, const unsigned char *bytes,
                        size_t size, int executable) {
    if (!exact_file(parent, name, bytes, size, executable)) return 0;
    if (errno != ENOENT) return -1;
    char temporary[96];
    snprintf(temporary, sizeof(temporary), ".%s-%ld.tmp", name, (long)getpid());
    int fd = openat(parent, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                    executable ? 0700 : 0600);
    if (fd < 0) return -1;
    int result = -1;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) goto done;
        offset += (size_t)count;
    }
    if (fsync(fd)) goto done;
    if (close(fd)) { fd = -1; goto done; }
    fd = -1;
    /* The app's bound gateway serializes installers. Recheck before publishing
     * so an existing foreign file is not replaced during an interrupted retry. */
    if (!exact_file(parent, name, bytes, size, executable)) { result = 0; goto done; }
    if (errno != ENOENT) goto done;
    result = renameat(parent, temporary, parent, name);
    if (!result) result = fsync(parent);
done:;
    int error = errno;
    if (fd >= 0) close(fd);
    unlinkat(parent, temporary, 0);
    errno = error;
    return result;
}

int ps5_install_runtime_tools(void) {
    if (!ps5_sdk_elf_valid(runtime_shell, sizeof(runtime_shell)) ||
        !ps5_sdk_elf_valid(runtime_box, sizeof(runtime_box))) { errno = ENOEXEC; return -1; }
    if (mkdir(PS5_AI_STATE "/runtime", 0700) && errno != EEXIST) return -1;
    int parent = open(PS5_AI_STATE "/runtime", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (parent < 0) return -1;
    int created = mkdirat(parent, RUNTIME_NAME, 0700) == 0;
    if (!created && errno != EEXIST) { int error = errno; close(parent); errno = error; return -1; }
    int directory = openat(parent, RUNTIME_NAME, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    close(parent);
    if (directory < 0) return -1;
    int result = -1;
    if (!created && exact_file(directory, ".owner", (const unsigned char *)owner, sizeof(owner)-1, 0)) goto done;
    if (created && install_file(directory, ".owner", (const unsigned char *)owner, sizeof(owner)-1, 0)) goto done;
    for (size_t i = 0; i < sizeof(runtime_assets)/sizeof(runtime_assets[0]); i++) {
        const struct runtime_asset *asset = &runtime_assets[i];
        if (install_file(directory, asset->name, asset->data, asset->size, 1)) goto done;
    }
    result = 0;
done:;
    int error = errno;
    close(directory);
    if (result) { errno = error; return -1; }
    return 0;
}

int ps5_configure_runtime_tools(void) {
    /* Before CLI threads start. Upstream may prepend its own command aliases. */
    return setenv("SHELL", RUNTIME_PATH "/sh", 1) || setenv("PATH", RUNTIME_PATH, 1) ? -1 : 0;
}
