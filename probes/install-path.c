/* Record this project's actual uploaded Codex image for elfldr self-reexec. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../build/install-path.h"

int main(void) {
    int image = open(PS5_INSTALLED_CODEX_PATH, O_RDONLY | O_NOFOLLOW);
    if (image < 0) return 1;
    struct stat info;
    unsigned char header[20];
    int valid = fstat(image, &info) == 0 && S_ISREG(info.st_mode) &&
                info.st_size == PS5_INSTALLED_CODEX_SIZE &&
                read(image, header, sizeof(header)) == sizeof(header) &&
                memcmp(header, "\177ELF\2\1", 6) == 0 &&
                header[16] == 3 && header[17] == 0 && header[18] == 62 && header[19] == 0;
    close(image);
    if (!valid) return 1;
    if (mkdir("/data/ps5-ai-cli", 0700) && errno != EEXIST) return 1;
    char temp[128];
    snprintf(temp, sizeof(temp), "/data/ps5-ai-cli/.codex-path-%ld.tmp", (long)getpid());
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return 1;
    const char *path = PS5_INSTALLED_CODEX_PATH "\n";
    size_t length = strlen(path);
    int result = 0;
    while (length) {
        ssize_t n = write(fd, path, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { result = 1; break; }
        path += n;
        length -= (size_t)n;
    }
    if (fsync(fd)) result = 1;
    if (close(fd)) result = 1;
    if (!result && rename(temp, "/data/ps5-ai-cli/codex-path")) result = 1;
    if (result) unlink(temp);
    return result;
}
