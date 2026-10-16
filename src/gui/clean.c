/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "clean.h"
#include "common.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a line from at up to its \n (or end): its length without a CR before that */
static size_t line_of(const char* at, const char* end, const char** next) {
    const char* eol = memchr(at, '\n', (size_t)(end - at));
    size_t n = (size_t)((eol ? eol : end) - at);
    *next = eol ? eol + 1 : end;
    return n && at[n - 1] == '\r' ? n - 1 : n;
}
static int blank(const char* at, size_t n) {
    while (n && (*at == ' ' || *at == '\t')) ++at, --n;
    return !n;
}
static int header(const char* at, size_t n) {
    while (n && (*at == ' ' || *at == '\t')) ++at, --n;
    return n && *at == '[';
}
size_t gui_clean_deprecated(char* text, size_t size) {
    const char *at = text, *end = text + size, *next;
    char *out = text, *blanks = NULL; /* where the blank lines before this one start, in out */
    int found = 0;
    while (at < end) {
        size_t n = line_of(at, end, &next);
        if (n == 12 && !memcmp(at, "[Deprecated]", 12)) { /* it, the lines up to the next section and the blanks */
            found = 1;
            if (blanks) out = blanks;
            blanks = NULL;
            for (at = next; at < end && !header(at, line_of(at, end, &next)); at = next) {}
            continue;
        }
        if (!blank(at, n))
            blanks = NULL;
        else if (!blanks)
            blanks = out;
        memmove(out, at, (size_t)(next - at));
        out += next - at;
        at = next;
    }
    if (!found) return size;
    if (out < end) *out = 0;
    return (size_t)(out - text);
}

void gui_cleaned_add(GuiCleaned* done, bool ok, const char* file, const char* what) {
    size_t n = done->text ? strlen(done->text) : 0, add = strlen(file) + strlen(what) + 4, i;
    char* grown = (char*)realloc(done->text, n + add + 1);
    if (!grown) return;
    done->text = grown;
    snprintf(grown + n, add + 1, "%c\t%s\t%s\n", ok ? '+' : '!', file, what);
    for (i = n + 2; i < n + add - 1; ++i) /* a tab or a line's end in a name would split the entry */
        if (grown[i] == '\n' || (grown[i] == '\t' && i != n + 2 + strlen(file))) grown[i] = ' ';
    done->count++;
    done->done += ok;
}
const char* gui_cleaned_entry(const char* at, bool* ok, char* file, char* what, size_t size) {
    const char *tab, *eol;
    if (!at || !*at) return NULL;
    *ok = at[0] == '+';
    tab = strchr(at + 2, '\t');
    eol = strchr(at, '\n');
    snprintf(file, size, "%.*s", (int)(tab - at - 2), at + 2);
    snprintf(what, size, "%.*s", (int)(eol - tab - 1), tab + 1);
    return eol[1] ? eol + 1 : NULL;
}
void gui_cleaned_free(GuiCleaned* done) {
    free(done->text);
    done->text = NULL;
    done->count = done->done = 0;
}

static int ends_ini(const char* name) {
    size_t n = strlen(name);
    return n > 4 && name[n - 4] == '.' && tolower((unsigned char)name[n - 3]) == 'i' &&
           tolower((unsigned char)name[n - 2]) == 'n' && tolower((unsigned char)name[n - 1]) == 'i';
}
int gui_clean_inis(GuiCleaned* done, const char* folder) {
    char** names = NULL;
    size_t count = 0, i;
    int found = 0;
    if (!folder || snx_list_dir(folder, &names, &count)) return 0;
    for (i = 0; i < count; ++i) {
        size_t size = 0, kept;
        char *path, *text, *staged, what[300];
        if (!ends_ini(names[i])) continue;
        path = snx_path_join(folder, names[i]);
        text = snx_is_file(path) ? (char*)snx_read_file(path, &size) : NULL;
        if (!text && snx_is_file(path) && done) {
            snprintf(what, sizeof what, "Not read: %s", strerror(errno));
            gui_cleaned_add(done, false, path, what);
        }
        kept = text ? gui_clean_deprecated(text, size) : size;
        if (text && kept != size) {
            ++found;
            if (done) { /* staged beside it, then put in place: a crash mid-write leaves it as it was */
                int failed;
                staged = snx_format("%s.new", path);
                failed = !staged || snx_write_file(staged, text, kept) || snx_replace(staged, path);
                if (failed) {
                    snprintf(what, sizeof what, "Not written without [Deprecated]: %s", strerror(errno));
                    if (staged) remove(staged);
                }
                gui_cleaned_add(done, !failed, path, failed ? what : "[Deprecated] taken out");
                free(staged);
            }
        }
        free(text);
        free(path);
    }
    snx_free_strings(names, count);
    return found;
}

void gui_clean_left(GuiCleaned* done, const char* old, const char* now) {
    char what[4400];
    int failed, error = 0;
    if (!snx_path_exists(old)) return;
    if (!snx_path_exists(now)) {
        char* dir = snx_dirname(now);
        failed = !dir || snx_mkdirs(dir) || snx_replace(old, now);
        error = errno;
        /* another disk: a file is copied, then the old one deleted; a copy that failed is not left behind */
        if (failed && error == EXDEV && snx_is_file(old)) {
            failed = snx_copy_file(old, now) || remove(old);
            error = errno;
            if (failed && snx_path_exists(old)) remove(now);
        }
        free(dir);
        if (failed)
            snprintf(what, sizeof what, "Not moved to %s: %s", now, strerror(error));
        else
            snprintf(what, sizeof what, "Moved to %s", now);
    } else {
        failed = snx_is_dir(old) ? snx_remove_tree(old) : remove(old);
        error = errno;
        if (failed)
            snprintf(what, sizeof what, "Not deleted (%s holds this version's): %s", now, strerror(error));
        else
            snprintf(what, sizeof what, "Deleted: %s holds this version's", now);
    }
    gui_cleaned_add(done, !failed, old, what);
}
