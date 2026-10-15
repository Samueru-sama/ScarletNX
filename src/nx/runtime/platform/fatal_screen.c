/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/platform/fatal_screen.h"
#include "font.h"
#include <string.h>

const char* fatal_hint(const char* msg) {
    if (strstr(msg, "GPU device was lost"))
        return "The GPU stopped answering and its driver reset it: something the game gave it, a shader or a draw, "
               "ran too long or read memory it should not.";
    if (strstr(msg, "Unsupported instruction")) return "The game ran an instruction ScarletNX does not translate yet.";
    if (strstr(msg, "Vulkan 1.3")) return "ScarletNX needs a GPU and driver with Vulkan 1.3, and this one has none.";
    if (strstr(msg, "out of memory") || strstr(msg, "Out of memory"))
        return "The device ran out of memory: close other apps and start the game again.";
    return NULL;
}

/* the bytes of the code point starting at s: a lead byte's continuations, three at most; any other byte alone */
static size_t step(const char* s) {
    size_t n = 1;
    if ((unsigned char)s[0] >= 0xC0)
        while (n < 4 && s[n] && ((unsigned char)s[n] & 0xC0) == 0x80) ++n;
    return n;
}

int fatal_wrap(const char* text, int cols, char (*rows)[FATAL_ROW], int max) {
    int count = 0;
    if (cols < 1) cols = 1;
    while (*text) {
        const char* end = text;
        const char* space = NULL;
        size_t n;
        int k;
        while (*text == ' ') ++text; /* a row starts at a word */
        if (!*text) break;
        for (end = text, k = 0; *end && k < cols; ++k) {
            if (*end == ' ') space = end;
            end += step(end);
        }
        /* the row ends at its last space when the next character would not fit, and a word fills it otherwise */
        if (*end && *end != ' ' && space) end = space;
        n = (size_t)(end - text);
        if (count < max) {
            if (n >= FATAL_ROW) n = FATAL_ROW - 1;
            memcpy(rows[count], text, n);
            rows[count][n] = 0;
            while (n && rows[count][n - 1] == ' ') rows[count][--n] = 0;
        }
        ++count;
        text = end;
    }
    return count;
}

void fatal_cp437(const char* text, char* out, size_t size) {
    size_t n = 0;
    while (*text && n + 1 < size) {
        const unsigned char* s = (const unsigned char*)text;
        size_t len = step(text);
        unsigned long cp = s[0];
        if (s[0] >= 0xF0 && len == 4)
            cp = (s[0] & 0x07ul) << 18 | (s[1] & 0x3Ful) << 12 | (s[2] & 0x3Ful) << 6 | (s[3] & 0x3Ful);
        else if (s[0] >= 0xE0 && len == 3)
            cp = (s[0] & 0x0Ful) << 12 | (s[1] & 0x3Ful) << 6 | (s[2] & 0x3Ful);
        else if (s[0] >= 0xC0 && len == 2)
            cp = (s[0] & 0x1Ful) << 6 | (s[1] & 0x3Ful);
        else if (s[0] >= 0x80)
            cp = 0xFFFD; /* not UTF-8 */
        out[n++] = (char)gui_glyph((uint32_t)cp);
        text += len;
    }
    out[n] = 0;
}
