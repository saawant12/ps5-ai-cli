#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ps5/kernel.h>

extern int ps5_rust_probe(void);

struct notification {
    char reserved[45];
    char message[3075];
};
_Static_assert(sizeof(struct notification) == 3120, "SDK notification ABI");
int sceKernelSendNotificationRequest(int, struct notification *, size_t, int);

static void notify(const char *message) {
    struct notification request = {0};
    snprintf(request.message, sizeof(request.message), "%s", message);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mkdir("/data/ps5-ai-cli", 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "Cannot create probe directory: %s\n", strerror(errno));
        notify("PS5 Rust probe: cannot create /data/ps5-ai-cli");
        return 1;
    }
    char path[128];
    snprintf(path, sizeof(path), "/data/ps5-ai-cli/rust-runtime-%ld.log", (long)getpid());
    int log = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (log < 0) {
        fprintf(stderr, "Cannot create probe log: %s\n", strerror(errno));
        return 1;
    }
    int loader_out = dup(STDOUT_FILENO);
    int loader_err = dup(STDERR_FILENO);
    if (loader_out < 0 || loader_err < 0) {
        close(log);
        if (loader_out >= 0) close(loader_out);
        if (loader_err >= 0) close(loader_err);
        return 1;
    }
    if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) {
        dup2(loader_out, STDOUT_FILENO);
        dup2(loader_err, STDERR_FILENO);
        close(loader_out);
        close(loader_err);
        close(log);
        return 1;
    }
    close(log);
    notify("PS5 Rust probe started. Log: /data/ps5-ai-cli/");
    puts("PS5 AI CLI: experimental Rust std runtime probe, target 13.60.");
    printf("Detected firmware: 0x%08x\n", kernel_get_fw_version());
    int result = ps5_rust_probe();
    printf("Probe completed with status %d\n", result);
    fflush(NULL);
    dup2(loader_out, STDOUT_FILENO);
    dup2(loader_err, STDERR_FILENO);
    close(loader_out);
    close(loader_err);
    printf("PS5 Rust probe status %d; log: %s\n", result, path);
    notify(result == 0 ? "PS5 Rust probe passed. This is a runtime test, not Codex yet."
                       : "PS5 Rust probe failed. Check the log in /data/ps5-ai-cli/");
    return result;
}
