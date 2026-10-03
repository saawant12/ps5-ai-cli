/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Structural screening, not a sandbox or a signature/trust check. */
int ps5_sdk_elf_valid(const uint8_t *image, size_t length);
pid_t ps5_spawn_sdk(const char *path, char **argv, char **envp, const char *cwd,
                    int input, int output, int error, const int *fds,
                    size_t fd_count, int process_mode);

/* Only for a single-threaded shell's forked command process. On success this
 * proxy waits and exits with the loaded child's status; it never returns. */
int ps5_shell_execve(const char *path, char *const argv[], char *const envp[]);
