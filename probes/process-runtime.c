/* Bounded fork/pipe and exec feasibility tests, restricted to our own files. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../build/process-helper.h"

extern char **environ;

static int write_all(int fd, const void *data, size_t length) {
    const char *next = data;
    while (length) {
        ssize_t count = write(fd, next, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        next += count;
        length -= (size_t)count;
    }
    return 0;
}

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

/* mode 0 tests fork + pipes; 1 calls execve; 2 calls execvp; 3 tests cancellation. */
static int run_case(const char *label, int mode, const char *program,
                    const char *directory, int expected_errno) {
    int pipes[4][2] = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};
    int sentinel = -1;
    int result = 1;
    for (int i = 0; i < 4; i++) {
        if (pipe2(pipes[i], O_CLOEXEC)) goto cleanup;
    }
    if (mode == 3) {
        int flags = fcntl(pipes[3][0], F_GETFL);
        if (flags < 0 || fcntl(pipes[3][0], F_SETFL, flags | O_NONBLOCK)) goto cleanup;
    }
    sentinel = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (sentinel < 0) goto cleanup;
    char descriptor[24], cwd_env[256];
    snprintf(descriptor, sizeof(descriptor), "%d", sentinel);
    snprintf(cwd_env, sizeof(cwd_env), "PS5_PROBE_CWD=%s", directory);
    char *args[] = {(char *)program, "argument with spaces", "quote'\"literal", descriptor, NULL};
    char *env[] = {"PS5_PROBE_VALUE=environment roundtrip", cwd_env, NULL};
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) goto cleanup;
    if (child == 0) {
        close(pipes[0][1]);
        for (int i = 1; i < 4; i++) close(pipes[i][0]);
        int error = 0;
        if (dup2(pipes[0][0], STDIN_FILENO) < 0 ||
            dup2(pipes[1][1], STDOUT_FILENO) < 0 ||
            dup2(pipes[2][1], STDERR_FILENO) < 0 || chdir(directory)) {
            error = errno;
        } else {
            close(pipes[0][0]);
            close(pipes[1][1]);
            close(pipes[2][1]);
            if (mode == 0) {
                char input[64];
                read_text(STDIN_FILENO, input, sizeof(input));
                if (strcmp(input, "input roundtrip\n")) _exit(82);
                write_all(STDOUT_FILENO, "stdout roundtrip\n", 17);
                write_all(STDERR_FILENO, "stderr roundtrip\n", 17);
                _exit(37);
            }
            if (mode == 3) {
                write_all(pipes[3][1], "R", 1);
                for (;;) pause();
            }
            if (mode == 1) execve(program, args, env);
            else { environ = env; execvp(program, args); }
            error = errno;
        }
        write_all(pipes[3][1], &error, sizeof(error));
        _exit(127);
    }
    close(pipes[0][0]); pipes[0][0] = -1;
    for (int i = 1; i < 4; i++) { close(pipes[i][1]); pipes[i][1] = -1; }
    if (mode != 3) write_all(pipes[0][1], "input roundtrip\n", 16);
    close(pipes[0][1]); pipes[0][1] = -1;
    int status = 0, timed_out = 0, cancel_ready = 0;
    pid_t waited = 0;
    for (int tick = 0; tick < 100; tick++) {
        if (mode == 3 && !cancel_ready) {
            char ready;
            if (read(pipes[3][0], &ready, 1) == 1 && ready == 'R') {
                cancel_ready = 1;
                kill(child, SIGTERM);
            }
        }
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno != EINTR)) break;
        usleep(100000);
    }
    if (waited != child) {
        timed_out = 1;
        kill(child, SIGKILL);
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    }
    char output[2048], errors[2048];
    read_text(pipes[1][0], output, sizeof(output));
    read_text(pipes[2][0], errors, sizeof(errors));
    int exec_error = 0;
    ssize_t error_bytes = mode == 3 ? 0 : read(pipes[3][0], &exec_error, sizeof(exec_error));
    int exited = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int signal_number = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    if (mode == 3) result = timed_out || waited != child || !cancel_ready || signal_number != SIGTERM;
    else if (expected_errno) result = timed_out || waited != child ||
        error_bytes != (ssize_t)sizeof(exec_error) || exec_error != expected_errno || exited != 127;
    else result = timed_out || waited != child || error_bytes != 0 || exited != 37 ||
        strcmp(output, "stdout roundtrip\n") || strcmp(errors, "stderr roundtrip\n");
    printf("%s: %s exit=%d signal=%d exec_errno=%d error_bytes=%ld timeout=%d\n",
           label, result ? "FAIL" : "PASS", exited, signal_number, exec_error,
           (long)error_bytes, timed_out);
    if (*output) printf("  stdout: %s", output);
    if (*errors) printf("  stderr: %s", errors);
cleanup:
    for (int i = 0; i < 4; i++) for (int j = 0; j < 2; j++) {
        if (pipes[i][j] >= 0) close(pipes[i][j]);
    }
    if (sentinel >= 0) close(sentinel);
    return result;
}

static int create_file(const char *path, const void *data, size_t length) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0700);
    if (fd < 0) return -1;
    int result = write_all(fd, data, length);
    if (close(fd)) result = -1;
    if (result) unlink(path);
    return result;
}

int main(void) {
    if (mkdir("/data/ps5-ai-cli", 0700) && errno != EEXIST) return 1;
    char log_path[128], directory[128], helper[160], missing[160];
    snprintf(log_path, sizeof(log_path), "/data/ps5-ai-cli/os-runtime-%ld.log", (long)getpid());
    int log = open(log_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (log < 0) return 1;
    if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) { close(log); return 1; }
    if (log > STDERR_FILENO) close(log);
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    puts("PS5 process execution probe: fork/pipe control, native exec and cancellation");
    snprintf(directory, sizeof(directory), "/data/ps5-ai-cli/process-probe-%ld", (long)getpid());
    if (mkdir(directory, 0700)) { perror("mkdir probe workspace"); return 1; }
    snprintf(helper, sizeof(helper), "%s/child.elf", directory);
    snprintf(missing, sizeof(missing), "%s/missing.elf", directory);
    if (create_file(helper, ps5_process_helper, sizeof(ps5_process_helper))) {
        perror("create child ELF"); rmdir(directory); return 1;
    }
    int failures = 0;
    failures += run_case("fork + stdin/stdout/stderr + exit 37", 0, helper, directory, 0);
    failures += run_case("execve missing path", 1, missing, directory, ENOENT);
    failures += run_case("execvp missing path", 2, missing, directory, ENOENT);
    failures += run_case("execve native SDK ELF", 1, helper, directory, 0);
    failures += run_case("execvp native SDK ELF", 2, helper, directory, 0);
    failures += run_case("cancel fork child + reap", 3, helper, directory, 0);
    int cleanup_ok = unlink(helper) == 0 && rmdir(directory) == 0;
    printf("Temporary files removed: %s\n", cleanup_ok ? "yes" : "no");
    printf("Process probe complete: %d failed cases\n", failures);
    return failures != 0 || !cleanup_ok;
}
