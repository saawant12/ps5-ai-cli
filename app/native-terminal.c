/* SPDX-License-Identifier: GPL-3.0-or-later
 * A raw byte terminal for the in-process CLI, not a general-purpose kernel PTY.
 * Window changes are delivered through SIGWINCH. Only our actual stdio socket
 * is adapted; files, pipes, and child-program descriptors retain native errors.
 */
#include "native-terminal.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static int channel[2] = {-1, -1}, attached, started, installed;
static struct termios attributes;
static struct winsize dimensions = {.ws_col = 80, .ws_row = 24};
static struct stat identity;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t selected = PTHREAD_COND_INITIALIZER;

int ps5_terminal_prepare(void) {
    /* Reserve standard descriptors before gateway threads create sockets. */
    for (int fd = 0; fd <= STDERR_FILENO; fd++) {
        if (fcntl(fd, F_GETFD) >= 0) continue;
        if (errno != EBADF) return -1;
        int file = open("/dev/null", O_RDWR);
        if (file < 0) return -1;
        if (file != fd) {
            int result = dup2(file, fd), error = errno;
            close(file);
            if (result < 0) { errno = error; return -1; }
        }
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel)) return -1;
    for (int i = 0; i < 2; i++) {
        if (fcntl(channel[i], F_SETFD, FD_CLOEXEC) < 0) goto fail;
    }
    if (fcntl(channel[0], F_SETFL, O_NONBLOCK) < 0) goto fail;
    memset(&attributes, 0, sizeof(attributes));
    attributes.c_cflag = CS8 | CREAD | CLOCAL;
    attributes.c_cc[VMIN] = 1;
    cfsetispeed(&attributes, B38400); cfsetospeed(&attributes, B38400);
    return 0;
fail:
    close(channel[0]); close(channel[1]); channel[0] = channel[1] = -1;
    return -1;
}

int ps5_terminal_attach(unsigned cols, unsigned rows) {
    pthread_mutex_lock(&lock);
    if (attached || channel[0] < 0) { pthread_mutex_unlock(&lock); errno = EBUSY; return -1; }
    int fd = dup(channel[0]);
    if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno; close(fd); fd = -1; errno = error;
    }
    if (fd >= 0) {
        dimensions.ws_col = cols; dimensions.ws_row = rows;
        attached = 1; started = 1;
        pthread_cond_signal(&selected);
    }
    int redraw = fd >= 0 && installed;
    pthread_mutex_unlock(&lock);
    if (redraw) kill(getpid(), SIGWINCH);
    return fd;
}

void ps5_terminal_detach(void) {
    pthread_mutex_lock(&lock); attached = 0; pthread_mutex_unlock(&lock);
}

void ps5_terminal_resize(unsigned cols, unsigned rows) {
    pthread_mutex_lock(&lock);
    int changed = dimensions.ws_col != cols || dimensions.ws_row != rows;
    dimensions.ws_col = cols; dimensions.ws_row = rows;
    int notify = installed && changed;
    pthread_mutex_unlock(&lock);
    if (notify) kill(getpid(), SIGWINCH);
}

int ps5_terminal_wait(void) {
    pthread_mutex_lock(&lock);
    while (!started) pthread_cond_wait(&selected, &lock);
    for (int fd = 0; fd <= 2; fd++) {
        if (dup2(channel[1], fd) < 0) { pthread_mutex_unlock(&lock); return -1; }
    }
    if (fstat(0, &identity)) { pthread_mutex_unlock(&lock); return -1; }
    close(channel[1]); channel[1] = -1;
    installed = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}

static int virtual_fd(int fd) {
    /* Crossterm borrows stdin/stdout. Restrict recognition to the connected
     * standard descriptors; do not advertise arbitrary sockets as terminals. */
    if (fd < 0 || fd > 2) return 0;
    struct stat info;
    pthread_mutex_lock(&lock);
    int enabled = installed;
    struct stat expected = identity;
    pthread_mutex_unlock(&lock);
    return enabled && !fstat(fd, &info) && S_ISSOCK(info.st_mode) &&
        info.st_dev == expected.st_dev && info.st_ino == expected.st_ino;
}

extern int __real_isatty(int);
extern int __real_tcgetattr(int, struct termios *);
extern int __real_tcsetattr(int, int, const struct termios *);
extern int __real_tcflush(int, int);
extern int __real_ioctl(int, unsigned long, ...);

int __wrap_isatty(int fd) { return virtual_fd(fd) ? 1 : __real_isatty(fd); }
int __wrap_tcgetattr(int fd, struct termios *value) {
    if (!virtual_fd(fd)) return __real_tcgetattr(fd, value);
    if (!value) { errno = EFAULT; return -1; }
    pthread_mutex_lock(&lock); *value = attributes; pthread_mutex_unlock(&lock);
    return 0;
}
int __wrap_tcflush(int fd, int action) {
    if (!virtual_fd(fd)) return __real_tcflush(fd, action);
    if (action != TCIFLUSH) { errno = EOPNOTSUPP; return -1; }
    char buffer[1024];
    while (recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {}
    return 0;
}
int __wrap_tcsetattr(int fd, int action, const struct termios *value) {
    if (!virtual_fd(fd)) return __real_tcsetattr(fd, action, value);
    if (!value) { errno = EFAULT; return -1; }
    if (action != TCSANOW && action != TCSADRAIN && action != TCSAFLUSH) { errno = EINVAL; return -1; }
    /* The CLI owns input parsing. Never claim to implement a cooked line
     * discipline, echo or kernel signal generation on this socket transport. */
    if (value->c_lflag & (ICANON | ECHO | ISIG) || value->c_oflag & OPOST) { errno = EOPNOTSUPP; return -1; }
    if (action == TCSAFLUSH && __wrap_tcflush(fd, TCIFLUSH)) return -1;
    pthread_mutex_lock(&lock); attributes = *value; pthread_mutex_unlock(&lock);
    return 0;
}
int __wrap_ioctl(int fd, unsigned long request, ...) {
    void *argument = NULL;
    if (request & (IOC_IN | IOC_OUT)) {
        va_list args; va_start(args, request); argument = va_arg(args, void *); va_end(args);
    }
    if (virtual_fd(fd)) {
        if (request == TIOCGWINSZ) {
            if (!argument) { errno = EFAULT; return -1; }
            pthread_mutex_lock(&lock); *(struct winsize *)argument = dimensions; pthread_mutex_unlock(&lock);
            return 0;
        }
        if (request == TIOCGETA) return __wrap_tcgetattr(fd, argument);
        if (request == TIOCSETA || request == TIOCSETAW || request == TIOCSETAF)
            return __wrap_tcsetattr(fd, request == TIOCSETA ? TCSANOW : request == TIOCSETAW ? TCSADRAIN : TCSAFLUSH, argument);
    }
    if (request & (IOC_IN | IOC_OUT)) return __real_ioctl(fd, request, argument);
    return __real_ioctl(fd, request);
}
