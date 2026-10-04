/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../app/native-terminal.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

extern int __wrap_isatty(int);
extern int __wrap_tcgetattr(int, struct termios *);
extern int __wrap_tcsetattr(int, int, const struct termios *);
extern int __wrap_ioctl(int, unsigned long, ...);

int main(int argc, char **argv) {
    signal(SIGWINCH, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "closed-stdio")) {
        close(0); close(1); close(2);
    }
    if (ps5_terminal_prepare()) return 10;
    /* Gateway descriptors must survive installation of the CLI streams. */
    int sentinel = open("/dev/null", O_RDONLY);
    if (sentinel <= 2) return 11;
    int terminal = ps5_terminal_attach(100, 30);
    if (terminal <= 2 || ps5_terminal_wait() || fcntl(sentinel, F_GETFD) < 0) return 12;
    int control[2] = {-1, -1};
    if (argc > 1 && !strcmp(argv[1], "child-control")) {
        struct winsize initial = {.ws_col = 100, .ws_row = 30};
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, control) ||
            send(control[0], &initial, sizeof(initial), 0) != sizeof(initial) ||
            ps5_terminal_adopt(control[1]) || !(fcntl(control[1], F_GETFD) & FD_CLOEXEC)) return 21;
    }
    if (!__wrap_isatty(0) || !__wrap_isatty(1) || __wrap_isatty(sentinel)) return 13;
    char buffer[4];
    if (write(terminal, "in", 2) != 2 || read(0, buffer, 2) != 2 || memcmp(buffer, "in", 2)) return 14;
    if (write(1, "out", 3) != 3 || read(terminal, buffer, 3) != 3 || memcmp(buffer, "out", 3)) return 15;
    struct termios attributes;
    if (__wrap_tcgetattr(0, &attributes) || __wrap_tcsetattr(0, TCSANOW, &attributes)) return 16;
    attributes.c_lflag |= ICANON;
    if (__wrap_tcsetattr(0, TCSANOW, &attributes) != -1 || errno != EOPNOTSUPP) return 17;
    ps5_terminal_resize(90, 40);
    if (control[0] >= 0) {
        struct winsize latest = {.ws_col = 90, .ws_row = 40};
        ps5_terminal_resize(80, 24);
        if (send(control[0], &latest, sizeof(latest), 0) != sizeof(latest)) return 22;
    }
    struct winsize window;
    if (__wrap_ioctl(1, TIOCGWINSZ, &window) || window.ws_col != 90 || window.ws_row != 40) return 18;
    if (ps5_terminal_attach(90, 40) != -1 || errno != EBUSY) return 19;
    close(terminal); ps5_terminal_detach();
    terminal = ps5_terminal_attach(80, 24);
    if (terminal < 0 || write(terminal, "ok", 2) != 2 || read(0, buffer, 2) != 2 || memcmp(buffer, "ok", 2)) return 20;
    close(terminal); close(sentinel);
    if (control[0] >= 0) { close(control[0]); close(control[1]); }
    return 0;
}
