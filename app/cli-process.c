/* SPDX-License-Identifier: GPL-3.0-or-later
 * Own one native CLI child. The gateway survives CLI exits and explicit restarts.
 */
#include "cli-process.h"
#include "config.h"
#include "native-terminal.h"
#include "../platform/sdk-spawn.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
extern int ps5_install_runtime_image(void);
extern int ps5_install_runtime_tools(void);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t detached = PTHREAD_COND_INITIALIZER;
static pid_t child = -1;
static int master = -1, control = -1, attached, launched, runtime_ready;
static struct winsize dimensions = {.ws_col = 80, .ws_row = 24};

static void record(const char *action, pid_t pid, int status) {
    char path[160], message[160];
    snprintf(path, sizeof(path), PS5_AI_STATE "/codex-terminal-%ld.log", (long)getpid());
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    int length = snprintf(message, sizeof(message), "CLI %s pid=%ld status=%d\n", action, (long)pid, status);
    if (length > 0 && (size_t)length < sizeof(message)) (void)write(fd, message, (size_t)length);
    close(fd);
}

static void close_channels(void) {
    if (master >= 0) close(master);
    if (control >= 0) close(control);
    master = control = -1;
}

static int reap(void) {
    if (child <= 0) return 1;
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
        record("exited", child, status);
        child = -1;
        return 1;
    }
    if (result < 0 && errno == ECHILD) {
        /* Ownership has ended. Never signal a PID after losing that proof. */
        child = -1;
        return 1;
    }
    return result < 0 && errno != EINTR ? -1 : 0;
}

static int start(void) {
    if (!runtime_ready) {
        record("installing-image", 0, 0);
        if (ps5_install_runtime_image()) { record("image-failed", 0, errno); return -1; }
        record("installing-tools", 0, 0);
        if (ps5_install_runtime_tools()) { record("tools-failed", 0, errno); return -1; }
        runtime_ready = 1;
    }
    int stream[2] = {-1, -1}, resize[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stream) ||
        socketpair(AF_UNIX, SOCK_DGRAM, 0, resize)) goto fail;
    for (int i = 0; i < 2; i++) {
        if (fcntl(stream[i], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(resize[i], F_SETFD, FD_CLOEXEC) < 0) goto fail;
    }
    if (fcntl(stream[0], F_SETFL, O_NONBLOCK) < 0 ||
        send(resize[0], &dimensions, sizeof(dimensions), MSG_DONTWAIT) != sizeof(dimensions)) goto fail;
    char descriptor[24];
    snprintf(descriptor, sizeof(descriptor), "%d", resize[1]);
    char *args[] = {PS5_AI_STATE "/runtime/codex.elf", "--ps5-ai-cli-terminal", descriptor, NULL};
    launched = 1;
    record("loading", 0, 0);
    child = ps5_spawn_sdk(args[0], args, environ, PS5_AI_STATE "/workspace",
        stream[1], stream[1], stream[1], &resize[1], 1, 2);
    if (child <= 0) goto fail;
    close(stream[1]); close(resize[1]);
    master = stream[0]; control = resize[0];
    record("started", child, 0);
    return 0;
fail:;
    int error = errno;
    record("start-failed", child, error);
    for (int i = 0; i < 2; i++) {
        if (stream[i] >= 0) close(stream[i]);
        if (resize[i] >= 0) close(resize[i]);
    }
    errno = error;
    return -1;
}

int ps5_cli_prepare(void) { return ps5_terminal_reserve_stdio(); }

int ps5_cli_attach(unsigned columns, unsigned rows) {
    pthread_mutex_lock(&lock);
    int fd = -1;
    if (attached) { errno = EBUSY; goto done; }
    dimensions.ws_col = columns; dimensions.ws_row = rows;
    int ended = reap();
    if (ended < 0) goto done;
    if (ended && launched) { close_channels(); errno = ESRCH; goto done; }
    if (!launched && start()) goto done;
    if (send(control, &dimensions, sizeof(dimensions), MSG_DONTWAIT) != sizeof(dimensions)) goto done;
    fd = dup(master);
    if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno; close(fd); fd = -1; errno = error;
    }
    if (fd >= 0) {
        attached = 1;
        (void)kill(child, SIGWINCH);
    }
done:;
    int error = errno;
    pthread_mutex_unlock(&lock);
    errno = error;
    return fd;
}

void ps5_cli_detach(void) {
    pthread_mutex_lock(&lock);
    attached = 0;
    pthread_cond_broadcast(&detached);
    pthread_mutex_unlock(&lock);
}

void ps5_cli_resize(unsigned columns, unsigned rows) {
    pthread_mutex_lock(&lock);
    dimensions.ws_col = columns; dimensions.ws_row = rows;
    if (child > 0 && control >= 0 &&
        send(control, &dimensions, sizeof(dimensions), MSG_DONTWAIT) == sizeof(dimensions))
        (void)kill(child, SIGWINCH);
    pthread_mutex_unlock(&lock);
}

static int wait_for_exit(unsigned milliseconds) {
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10) {
        int result = reap();
        if (result) return result;
        usleep(10000);
    }
    return 0;
}

int ps5_cli_restart(void) {
    pthread_mutex_lock(&lock);
    int result = -1;
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) goto done;
    deadline.tv_sec += 3;
    while (attached) {
        int error = pthread_cond_timedwait(&detached, &lock, &deadline);
        if (error) { errno = error; goto done; }
    }
    if (child > 0) {
        /* The unreaped child owns its new session/process-group ID. */
        if (kill(-child, SIGTERM) && errno != ESRCH) goto done;
        int ended = wait_for_exit(1000);
        if (ended < 0) goto done;
        if (!ended) {
            if (kill(-child, SIGKILL) && errno != ESRCH) goto done;
            ended = wait_for_exit(2000);
            if (ended < 0) goto done;
            if (!ended) { errno = ETIMEDOUT; goto done; }
        }
    }
    close_channels();
    result = start();
done:;
    int error = errno;
    pthread_mutex_unlock(&lock);
    errno = error;
    return result;
}

void ps5_cli_poll(void) {
    pthread_mutex_lock(&lock);
    if (!attached && reap() > 0) close_channels();
    pthread_mutex_unlock(&lock);
}
