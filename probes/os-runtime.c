/* Exercise the native interfaces needed before Codex parses its arguments. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <libutil.h>
#include <unistd.h>

static void *thread_entry(void *unused) { return unused; }
static void report(const char *name, int result) {
    int error = errno;
    printf("%s: result=%d errno=%d\n", name, result, result < 0 ? error : 0);
}

int main(void) {
    if (mkdir("/data/ps5-ai-cli", 0700) && errno != EEXIST) return 1;
    char path[128];
    snprintf(path, sizeof(path), "/data/ps5-ai-cli/os-runtime-%ld.log", (long)getpid());
    int log = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (log < 0) return 1;
    if (dup2(log, 1) < 0 || dup2(log, 2) < 0) { close(log); return 1; }
    if (log > 2) close(log);
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("PS5 native OS runtime diagnostic (not Codex)");
    int kq = kqueue();
    report("kqueue", kq);
    if (kq >= 0) {
        report("kqueue F_SETFD CLOEXEC", fcntl(kq, F_SETFD, FD_CLOEXEC));
        int duplicate = fcntl(kq, F_DUPFD_CLOEXEC, 3);
        report("kqueue F_DUPFD_CLOEXEC", duplicate);
        if (duplicate >= 0) close(duplicate);
        duplicate = dup(kq);
        report("kqueue dup", duplicate);
        if (duplicate >= 0) close(duplicate);
        struct kevent change, event;
        struct timespec zero = {0};
        EV_SET(&change, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
        report("kevent EVFILT_USER register", kevent(kq, &change, 1, NULL, 0, &zero));
        EV_SET(&change, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        report("kevent EVFILT_USER trigger", kevent(kq, &change, 1, &event, 1, &zero));
        close(kq);
    }
    int pair[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair);
    report("socketpair flags", result);
    if (!result) { close(pair[0]); close(pair[1]); }
    result = pipe2(pair, O_NONBLOCK | O_CLOEXEC);
    report("pipe2 flags", result);
    if (!result) { close(pair[0]); close(pair[1]); }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    report("socket flags", fd);
    if (fd >= 0) close(fd);
    pthread_attr_t attributes;
    result = pthread_attr_init(&attributes);
    printf("pthread_attr_init: %d\n", result);
    if (!result) {
        result = pthread_attr_setstacksize(&attributes, 16 * 1024 * 1024);
        printf("pthread_attr_setstacksize 16MiB: %d\n", result);
        if (!result) {
            pthread_t thread;
            result = pthread_create(&thread, &attributes, thread_entry, NULL);
            printf("pthread_create 16MiB: %d\n", result);
            if (!result) printf("pthread_join: %d\n", pthread_join(thread, NULL));
        }
        pthread_attr_destroy(&attributes);
    }
    int master = -1, slave = -1;
    errno = 0;
    int raw_master = posix_openpt(O_RDWR | O_NOCTTY);
    printf("SDK posix_openpt direct: result=%d errno=%d\n", raw_master, errno);
    if (raw_master >= 0) {
        report("SDK posix_openpt descriptor check", fcntl(raw_master, F_GETFD));
        if (fcntl(raw_master, F_GETFD) >= 0) close(raw_master);
    }
    struct stat missing;
    errno = 0;
    result = fstatat(AT_FDCWD, "/data/ps5-ai-cli/definitely-not-present-probe", &missing, 0);
    printf("SDK fstatat missing path: result=%d errno=%d\n", result, errno);
    result = openpty(&master, &slave, NULL, NULL, NULL);
    report("openpty", result);
    if (!result) {
        char name[128];
        printf("ttyname_r: %d\n", ttyname_r(slave, name, sizeof(name)));
        close(master);
        close(slave);
    }
    const char *shell_paths[] = {"/bin/sh", "/usr/bin/sh", "/system/bin/sh",
                                 "/user/homebrew/bin/sh", "/data/ps5-ai-cli/bin/sh"};
    for (size_t i = 0; i < sizeof(shell_paths) / sizeof(shell_paths[0]); i++) {
        struct stat info;
        result = stat(shell_paths[i], &info);
        report(shell_paths[i], result);
    }
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) _exit(42);
    report("fork", child);
    if (child > 0) {
        int status = 0;
        pid_t waited = waitpid(child, &status, 0);
        report("waitpid", waited);
        printf("child exit: %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    puts("OS diagnostic complete");
    return 0;
}
