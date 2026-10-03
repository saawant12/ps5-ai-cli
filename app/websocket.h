/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_AI_WEBSOCKET_H
#define PS5_AI_WEBSOCKET_H
#include "http.h"
int terminal_upgrade(int client, const struct ui_request *request);
int terminal_frame(int client, unsigned opcode, const void *data, size_t length);
/* Returns opcode, -1 for disconnect/protocol error. Browser frames are masked. */
int terminal_read_frame(int client, unsigned char *data, size_t capacity, size_t *length);
#endif
