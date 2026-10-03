/* Exercise the pinned upstream loader only against our embedded test child.
 * A supervisor bounds the complete experiment and owns its process group.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../vendor/shsrv/elfldr.h"
#include "../build/process-helper.h"

#ifdef PS5_LOADER_RUST
extern int ps5_rust_loader_probe(const char *directory);
#endif

static void read_text(int fd, char *buffer, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        ssize_t count = read(fd, buffer + used, capacity - used - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += (size_t)count;
    }
    buffer[used] = '\0';
}

int ps5_probe_spawn_loader(const char *directory, int mode) {
    if (mode == 2) {
        char missing[256];
        snprintf(missing, sizeof(missing), "%s/missing-directory", directory);
        char *args[] = {"ps5-process-helper", NULL};
        char *env[] = {NULL};
        errno = 0;
        pid_t child = elfldr_spawn_env(-1, -1, -1, (uint8_t *)ps5_process_helper, args, env, missing);
        int error = errno;
        if (child > 0) {
            kill(child, SIGKILL);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        }
        int failed = child != -1 || error != ENOENT;
        printf("invalid child cwd rejected before spawn: %s errno=%d\n", failed ? "FAIL" : "PASS", error);
        return failed;
    }
    int cancel_test = mode == 1;
    int input[2], output[2], errors[2];
    if (pipe2(input, O_CLOEXEC) || pipe2(output, O_CLOEXEC) || pipe2(errors, O_CLOEXEC)) {
        perror("pipe2"); return 1;
    }
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_fd < 0) { perror("open sentinel"); return 1; }
    int sentinel = fcntl(null_fd, F_DUPFD_CLOEXEC, 300);
    close(null_fd);
    if (sentinel < 0) { perror("duplicate sentinel"); return 1; }
    char descriptor[24], cwd_env[256], pwd_env[256];
    snprintf(descriptor, sizeof(descriptor), "%d", sentinel);
    snprintf(cwd_env, sizeof(cwd_env), "PS5_PROBE_CWD=%s", directory);
    snprintf(pwd_env, sizeof(pwd_env), "PWD=%s", directory);
    char *args[] = {"ps5-process-helper", "argument with spaces", "quote'\"literal", descriptor, NULL};
    char *cancel_args[] = {"ps5-process-helper", "--wait", NULL};
    char *env[] = {"PS5_PROBE_VALUE=environment roundtrip", cwd_env, pwd_env, NULL};
    printf("loader: starting embedded SDK child (%s)\n", cancel_test ? "cancellation" : "roundtrip");
    pid_t child = elfldr_spawn_env(input[0], output[1], errors[1],
                                  (uint8_t *)ps5_process_helper, cancel_test ? cancel_args : args,
                                  env, directory);
    printf("loader: child=%ld errno=%d\n", (long)child, child < 0 ? errno : 0);
    if (child < 0) {
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        close(errors[0]); close(errors[1]); close(sentinel);
        return 1;
    }
    close(input[0]); close(output[1]); close(errors[1]);
    ssize_t written = 0;
    if (!cancel_test) {
        do { written = write(input[1], "input roundtrip\n", 16); } while (written < 0 && errno == EINTR);
    }
    close(input[1]);
    char ready[7] = {0};
    int canceled = 0;
    if (cancel_test) {
        size_t used = 0;
        while (used < 6) {
            ssize_t count = read(output[0], ready + used, 6 - used);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            used += (size_t)count;
        }
        canceled = kill(child, SIGTERM) == 0;
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    char out[2048], err[2048];
    read_text(output[0], out, sizeof(out));
    read_text(errors[0], err, sizeof(err));
    close(output[0]); close(errors[0]); close(sentinel);
    int exited = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int signaled = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    printf("loader: waited=%ld exit=%d signal=%d stdin_bytes=%ld\n",
           (long)waited, exited, signaled, (long)written);
    printf("loader stdout: %s\nloader stderr: %s\n", out, err);
    int failed;
    if (cancel_test) {
        failed = waited != child || !canceled || strcmp(ready, "ready\n") ||
                 signaled != SIGTERM || *out || *err;
        printf("loaded child ready + SIGTERM + reap: %s\n", failed ? "FAIL" : "PASS");
    } else {
        failed = waited != child || written != 16 || exited != 37 ||
                 strcmp(out, "stdout roundtrip\n") || strcmp(err, "stderr roundtrip\n");
        printf("argv + environment + cwd + descriptor isolation + pipes + exit: %s\n",
               failed ? "FAIL" : "PASS");
    }
    return failed;
}

int main(void) {
    if (mkdir("/data/ps5-ai-cli", 0700) && errno != EEXIST) return 1;
    char log_path[128], directory[128];
    snprintf(log_path, sizeof(log_path), "/data/ps5-ai-cli/os-runtime-%ld.log", (long)getpid());
    int log = open(log_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (log < 0) return 1;
    if (dup2(log, 1) < 0 || dup2(log, 2) < 0) { close(log); return 1; }
    if (log > 2) close(log);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    puts("PS5 supervised ELF-loader process probe");
    snprintf(directory, sizeof(directory), "/data/ps5-ai-cli/loader-probe-%ld", (long)getpid());
    if (mkdir(directory, 0700)) { perror("mkdir"); return 1; }
    int ready[2];
    if (pipe2(ready, O_CLOEXEC | O_NONBLOCK)) { rmdir(directory); return 1; }
    pid_t worker = fork();
    if (worker < 0) { close(ready[0]); close(ready[1]); rmdir(directory); return 1; }
    if (!worker) {
        close(ready[0]);
        if (setsid() < 0 || write(ready[1], "R", 1) != 1) _exit(90);
        close(ready[1]);
#ifdef PS5_LOADER_RUST
        int result = ps5_rust_loader_probe(directory);
#else
        int result = ps5_probe_spawn_loader(directory, 0);
        if (!result) result = ps5_probe_spawn_loader(directory, 1);
        if (!result) result = ps5_probe_spawn_loader(directory, 2);
#endif
        _exit(result);
    }
    close(ready[1]);
    int isolated = 0, status = 0, timed_out = 0;
    pid_t waited = 0;
    for (int tick = 0; tick < 300; tick++) {
        char byte;
        if (!isolated && read(ready[0], &byte, 1) == 1 && byte == 'R') isolated = 1;
        waited = waitpid(worker, &status, WNOHANG);
        if (waited == worker || (waited < 0 && errno != EINTR)) break;
        usleep(100000);
    }
    close(ready[0]);
    if (waited != worker) {
        timed_out = 1;
        /* Only the worker's acknowledged, newly created group may be targeted. */
        if (isolated) kill(-worker, SIGKILL);
        kill(worker, SIGKILL);
        do { waited = waitpid(worker, &status, 0); } while (waited < 0 && errno == EINTR);
    } else if (isolated && (!WIFEXITED(status) || WEXITSTATUS(status))) {
        /* The loader may have failed after creating its own child. */
        kill(-worker, SIGKILL);
    }
    int exited = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int removed = rmdir(directory) == 0;
    printf("Supervisor: worker=%ld isolated=%d exit=%d timeout=%d\n",
           (long)worker, isolated, exited, timed_out);
    printf("Temporary directory removed: %s\n", removed ? "yes" : "no");
    int failed = waited != worker || !isolated || exited != 0 || timed_out || !removed;
    printf("Loader probe complete: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
