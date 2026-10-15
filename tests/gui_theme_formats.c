/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Theme files keep reading the same: each tests/gui_themes/<format>/<name>.ini, a file as the theme builder
   (scarletnx.github.io/theme/) writes them for that format, reads to what <name>.json says, its id the file's name.
   A field the JSON names must keep its value; one added to themes later is not in it. docs/GUI.md, "Theme formats".

     snx-test-gui-theme-formats [DIR]       every format's fixtures in DIR (tests/gui_themes)
     snx-test-gui-theme-formats --print FILE ID   the JSON FILE reads to, for a new fixture */
#include "common.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SNX_FIXTURE_LINE = 512, SNX_FIXTURE_FIELDS = 64 }; /* a field's line, escaped; the fields of a theme */

typedef struct {
    char line[SNX_FIXTURE_FIELDS][SNX_FIXTURE_LINE];
    int count;
} Dump;

static void add(Dump* d, const char* key, const char* value) {
    if (d->count < SNX_FIXTURE_FIELDS) snprintf(d->line[d->count++], SNX_FIXTURE_LINE, "\"%s\": %s", key, value);
}
/* a JSON string of s: quotes and backslashes escaped, the bytes under 0x20 as \u00XX, the others as they are */
static void add_string(Dump* d, const char* key, const char* s) {
    char out[SNX_FIXTURE_LINE];
    size_t n = 0;
    out[n++] = '"';
    for (; *s && n + 8 < sizeof out; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20)
            n += (size_t)sprintf(out + n, "\\u%04x", c);
        else
            out[n++] = (char)c;
    }
    out[n++] = '"';
    out[n] = 0;
    add(d, key, out);
}
static void add_colour(Dump* d, const char* key, GuiRgb c) {
    char out[16];
    sprintf(out, "\"#%02X%02X%02X\"", c.r, c.g, c.b);
    add(d, key, out);
}
static void add_flag(Dump* d, const char* key, int b) { add(d, key, b ? "true" : "false"); }
static void add_number(Dump* d, const char* key, unsigned long n) {
    char out[24];
    sprintf(out, "%lu", n);
    add(d, key, out);
}
static void add_word(Dump* d, const char* key, const char* word) {
    char out[40];
    sprintf(out, "\"%s\"", word);
    add(d, key, out);
}
/* the theme as one line per field, in the theme builder's names */
static void dump(Dump* d, const GuiTheme* t) {
    static const char* const styles[] = {"plain", "bar", "none"};
    static const char* const screens[] = {"title.menu", "title.games", "title.keys", "title.compiling",
                                          "title.loading"};
    static const char* const rings[] = {"none", "left", "right", "right-near-text"};
    static const char* const entrances[] = {"none", "typing", "slide", "fade"};
    static const char* const aligns[] = {"left", "center", "right"};
    static const char* const lines[] = {"prompt.lines.keys", "prompt.lines.done", "prompt.lines.failed",
                                        "prompt.lines.aborting"};
    int i;
    d->count = 0;
    add_string(d, "id", t->id);
    add_string(d, "name", t->name);
    add_string(d, "author", t->author);
    add_number(d, "format", t->format);
    add_colour(d, "colors.page", t->page);
    add_colour(d, "colors.ink", t->ink);
    add_colour(d, "colors.soft", t->soft);
    add_colour(d, "colors.focus", t->focus);
    add_colour(d, "colors.active", t->active);
    add_colour(d, "colors.failure", t->failure);
    add_colour(d, "colors.dim", t->dim);
    add_colour(d, "colors.warning", t->warning);
    add_word(d, "head", styles[t->head]);
    add_flag(d, "head_rule", t->head_rule);
    add_word(d, "footer", styles[t->footer]);
    add_flag(d, "footer_rule", t->footer_rule);
    for (i = 0; i < GUI_SCREEN_COUNT; ++i) add_string(d, screens[i], t->title[i]);
    add_string(d, "marker", t->marker);
    add_flag(d, "focus_bar", t->focus_bar);
    add_word(d, "run", t->run == GUI_RUN_STEPS ? "steps" : "console");
    add_colour(d, "run_colors.page", t->run_colors.page);
    add_colour(d, "run_colors.ink", t->run_colors.ink);
    add_colour(d, "run_colors.soft", t->run_colors.soft);
    add_colour(d, "run_colors.dim", t->run_colors.dim);
    add_colour(d, "run_colors.failure", t->run_colors.failure);
    add_colour(d, "run_colors.warning", t->run_colors.warning);
    add_flag(d, "texture", t->texture);
    add_flag(d, "bar", t->bar);
    add_flag(d, "detailed", t->detailed);
    add_flag(d, "collapse", t->collapse);
    add_word(d, "ring", rings[t->ring]);
    add_word(d, "prompt.align", aligns[t->prompt.align]);
    add_string(d, "prompt.open", t->prompt.open);
    add_string(d, "prompt.close", t->prompt.close);
    add_flag(d, "prompt.blink", t->prompt.blink);
    for (i = 0; i < GUI_PROMPT_COUNT; ++i) add_flag(d, lines[i], t->prompt.lines[i]);
    add_word(d, "line_in", entrances[t->line_in]);
    add_flag(d, "typing_sync", t->typing_sync);
    add_number(d, "fade_in_ms", t->fade_in_ms);
    add_number(d, "fade_out_ms", t->fade_out_ms);
    add_number(d, "line_in_ms", t->line_in_ms);
    add_number(d, "typing_ms", t->typing_ms);
    add_number(d, "blink_ms", t->blink_ms);
    add_number(d, "scroll_ms", t->scroll_ms);
    add_colour(d, "splash.page", t->splash_page);
    add_number(d, "splash.ms", t->splash_ms);
    add_number(d, "splash.chance", t->splash_chance);
}
static int read_theme(GuiTheme* t, const char* file, const char* id) {
    size_t size = 0;
    unsigned char* text = snx_read_file(file, &size);
    int ok = text && gui_theme_parse(t, id, (const char*)text, size);
    free(text);
    return ok;
}
/* a JSON line of the fixture without its indent and comma */
static void bare(char* s) {
    size_t n;
    char* p = s;
    while (*p == ' ' || *p == '\t') ++p;
    memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && (s[n - 1] == ',' || s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
}
/* one fixture: every field its JSON names has the value the file reads to */
static int check(const char* dir, const char* name, int format) {
    char json_name[SNX_FIXTURE_LINE], id[SNX_FIXTURE_LINE];
    char *ini_path, *json_path, *json, *line, *next;
    size_t size = 0, n = strlen(name) - 4;
    GuiTheme t;
    static Dump d;
    int i, fails = 0, fields = 0;
    snprintf(id, sizeof id, "%.*s", (int)n, name);
    snprintf(json_name, sizeof json_name, "%s.json", id);
    ini_path = snx_path_join(dir, name);
    json_path = snx_path_join(dir, json_name);
    json = (char*)snx_read_file(json_path, &size);
    if (!json || !read_theme(&t, ini_path, id)) {
        printf("FAIL %s/%s: cannot read it or its .json\n", dir, name);
        free(json);
        free(ini_path);
        free(json_path);
        return 1;
    }
    if ((int)t.format != format) {
        printf("FAIL %s/%s: format %u in the folder of format %d\n", dir, name, t.format, format);
        ++fails;
    }
    dump(&d, &t);
    for (line = json; line && *line; line = next) {
        char want[SNX_FIXTURE_LINE];
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        snprintf(want, sizeof want, "%s", line);
        bare(want);
        if (!want[0] || !strcmp(want, "{") || !strcmp(want, "}")) continue;
        ++fields;
        for (i = 0; i < d.count && strcmp(d.line[i], want); ++i) {}
        if (i == d.count) {
            printf("FAIL %s/%s: %s, but the GUI reads it otherwise\n", dir, name, want);
            ++fails;
        }
    }
    if (!fails) printf("ok   %s/%s (%d fields)\n", dir, name, fields);
    free(json);
    free(ini_path);
    free(json_path);
    return fails;
}

int main(int argc, char** argv) {
    char** formats = NULL;
    size_t count = 0, f, k;
    int fails = 0, files = 0;
    const char* root = argc > 1 ? argv[1] : SNX_GUI_THEME_FIXTURES;
    if (argc == 4 && !strcmp(argv[1], "--print")) {
        GuiTheme t;
        Dump d;
        int i;
        if (!read_theme(&t, argv[2], argv[3])) return 2;
        dump(&d, &t);
        puts("{");
        for (i = 0; i < d.count; ++i) printf("%s%s\n", d.line[i], i + 1 < d.count ? "," : "");
        puts("}");
        return 0;
    }
    if (snx_list_dir(root, &formats, &count)) {
        printf("FAIL no fixtures in %s\n", root);
        return 1;
    }
    for (f = 0; f < count; ++f) {
        char** names = NULL;
        size_t found = 0;
        char* dir = snx_path_join(root, formats[f]);
        int format = atoi(formats[f]);
        if (format > 0 && snx_is_dir(dir) && !snx_list_dir(dir, &names, &found)) {
            for (k = 0; k < found; ++k) {
                size_t n = strlen(names[k]);
                if (n > 4 && !strcmp(names[k] + n - 4, ".ini")) {
                    fails += check(dir, names[k], format);
                    ++files;
                }
            }
            snx_free_strings(names, found);
        }
        free(dir);
    }
    snx_free_strings(formats, count);
    if (!files) {
        printf("FAIL no fixtures in %s\n", root);
        return 1;
    }
    if (fails)
        printf("%d differences: a theme file of a format this GUI reads no longer reads as it did. The GUI keeps every "
               "format's files reading the same (docs/GUI.md, Theme formats): read them so again, never change the "
               "fixtures\n",
               fails);
    else
        printf("theme formats: %d files read as they did\n", files);
    return fails ? 1 : 0;
}
