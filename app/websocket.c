/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "websocket.h"
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#define SHA1(data, length, digest) CC_SHA1(data, (CC_LONG)(length), digest)
#else
#include <openssl/sha.h>
#endif

int terminal_upgrade(int client, const struct ui_request *r) {
    char input[61], encoded[29], response[256];
    unsigned char digest[20];
    snprintf(input, sizeof(input), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", r->key);
    SHA1((const unsigned char *)input, strlen(input), digest);
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = 0;
    for (size_t i = 0; i < sizeof(digest); i += 3) {
        unsigned n = (unsigned)digest[i] << 16;
        if (i + 1 < sizeof(digest)) n |= (unsigned)digest[i + 1] << 8;
        if (i + 2 < sizeof(digest)) n |= digest[i + 2];
        encoded[out++] = alphabet[(n >> 18) & 63];
        encoded[out++] = alphabet[(n >> 12) & 63];
        encoded[out++] = i + 1 < sizeof(digest) ? alphabet[(n >> 6) & 63] : '=';
        encoded[out++] = i + 2 < sizeof(digest) ? alphabet[n & 63] : '=';
    }
    encoded[out] = 0;
    int n = snprintf(response, sizeof(response), "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", encoded);
    return n < 0 || (size_t)n >= sizeof(response) ? -1 : ui_write(client, response, (size_t)n);
}

int terminal_frame(int client, unsigned opcode, const void *data, size_t length) {
    unsigned char header[4] = {(unsigned char)(0x80 | opcode), (unsigned char)length};
    size_t size = 2;
    if (length > 65535) return -1;
    if (length >= 126) { header[1] = 126; header[2] = length >> 8; header[3] = length; size = 4; }
    return ui_write(client, header, size) || ui_write(client, data, length) ? -1 : 0;
}

static int read_exact(int fd, void *data, size_t length, time_t deadline) {
    unsigned char *p = data;
    while (length) {
        int remaining = (int)(deadline - time(NULL));
        struct pollfd wait = {.fd = fd, .events = POLLIN};
        if (remaining <= 0) return -1;
        int ready = poll(&wait, 1, remaining * 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t n = recv(fd, p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

int terminal_read_frame(int client, unsigned char *data, size_t capacity, size_t *length) {
    unsigned char header[2], mask[4], extended[2];
    time_t deadline = time(NULL) + 5;
    if (read_exact(client, header, 2, deadline)) return -1;
    unsigned opcode = header[0] & 15;
    /* Bounded independent frames. Browser input is chunked below this limit. */
    if ((header[0] & 0xf0) != 0x80 || !(header[1] & 0x80) ||
        (opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10)) return -1;
    size_t size = header[1] & 127;
    if (size == 127 || (opcode >= 8 && size >= 126)) return -1;
    if (size == 126) {
        if (read_exact(client, extended, 2, deadline)) return -1;
        size = ((size_t)extended[0] << 8) | extended[1];
        if (size < 126) return -1;
    }
    if (size > capacity || (opcode == 8 && size == 1) || read_exact(client, mask, 4, deadline) ||
        read_exact(client, data, size, deadline)) return -1;
    for (size_t i = 0; i < size; i++) data[i] ^= mask[i % 4];
    *length = size;
    return (int)opcode;
}
