/* SPDX-License-Identifier: GPL-3.0-or-later
 * Adapted from saawant12/orbit-store-ps5-source launcher.
 */
#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif
#include "install.h"
#include "../app/config.h"
#include "launcher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned char owner[] = "PS5 AI CLI " PS5_AI_TITLE " launcher v1\n";
static const unsigned char icon_owner[] = "PS5 AI CLI " PS5_AI_TITLE " icon v2\n";
static const char *title_id = PS5_AI_TITLE;
static char last_error[192];

const char *ps5_ai_launcher_last_error(void) {
    return last_error;
}

void ps5_ai_launcher_record_error(const char *step, int system_error, int platform_error) {
    snprintf(last_error, sizeof last_error, "%s: errno=%d, platform=0x%08x", step,
             system_error, (unsigned)platform_error);
}

/* 1 matches, 0 missing, -1 unsafe/different/unreadable. Never follow symlinks. */
static int matches(int parent, const char *name, const unsigned char *data, size_t length) {
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat st;
    int result = -1;
    if (fstat(fd, &st))
        goto done;
    if (!S_ISREG(st.st_mode) || st.st_size != (off_t)length) {
        errno = EINVAL;
        goto done;
    }
    unsigned char buffer[512];
    size_t offset = 0;
    while (offset < length) {
        size_t wanted = length - offset;
        if (wanted > sizeof buffer)
            wanted = sizeof buffer;
        ssize_t n = read(fd, buffer, wanted);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            goto done;
        if (!n || memcmp(buffer, data + offset, (size_t)n)) {
            errno = EINVAL;
            goto done;
        }
        offset += (size_t)n;
    }
    result = 1;
done:
    {
        int saved_error = errno;
        close(fd);
        errno = saved_error;
    }
    return result;
}

static int ensure_file(int parent, const char *name, const unsigned char *data, size_t length) {
    int existing = matches(parent, name, data, length);
    if (existing)
        return existing == 1 ? 0 : -1;
    int fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return -1;
    size_t offset = 0;
    int result = -1;
    while (offset < length) {
        ssize_t n = write(fd, data + offset, length - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            goto done;
        }
        offset += (size_t)n;
    }
    result = fsync(fd);
done:
    {
        int saved_error = errno;
        if (close(fd) && result == 0)
            result = -1;
        else
            errno = saved_error;
    }
    /* A partial write is preserved and rejected on retry, never overwritten. */
    return result;
}

static int upgrade_legacy_icon(int parent) {
    const char *temporary = ".ps5-ai-cli-icon-v2.tmp";
    if (ensure_file(parent, temporary, launcher_icon, sizeof launcher_icon)) return -1;
    /* Only the exact previously shipped image may be replaced. An existing
     * modified icon or symlink is preserved, even in an owned title folder. */
    if (matches(parent, "icon0.png", launcher_legacy_icon, sizeof launcher_legacy_icon) != 1) return -1;
    if (renameat(parent, temporary, parent, "icon0.png")) return -1;
    return fsync(parent);
}

static int open_dir(int parent, const char *name, bool create) {
    if (create && mkdirat(parent, name, 0755) && errno != EEXIST)
        return -1;
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
}

static int absent(int parent, const char *name) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0)
        return 0;
    return errno == ENOENT ? 1 : -1;
}

int ps5_ai_launcher_ensure_at(int state_fd, const char *user_parent, int (*prepare)(void),
                             int (*register_title)(void)) {
    int up = -1, user = -1, us = -1, result = -1, prepared = 0;
    const char *step = "open-app-parent";
    last_error[0] = '\0';
    errno = 0;
    up = open(user_parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (up < 0)
        goto done;
    step = "verify-ownership-record";
    int owned = matches(state_fd, "ps5-ai-cli-launcher-owned-v1", owner, sizeof owner - 1);
    if (owned < 0)
        goto done;
    step = "check-existing-title";
    if (!owned) {
        int missing = absent(up, title_id);
        if (missing <= 0) {
            if (!missing)
                errno = EEXIST;
            goto done;
        }
    }
    step = "verify-ready-record";
    int ready = matches(state_fd, "ps5-ai-cli-launcher-ready-v1", owner, sizeof owner - 1);
    if (ready < 0)
        goto done;
    if (ready && !owned) {
        errno = EINVAL;
        goto done;
    }
    /* The user may delete the home-screen app while retaining saved data.
     * Recreate a missing owned title; an existing conflicting title is still
     * checked file by file below and is never replaced. */
    if (ready) {
        step = "check-removed-title";
        int missing = absent(up, title_id);
        if (missing < 0) goto done;
        if (missing) ready = 0;
    }
    step = "verify-icon-record";
    int icon_ready = matches(state_fd, "ps5-ai-cli-launcher-icon-v2", icon_owner, sizeof icon_owner - 1);
    if (icon_ready < 0 || (icon_ready && !owned)) goto done;
    if (!ready) {
        step = "prepare-app-registration";
        errno = 0;
        if (prepare())
            goto done;
        prepared = 1;
        step = "write-ownership-record";
        if (ensure_file(state_fd, "ps5-ai-cli-launcher-owned-v1", owner, sizeof owner - 1))
            goto done;
        owned = 1;
        step = "sync-ownership-directory";
        if (fsync(state_fd))
            goto done;
    }
    step = "open-title-directory";
    user = open_dir(up, title_id, !ready);
    if (user < 0)
        goto done;
    step = "open-sce-sys-directory";
    us = open_dir(user, "sce_sys", !ready);
    if (us < 0)
        goto done;
    step = "verify-or-write-manifest";
    if (ready ? matches(us, "param.json", launcher_manifest, sizeof launcher_manifest) != 1
              : ensure_file(us, "param.json", launcher_manifest, sizeof launcher_manifest)) goto done;
    step = "verify-or-write-icon";
    int icon_match = matches(us, "icon0.png", launcher_icon, sizeof launcher_icon);
    if (icon_match != 1) {
        int legacy = owned && matches(us, "icon0.png", launcher_legacy_icon, sizeof launcher_legacy_icon) == 1;
        if (icon_match != 0 && !legacy) goto done;
        if (!prepared) {
            step = "prepare-icon-registration";
            if (prepare()) goto done;
            prepared = 1;
        }
        step = "install-icon";
        if (legacy ? upgrade_legacy_icon(us) : ensure_file(us, "icon0.png", launcher_icon, sizeof launcher_icon)) goto done;
        icon_ready = 0;
    }
    if (ready && icon_ready) {
        result = 0;
        goto done;
    }
    if (!prepared) {
        step = "prepare-icon-registration";
        if (prepare()) goto done;
    }
    step = "register-title";
    errno = 0;
    if (register_title())
        goto done;
    step = "write-ready-record";
    if (ensure_file(state_fd, "ps5-ai-cli-launcher-ready-v1", owner, sizeof owner - 1))
        goto done;
    step = "write-icon-record";
    if (ensure_file(state_fd, "ps5-ai-cli-launcher-icon-v2", icon_owner, sizeof icon_owner - 1)) goto done;
    step = "sync-ready-directory";
    if (fsync(state_fd))
        goto done;
    result = 1;
done:
    if (result < 0 && !last_error[0])
        ps5_ai_launcher_record_error(step, errno, 0);
    if (us >= 0)
        close(us);
    if (user >= 0)
        close(user);
    if (up >= 0)
        close(up);
    if (result < 0)
        printf("PS5 AI CLI launcher is not ready: %s. Server remains available.\n", last_error);
    return result;
}
