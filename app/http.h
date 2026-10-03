/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_AI_HTTP_H
#define PS5_AI_HTTP_H
#include <stddef.h>
struct ui_request {
    char method[12], path[256], host[128], origin[160];
    char cookie[2048], key[32], upgrade[32], connection[128], client[16];
    char body[128];
    size_t length;
    int version;
};
int ui_read_request(int fd, struct ui_request *request);
int ui_write(int fd, const void *data, size_t size);
void ui_reply(int fd, int status, const char *type, const void *body, size_t length, const char *extra);
int ui_host_allowed(int fd, const char *host, unsigned short port);
int ui_same_origin(const struct ui_request *request);
int ui_connect(unsigned short port);
void ui_socket_options(int fd);
#endif
