/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_AI_GATEWAY_H
#define PS5_AI_GATEWAY_H
#include <stddef.h>
#include <time.h>
struct ui_config {
    unsigned short port;
    const char *pair_code;
    int fixture;
    int loopback_only;
    /* One running CLI; an attachment owns a dup of its terminal descriptor. */
    int (*attach)(unsigned columns, unsigned rows);
    void (*resize)(unsigned columns, unsigned rows);
    void (*detach)(void);
};
/* Binds synchronously, then serves on detached native threads. */
int ps5_ui_start(const struct ui_config *config);
#endif
