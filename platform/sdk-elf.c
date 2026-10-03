/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bound every extent read by shsrv's loader before it touches a child process.
 * Read little-endian fields explicitly so host tests need no target ELF headers.
 */
#include "sdk-spawn.h"
#include <string.h>

static uint64_t le(const uint8_t *p, size_t n) {
    uint64_t value = 0;
    for (size_t i = 0; i < n; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static int extent(uint64_t offset, uint64_t length, uint64_t limit) {
    return offset <= limit && length <= limit - offset;
}
static const uint8_t *mapped(const uint8_t *image, uint64_t phoff, unsigned count,
                             uint64_t address, uint64_t length, int file_backed) {
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *p = image + phoff + 56 * i;
        uint64_t start = le(p + 16, 8), size = le(p + (file_backed ? 32 : 40), 8);
        if (le(p, 4) == 1 && address >= start && extent(address - start, length, size))
            return image + (file_backed ? le(p + 8, 8) + address - start : 0);
    }
    return NULL;
}
int ps5_sdk_elf_valid(const uint8_t *image, size_t length) {
    if (!image || length < 64 || length > 256U * 1024 * 1024 ||
        memcmp(image, "\177ELF\2\1\1", 7) || le(image + 16, 2) != 3 ||
        le(image + 18, 2) != 62 || le(image + 20, 4) != 1 || le(image + 52, 2) != 64)
        return 0;
    uint64_t entry = le(image + 24, 8), phoff = le(image + 32, 8), shoff = le(image + 40, 8);
    unsigned phnum = (unsigned)le(image + 56, 2), shnum = (unsigned)le(image + 60, 2);
    if (le(image + 54, 2) != 56 || !phnum || phnum > 128 || phoff < 64 ||
        !extent(phoff, phnum * 56, length) || le(image + 58, 2) != 64 || !shnum ||
        shnum > 8192 || shoff < 64 || !extent(shoff, shnum * 64, length)) return 0;
    int zero_base = 0, entry_ok = 0;
    const uint8_t *dynamic = NULL;
    uint64_t dynamic_size = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *p = image + phoff + 56 * i;
        uint64_t kind = le(p, 4), flags = le(p + 4, 4), offset = le(p + 8, 8);
        uint64_t va = le(p + 16, 8), file = le(p + 32, 8), memory = le(p + 40, 8);
        uint64_t align = le(p + 48, 8);
        if (kind == 3 || kind == 7 || file > memory || !extent(offset, file, length) ||
            !extent(va, memory, 1024U * 1024 * 1024)) return 0;
        if (kind == 1) {
            if (align < 0x4000 || (align & (align - 1)) || (va & 0x3fff) ||
                ((offset - va) & (align - 1))) return 0;
            zero_base |= va == 0 && memory != 0;
            entry_ok |= (flags & 1) && entry >= va && entry - va < file;
        }
        if (kind == 2) {
            if (dynamic || !file || file % 16) return 0;
            dynamic = image + offset;
            dynamic_size = file;
        }
    }
    if (!zero_base || !entry_ok || !dynamic) return 0;
    uint64_t strings = 0, strings_size = 0, end = 0;
    for (; end < dynamic_size; end += 16) {
        uint64_t tag = le(dynamic + end, 8), value = le(dynamic + end + 8, 8);
        if (!tag) break;
        if (tag == 5) strings = value;
        if (tag == 10) strings_size = value;
    }
    if (end == dynamic_size || !strings_size) return 0;
    const uint8_t *strtab = mapped(image, phoff, phnum, strings, strings_size, 1);
    if (!strtab) return 0;
    int kernel = 0;
    for (uint64_t pos = 0; pos < end; pos += 16) {
        if (le(dynamic + pos, 8) != 1) continue;
        uint64_t offset = le(dynamic + pos + 8, 8);
        if (offset >= strings_size) return 0;
        const char *name = (const char *)strtab + offset;
        const char *nul = memchr(name, 0, strings_size - offset);
        if (!nul || nul - name < 9 || memcmp(name, "lib", 3) || strcmp(nul - 5, ".sprx")) return 0;
        for (const char *p = name + 3; p < nul - 5; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) return 0;
        kernel |= !strcmp(name, "libkernel_web.sprx") || !strcmp(name, "libkernel_sys.sprx");
    }
    if (!kernel) return 0;
    uint64_t symbol_count = 0;
    for (unsigned i = 0; i < shnum; i++) {
        const uint8_t *s = image + shoff + 64 * i;
        if (le(s + 4, 4) != 11) continue;
        uint64_t offset = le(s + 24, 8), size = le(s + 32, 8);
        if (symbol_count || le(s + 56, 8) != 24 || size % 24 || !extent(offset, size, length)) return 0;
        symbol_count = size / 24;
        for (uint64_t pos = 0; pos < size; pos += 24) {
            uint64_t name = le(image + offset + pos, 4);
            if (name >= strings_size || !memchr(strtab + name, 0, strings_size - name)) return 0;
        }
    }
    for (unsigned i = 0; i < shnum; i++) {
        const uint8_t *s = image + shoff + 64 * i;
        uint64_t kind = le(s + 4, 4), offset = le(s + 24, 8), size = le(s + 32, 8);
        if (kind != 8 && !extent(offset, size, length)) return 0;
        if (kind == 9) return 0; /* REL is not supported by this loader. */
        if (kind != 4) continue;
        if (le(s + 56, 8) != 24 || size % 24) return 0;
        for (uint64_t pos = 0; pos < size; pos += 24) {
            const uint8_t *r = image + offset + pos;
            uint64_t info = le(r + 8, 8), type = info & 0xffffffff;
            if (!type) continue;
            /* The loader applies RELATIVE; SDK startup resolves GLOB_DAT and
             * JUMP_SLOT imports from its own dynamic symbol table. */
            if ((type != 8 && type != 6 && type != 7) ||
                (type == 8 ? info >> 32 != 0 : info >> 32 >= symbol_count) ||
                !mapped(image, phoff, phnum, le(r, 8), 8, 0)) return 0;
        }
    }
    return 1;
}
