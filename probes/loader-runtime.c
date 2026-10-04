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
#include "../platform/sdk-spawn.h"
#include "../build/process-helper.h"
#ifdef PS5_LOADER_SHELL
#include "../build/shell-probe.h"
#endif

#ifdef PS5_LOADER_SHELL
static int install_file(const char *directory, const char *name, const uint8_t *data, size_t size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used < size) {
        ssize_t n = write(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        used += (size_t)n;
    }
    return close(fd);
}
static int install_shell(const char *directory) {
    if (!ps5_sdk_elf_valid(ps5_shell_probe, sizeof(ps5_shell_probe)) ||
        !ps5_sdk_elf_valid(ps5_tools_probe, sizeof(ps5_tools_probe))) { errno = ENOEXEC; return -1; }
    if (install_file(directory, "sh", ps5_shell_probe, sizeof(ps5_shell_probe))) return -1;
    if (install_file(directory, "helper", ps5_process_helper, sizeof(ps5_process_helper))) return -1;
    for (const char **name = ps5_tool_names; *name; name++)
        if (install_file(directory, *name, ps5_tools_probe, sizeof(ps5_tools_probe))) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/mkdir", directory);
    struct stat info;
    int result = stat(path, &info);
    printf("Tool metadata: stat=%d mode=%o access_x=%d\n", result,
           result ? 0 : (unsigned)info.st_mode, access(path, X_OK));
    return 0;
}
static int remove_shell(const char *directory) {
    char path[512];
    int failed = 0;
    snprintf(path, sizeof(path), "%s/sh", directory);
    failed |= unlink(path) != 0;
    snprintf(path, sizeof(path), "%s/helper", directory);
    failed |= unlink(path) != 0;
    for (const char **name = ps5_tool_names; *name; name++) {
        snprintf(path, sizeof(path), "%s/%s", directory, *name);
        failed |= unlink(path) != 0;
    }
    const char *task_files[] = {"task-input", "task-expected", "task-edited", NULL};
    for (const char **name = task_files; *name; name++) {
        snprintf(path, sizeof(path), "%s/%s", directory, *name);
        if (unlink(path) && errno != ENOENT) failed = 1;
    }
    const char *outputs[] = {"input", "output", "copied", "moved", "script", "from-xargs", NULL};
    for (const char **name = outputs; *name; name++) {
        snprintf(path, sizeof(path), "%s/tool-dir/%s", directory, *name);
        if (unlink(path) && errno != ENOENT) failed = 1;
    }
    snprintf(path, sizeof(path), "%s/tool-dir", directory);
    if (rmdir(path) && errno != ENOENT) failed = 1;
    return failed;
}
#endif

#ifdef PS5_LOADER_RUST
extern int ps5_rust_loader_probe(const char *directory);
#endif

int ps5_probe_has_shell(void) {
#ifdef PS5_LOADER_SHELL
    return 1;
#else
    return 0;
#endif
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

/* Drain both streams while the child runs. Waiting before reading can deadlock
 * even a healthy command when its diagnostic output fills the pipe. */
struct capture {
    int fd, eof, failed, overflow;
    char *text;
    size_t used, capacity;
};

static void capture_available(struct capture *capture) {
    for (int batch = 0; batch < 16 && !capture->eof; batch++) {
        char bytes[1024];
        ssize_t count = read(capture->fd, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (count <= 0) {
            capture->eof = 1;
            capture->failed = count < 0;
            return;
        }
        size_t copy = (size_t)count;
        size_t remaining = capture->capacity - capture->used - 1;
        if (copy > remaining) { copy = remaining; capture->overflow = 1; }
        memcpy(capture->text + capture->used, bytes, copy);
        capture->used += copy;
        capture->text[capture->used] = '\0';
    }
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
    int cancel_test = mode == 1 || mode == 4;
    int input[2], output[2], errors[2], attachment[2];
    if (pipe2(input, O_CLOEXEC) || pipe2(output, O_CLOEXEC) || pipe2(errors, O_CLOEXEC) ||
        pipe2(attachment, O_CLOEXEC)) {
        perror("pipe2"); return 1;
    }
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_fd < 0) { perror("open sentinel"); return 1; }
    int sentinel = fcntl(null_fd, F_DUPFD_CLOEXEC, 300);
    close(null_fd);
    if (sentinel < 0) { perror("duplicate sentinel"); return 1; }
    char descriptor[24], cwd_env[256], pwd_env[256], attachment_env[64];
    snprintf(descriptor, sizeof(descriptor), "%d", sentinel);
    snprintf(cwd_env, sizeof(cwd_env), "PS5_PROBE_CWD=%s", directory);
    snprintf(pwd_env, sizeof(pwd_env), "PWD=%s", directory);
    snprintf(attachment_env, sizeof(attachment_env), "PS5_PROBE_ATTACHMENT=%d", attachment[1]);
    char *args[] = {"ps5-process-helper", "argument with spaces", "quote'\"literal", descriptor, NULL};
    char *cancel_args[] = {"ps5-process-helper", mode == 4 ? "--wait-tree" : "--wait", NULL};
    char path_env[512], shell_env[512];
    snprintf(path_env, sizeof(path_env), "PATH=%s", directory);
    snprintf(shell_env, sizeof(shell_env), "SHELL=%s/sh", directory);
    char *env[] = {"PS5_PROBE_VALUE=environment roundtrip", cwd_env, pwd_env, path_env, shell_env,
                   mode == 3 ? attachment_env : NULL, NULL};
    uint8_t *image = (uint8_t *)ps5_process_helper;
    char **command_args = cancel_test ? cancel_args : args;
#ifdef PS5_LOADER_SHELL
    char *shell_args[] = {"sh", "-c",
        "[ \"$1\" = 'argument with spaces' ] || exit 80; "
        "[ \"$PS5_PROBE_VALUE\" = 'environment roundtrip' ] || exit 81; "
        "[ \"$PWD\" = \"$PS5_PROBE_CWD\" ] || exit 82; "
        "IFS= read -r value; [ \"$value\" = 'input roundtrip' ] || exit 83; "
        "printf 'quoted value\\n' > shell-roundtrip.txt; "
        "IFS= read -r value < shell-roundtrip.txt; "
        "[ \"$value\" = 'quoted value' ] || exit 84; "
        "value=$(printf '%s' \"$value\"); [ \"$value\" = 'quoted value' ] || exit 85; "
        "printf 'pipeline\\n' | { IFS= read -r item; [ \"$item\" = pipeline ]; } || exit 86; "
        "printf 'stdout roundtrip\\n'; printf 'stderr roundtrip\\n' >&2; exit 37",
        "ps5-shell-probe", "argument with spaces", NULL};
    if (mode == 5) { image = (uint8_t *)ps5_shell_probe; command_args = shell_args; }
    char *tool_args[] = {"sh", "-lc",
        "set -e; IFS= read -r value; [ \"$value\" = 'input roundtrip' ]; "
        "mkdir tool-dir; [ -d tool-dir ]; cd tool-dir; cd ..; "
        "printf 'alpha\\nbeta\\n' > tool-dir/input; "
        "cat tool-dir/input | grep -e '^beta$' | sed 's/beta/gamma/' > tool-dir/output; "
        "IFS= read -r value < tool-dir/output; [ \"$value\" = gamma ]; "
        "cp tool-dir/output tool-dir/copied; mv tool-dir/copied tool-dir/moved; "
        "[ \"$(find tool-dir -name moved)\" = tool-dir/moved ]; "
        "grep missing tool-dir/input > /dev/null && exit 90; [ \"$?\" = 1 ]; "
        "missing-command-for-probe 2>/dev/null && exit 91; [ \"$?\" = 127 ]; "
        "printf 'printf script-ok' > tool-dir/script; chmod 700 tool-dir/script; "
        "[ \"$(./tool-dir/script)\" = script-ok ]; "
        "printf '%s\\n' tool-dir/input | xargs cat > tool-dir/from-xargs; "
        "cmp tool-dir/input tool-dir/from-xargs; "
        "rm tool-dir/input tool-dir/output tool-dir/moved tool-dir/script tool-dir/from-xargs; "
        "rmdir tool-dir; [ ! -d tool-dir ]; "
        "printf 'stdout roundtrip\\n'; printf 'stderr roundtrip\\n' >&2; exit 37",
        "ps5-tools-probe", NULL};
    if (mode == 6) { image = (uint8_t *)ps5_shell_probe; command_args = tool_args; }
#endif
    printf("loader: starting embedded SDK child (%s)\n", cancel_test ? "cancellation" : "roundtrip");
    pid_t child = elfldr_spawn_context(input[0], output[1], errors[1],
                                  image, command_args,
                                  env, directory, mode == 3 ? &attachment[1] : NULL,
                                  mode == 3 ? 1 : 0, mode >= 3 ? 2 : 0);
    printf("loader: child=%ld errno=%d\n", (long)child, child < 0 ? errno : 0);
    if (child < 0) {
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        close(errors[0]); close(errors[1]); close(sentinel);
        close(attachment[0]); close(attachment[1]);
        return 1;
    }
    close(input[0]); close(output[1]); close(errors[1]);
    close(attachment[1]);
    int group_ok = mode < 3 || getpgid(child) == child;
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
        canceled = kill(mode == 4 ? -child : child, SIGTERM) == 0;
    }
    char out[2048] = {0}, err[16384] = {0};
    struct capture stdout_capture = {.fd = output[0], .text = out, .capacity = sizeof(out)};
    struct capture stderr_capture = {.fd = errors[0], .text = err, .capacity = sizeof(err)};
    if (fcntl(output[0], F_SETFL, fcntl(output[0], F_GETFL) | O_NONBLOCK) ||
        fcntl(errors[0], F_SETFL, fcntl(errors[0], F_GETFL) | O_NONBLOCK)) {
        perror("nonblocking output capture");
        stdout_capture.failed = stderr_capture.failed = 1;
        stdout_capture.eof = stderr_capture.eof = 1;
        kill(child, SIGKILL);
    }
    int status = 0;
    pid_t waited = 0;
    int timed_out = 0;
    /* The tool script makes many serialized native launches; its overall
     * budget must exceed a single command's 30-second execution budget. */
    const int max_ticks = mode == 6 ? 12000 : 3000;
    for (int tick = 0; tick < max_ticks; tick++) {
        capture_available(&stdout_capture);
        capture_available(&stderr_capture);
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno != EINTR)) break;
        usleep(10000);
    }
    if (waited != child) {
        timed_out = 1;
        /* Ownership remains here until the blocking wait consumes the exit. */
        if (group_ok && mode >= 3) kill(-child, SIGKILL);
        kill(child, SIGKILL);
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    }
    for (int tick = 0; tick < 100 && (!stdout_capture.eof || !stderr_capture.eof); tick++) {
        capture_available(&stdout_capture);
        capture_available(&stderr_capture);
        if (!stdout_capture.eof || !stderr_capture.eof) usleep(10000);
    }
    char attached[64];
    read_text(attachment[0], attached, sizeof(attached));
    close(output[0]); close(errors[0]); close(sentinel); close(attachment[0]);
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
    failed |= stdout_capture.failed || stderr_capture.failed ||
              stdout_capture.overflow || stderr_capture.overflow ||
              !stdout_capture.eof || !stderr_capture.eof;
    if (mode == 3) {
        int context_ok = group_ok && !strcmp(attached, "attached\n");
        printf("new session + explicit CLOEXEC attachment: %s\n", context_ok ? "PASS" : "FAIL");
        failed |= !context_ok;
    }
    if (mode == 4) {
        int gone = 0;
        for (int tick = 0; tick < 200; tick++) {
            if (kill(-child, 0) < 0 && errno == ESRCH) { gone = 1; break; }
            usleep(10000);
        }
        printf("whole command group removed: %s\n", group_ok && gone ? "PASS" : "FAIL");
        failed |= !group_ok || !gone;
    }
    if (mode == 5) {
        char file[256];
        snprintf(file, sizeof(file), "%s/shell-roundtrip.txt", directory);
        int removed = unlink(file) == 0;
        failed |= !removed;
        printf("native shell quoting + files + substitution + pipeline: %s\n",
               failed ? "FAIL" : "PASS");
    }
    if (mode == 6) {
        printf("native external tools + pipelines + scripts + failure statuses: %s\n",
               failed ? "FAIL" : "PASS");
    }
    return failed || timed_out;
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
#ifdef PS5_LOADER_SHELL
    if (install_shell(directory)) { perror("install shell probe"); return 1; }
#endif
#ifdef PS5_LOADER_DIRECT
    /* Exercise the production shape: initialize Rust in the payload process,
     * rather than forking a process whose SDK may already own native threads. */
    signal(SIGALRM, SIG_DFL);
    alarm(180);
    int result = ps5_rust_loader_probe(directory);
    alarm(0);
#ifdef PS5_LOADER_SHELL
    result |= remove_shell(directory);
#endif
    int removed = rmdir(directory) == 0;
    printf("Direct runtime: result=%d temporary_directory_removed=%d\n", result, removed);
    return result || !removed;
#else
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
        if (!result) result = ps5_probe_spawn_loader(directory, 3);
        if (!result) result = ps5_probe_spawn_loader(directory, 4);
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
#endif
}
