/* Invoke the real upstream CLI's --version path with persistent diagnostics. */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

extern int __real_main(int argc, char **argv);
extern int ps5_init_executable_path(void);
#ifdef PS5_APP_SERVER
#include "../build/session-auth.h"
#define LOG_PREFIX "codex-service"
#else
#define LOG_PREFIX "codex-version"
#endif

int __wrap_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (mkdir("/data/ps5-ai-cli", 0700) && errno != EEXIST) return 1;
    char path[128];
    snprintf(path, sizeof(path), "/data/ps5-ai-cli/" LOG_PREFIX "-%ld.log", (long)getpid());
    int log = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (log < 0) return 1;
    if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) {
        close(log);
        return 1;
    }
    if (log > STDERR_FILENO) close(log);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    puts("PS5 native Codex " LOG_PREFIX ": entering the upstream CLI.");
    /* elfldr does not supply a Unix home directory. Define this application's
     * home only in its own process, before Rust starts any threads.
     */
    if (mkdir("/data/ps5-ai-cli/home", 0700) && errno != EEXIST) return 1;
    if (setenv("HOME", "/data/ps5-ai-cli/home", 0)) return 1;
#ifdef PS5_APP_SERVER
    if (ps5_init_executable_path()) {
        perror("Codex install path is unavailable; run the path installer first");
        return 1;
    }
    char *args[] = {"codex", "app-server", "--listen", "ws://0.0.0.0:49321",
                    "--ws-auth", "capability-token", "--ws-token-sha256", PS5_SESSION_TOKEN_SHA256,
                    "-c", "cli_auth_credentials_store=\"file\"",
                    "-c", "mcp_oauth_credentials_store=\"file\"", NULL};
    int result = __real_main((int)(sizeof(args) / sizeof(args[0]) - 1), args);
#else
    char name[] = "codex";
    char version[] = "--version";
    char *args[] = {name, version, NULL};
    /* Clap may exit directly after printing the version. */
    int result = __real_main(2, args);
#endif
    printf("Codex returned %d\n", result);
    return result;
}
