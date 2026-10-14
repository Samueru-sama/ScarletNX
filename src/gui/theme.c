/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "theme.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>

static const GuiTheme builtins[] = {
    {
        .id = "default",
        .author = "ScarletNX",
        .format = GUI_THEME_FORMAT,
        .warning = {255, 170, 0},
        .name = "Default",
        .page = {0, 0, 0},
        .ink = {220, 220, 220},
        .soft = {160, 170, 180},
        .focus = {255, 255, 0},
        .active = {255, 85, 255},
        .failure = {255, 0, 0},
        .dim = {80, 80, 80},
        .head = GUI_STYLE_PLAIN,
        .footer = GUI_STYLE_PLAIN,
        .marker = "- ",
        .run_colors = {.page = {0, 0, 0},
                       .ink = {220, 220, 220},
                       .soft = {160, 170, 180},
                       .dim = {80, 80, 80},
                       .failure = {255, 0, 0},
                       .warning = {255, 170, 0}},
        .typing_ms = 25,
        .blink_ms = 500,
        .prompt = {.align = GUI_ALIGN_CENTER,
                   .open = "[ ",
                   .close = " ]",
                   .blink = 1,
                   .lines = {[GUI_PROMPT_KEYS] = 1}},
    },
    {
        .id = "yorha",
        .author = "ScarletNX",
        .format = GUI_THEME_FORMAT,
        .warning = {0x8A, 0x6A, 0x2A},
        .name = "YoRHa",
        .page = {0xDA, 0xD6, 0xBE},
        .ink = {0x45, 0x41, 0x3A},
        .soft = {0x8A, 0x86, 0x76},
        .focus = {0x45, 0x41, 0x3A},
        .active = {0x8A, 0x6A, 0x2A},
        .failure = {0xA5, 0x47, 0x3C},
        .dim = {0xB4, 0xB0, 0x9C},
        .head = GUI_STYLE_BAR,
        .footer = GUI_STYLE_BAR,
        .head_rule = 1,
        .focus_bar = 1,
        .marker = "\xE2\x96\xA0 ", /* U+25A0, the font's square */
        .title = {"SCARLETNX  /  MENU", "SCARLETNX  /  GAMES", "SCARLETNX  /  KEYS", "COMPILING", "BOOTING"},
        .run = GUI_RUN_STEPS,
        .run_colors = {.page = {0x14, 0x14, 0x16},
                       .ink = {0xE6, 0xE4, 0xDC},
                       .soft = {0x8E, 0x8C, 0x86},
                       .dim = {0x3E, 0x3E, 0x40},
                       .failure = {0xD2, 0x46, 0x3A},
                       .warning = {0xD9, 0xB2, 0x4A}},
        .texture = 1,
        .detailed = 1,
        .ring = GUI_RING_RIGHT,
        .line_in = GUI_LINE_TYPING,
        .fade_in_ms = 250,
        .fade_out_ms = 500,
        .line_in_ms = 220,
        .typing_ms = 5,
        .blink_ms = 500,
        .prompt = {.align = GUI_ALIGN_CENTER,
                   .open = "[ ",
                   .close = " ]",
                   .blink = 1,
                   .lines = {[GUI_PROMPT_KEYS] = 1}},
    },
};
/* a theme's id fits a slot of the theme list: an array of negative size otherwise */
typedef char snx_gui_id_slot[sizeof(((GuiThemes*)0)->ids[0]) == sizeof(((GuiTheme*)0)->id) ? 1 : -1];
static const char* const screens[GUI_SCREEN_COUNT] = {"menu", "games", "keys", "compiling", "loading"};
static const char* const rings[] = {"none", "left", "right", "right-near-text"};

const GuiTheme* gui_theme_builtin(const char* id) {
    size_t i;
    for (i = 0; id && i < sizeof(builtins) / sizeof(*builtins); ++i)
        if (!strcmp(builtins[i].id, id)) return &builtins[i];
    return NULL;
}

static char* trim(char* s) {
    size_t n;
    while (*s == ' ' || *s == '\t') ++s;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}
/* "quoted" keeps its spaces */
static char* unquote(char* s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = 0;
        return s + 1;
    }
    return s;
}
static int copy(char* out, size_t size, const char* value) {
    if (strlen(value) >= size) return 0;
    strcpy(out, value);
    return 1;
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int colour(GuiRgb* out, const char* value) {
    int i;
    uint8_t v[3];
    if (strlen(value) != 7 || value[0] != '#') return 0;
    for (i = 0; i < 3; ++i) {
        int hi = hex(value[1 + 2 * i]), lo = hex(value[2 + 2 * i]);
        if (hi < 0 || lo < 0) return 0;
        v[i] = (uint8_t)(hi * 16 + lo);
    }
    out->r = v[0];
    out->g = v[1];
    out->b = v[2];
    return 1;
}
static int style(GuiStyle* out, const char* value) {
    if (!strcmp(value, "plain"))
        *out = GUI_STYLE_PLAIN;
    else if (!strcmp(value, "bar"))
        *out = GUI_STYLE_BAR;
    else if (!strcmp(value, "none"))
        *out = GUI_STYLE_NONE;
    else
        return 0;
    return 1;
}
static int flag(int* out, const char* value) {
    if (!strcmp(value, "true"))
        *out = 1;
    else if (!strcmp(value, "false"))
        *out = 0;
    else
        return 0;
    return 1;
}
/* a whole number, 0 to 10000 */
static int integer(uint32_t* out, const char* value) {
    char* end;
    unsigned long n = strtoul(value, &end, 10);
    if (!*value || *end || n > 10000) return 0;
    *out = (uint32_t)n;
    return 1;
}
static int word(int* out, const char* value, const char* const* words, int count) {
    int i;
    for (i = 0; i < count; ++i)
        if (!strcmp(value, words[i])) {
            *out = i;
            return 1;
        }
    return 0;
}
static void palette(GuiPalette* p, const char* key, const char* value) {
    size_t i;
    static const size_t roles[] = {offsetof(GuiPalette, page),    offsetof(GuiPalette, ink),
                                   offsetof(GuiPalette, soft),    offsetof(GuiPalette, dim),
                                   offsetof(GuiPalette, failure), offsetof(GuiPalette, warning)};
    static const char* const names[] = {"page", "ink", "soft", "dim", "failure", "warning"};
    for (i = 0; i < sizeof roles / sizeof *roles; ++i)
        if (!strcmp(key, names[i])) colour((GuiRgb*)((char*)p + roles[i]), value);
}
/* run_colors: an ErET loading theme's [colors] are its loading screen's, here the run's */
static void apply(GuiTheme* t, const char* section, const char* key, const char* value, int run_colors) {
    uint32_t number;
    if (!strcmp(section, "theme") && !strcmp(key, "name"))
        copy(t->name, sizeof t->name, value);
    else if (!strcmp(section, "theme") && !strcmp(key, "author"))
        copy(t->author, sizeof t->author, value);
    else if (!strcmp(section, "theme") && !strcmp(key, "format")) {
        if (integer(&number, value) && number) t->format = number;
    } else if (!strcmp(section, "colors") && run_colors) {
        if (!strcmp(key, "texture")) flag(&t->texture, value);
        palette(&t->run_colors, key, value);
    } else if (!strcmp(section, "colors")) {
        size_t i;
        static const size_t roles[] = {offsetof(GuiTheme, page),   offsetof(GuiTheme, ink),
                                       offsetof(GuiTheme, soft),   offsetof(GuiTheme, focus),
                                       offsetof(GuiTheme, active), offsetof(GuiTheme, failure),
                                       offsetof(GuiTheme, dim),    offsetof(GuiTheme, warning)};
        static const char* const names[] = {"page", "ink", "soft", "focus", "active", "failure", "dim", "warning"};
        if (!strcmp(key, "texture")) flag(&t->texture, value); /* where ErET keeps it */
        for (i = 0; i < sizeof roles / sizeof *roles; ++i)
            if (!strcmp(key, names[i])) colour((GuiRgb*)((char*)t + roles[i]), value);
    } else if (!strcmp(section, "head") || !strcmp(section, "footer")) {
        int i;
        int head = section[0] == 'h';
        if (!strcmp(key, "style"))
            style(head ? &t->head : &t->footer, value);
        else if (!strcmp(key, "rule"))
            flag(head ? &t->head_rule : &t->footer_rule, value);
        else
            for (i = 0; head && i < GUI_SCREEN_COUNT; ++i)
                if (!strcmp(key, screens[i])) copy(t->title[i], sizeof t->title[i], value);
    } else if (!strcmp(section, "body")) {
        if (!strcmp(key, "marker"))
            copy(t->marker, sizeof t->marker, value);
        else if (!strcmp(key, "focus") && (!strcmp(value, "bar") || !strcmp(value, "color")))
            t->focus_bar = value[0] == 'b';
    } else if (!strcmp(section, "run")) {
        static const char* const layouts[] = {"console", "steps"};
        int n;
        if (!strcmp(key, "layout") && word(&n, value, layouts, 2))
            t->run = (GuiRunLayout)n;
        else if (!strcmp(key, "ring") && word(&n, value, rings, 4))
            t->ring = (GuiRing)n;
        else if (!strcmp(key, "texture"))
            flag(&t->texture, value);
        else if (!strcmp(key, "bar"))
            flag(&t->bar, value);
        else if (!strcmp(key, "detailed"))
            flag(&t->detailed, value);
        else if (!strcmp(key, "collapse"))
            flag(&t->collapse, value);
        else
            palette(&t->run_colors, key, value);
    } else if (!strcmp(section, "transitions")) {
        static const char* const entrances[] = {"none", "typing", "slide", "fade"};
        int n;
        if (!strcmp(key, "line_in") && word(&n, value, entrances, 4))
            t->line_in = (GuiLineIn)n;
        else if (!strcmp(key, "fade_in_ms"))
            integer(&t->fade_in_ms, value);
        else if (!strcmp(key, "fade_out_ms"))
            integer(&t->fade_out_ms, value);
        else if (!strcmp(key, "line_in_ms"))
            integer(&t->line_in_ms, value);
        else if (!strcmp(key, "typing_ms"))
            integer(&t->typing_ms, value);
        else if (!strcmp(key, "blink_ms"))
            integer(&t->blink_ms, value);
        else if (!strcmp(key, "typing") && (!strcmp(value, "async") || !strcmp(value, "sync")))
            t->typing_sync = value[0] == 's';
    } else if (!strcmp(section, "header") && !strcmp(key, "ring_side")) {
        /* ErET's words; none is ours */
        static const char* const sides[] = {"right", "left", "right-near-text"};
        static const GuiRing as[] = {GUI_RING_RIGHT, GUI_RING_LEFT, GUI_RING_NEAR_TEXT};
        int n;
        if (word(&n, value, sides, 3)) t->ring = as[n];
    } else if (!strcmp(section, "prompt")) {
        static const char* const aligns[] = {"left", "center", "right"};
        static const char* const lines[GUI_PROMPT_COUNT] = {"keys", "done", "failed", "aborting"};
        int n;
        if (!strcmp(key, "align") && word(&n, value, aligns, 3))
            t->prompt.align = (GuiAlign)n;
        else if (!strcmp(key, "open"))
            copy(t->prompt.open, sizeof t->prompt.open, value);
        else if (!strcmp(key, "close"))
            copy(t->prompt.close, sizeof t->prompt.close, value);
        else if (!strcmp(key, "blink"))
            flag(&t->prompt.blink, value);
        else if (word(&n, key, lines, GUI_PROMPT_COUNT))
            flag(&t->prompt.lines[n], value);
    } else if (!strcmp(section, "loading") && !strcmp(key, "title")) {
        copy(t->title[GUI_SCREEN_LOADING], sizeof t->title[GUI_SCREEN_LOADING], value); /* ErET's boot word */
    }
}
/* each key = value of text, with its section, to visit; false when out of memory */
/* ErET's layout as the built-in to start from, when no base names one */
static const GuiTheme* layout_base(const char* layout) {
    if (!strcmp(layout, "menu")) return gui_theme_builtin("yorha");
    if (!strcmp(layout, "loading")) return gui_theme_builtin("yorha");
    if (!strcmp(layout, "plain") || !strcmp(layout, "debug") || !strcmp(layout, "skip"))
        return gui_theme_builtin("default");
    return NULL;
}
static int visit(const char* text, size_t size, GuiTheme* theme, const GuiTheme** bases, int base_only,
                  int run_colors) {
    char section[32];
    char* line;
    char* next;
    char* copy_of = malloc(size + 1);
    if (!copy_of) return 0;
    memcpy(copy_of, text, size);
    copy_of[size] = 0;
    memset(section, 0, sizeof section);
    for (line = copy_of; line; line = next) {
        char *key;
        char * value;
        char * equals;
        char * s;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            if (end && (size_t)(end - s - 1) < sizeof section) {
                memcpy(section, s + 1, (size_t)(end - s - 1));
                section[end - s - 1] = 0;
            } else
                section[0] = 0;
            continue;
        }
        equals = strchr(s, '=');
        if (!equals) continue;
        *equals = 0;
        key = trim(s); value = unquote(trim(equals + 1));
        if (base_only) {
            if (!strcmp(section, "theme") && !strcmp(key, "base") && gui_theme_builtin(value))
                bases[0] = gui_theme_builtin(value);
            else if (!strcmp(section, "theme") && !strcmp(key, "layout") && layout_base(value)) {
                bases[1] = layout_base(value);
                bases[2] = !strcmp(value, "loading") ? bases[1] : NULL;
            }
        } else
            apply(theme, section, key, value, run_colors);
    }
    free(copy_of);
    return 1;
}
int gui_theme_parse(GuiTheme* theme, const char* id, const char* text, size_t size) {
    const GuiTheme* bases[3] = {NULL, NULL, NULL}; /* the base, ErET's layout, set when that is loading */
    if (!visit(text, size, theme, bases, 1, 0)) return 0;
    *theme = bases[0] ? *bases[0] : bases[1] ? *bases[1] : builtins[0];
    copy(theme->id, sizeof theme->id, id);
    copy(theme->name, sizeof theme->name, id);
    return visit(text, size, theme, bases, 0, !bases[0] && bases[2]);
}
void gui_theme_load(GuiTheme* theme, const char* const* dirs, size_t count, const char* id) {
    size_t i;
    const GuiTheme* builtin;
    for (i = 0; i < count; ++i) {
        char* folder = dirs[i] ? snx_path_join(dirs[i], id) : NULL;
        char* path = folder ? snx_path_join(folder, "theme.ini") : NULL;
        size_t size = 0;
        unsigned char* text = path ? snx_read_file(path, &size) : NULL;
        int read = text && gui_theme_parse(theme, id, (const char*)text, size);
        free(text);
        free(path);
        free(folder);
        if (read) return;
    }
    builtin = gui_theme_builtin(id);
    *theme = builtin ? *builtin : builtins[0];
}

void gui_themes_free(GuiThemes* themes) {
    free(themes->ids);
    themes->ids = NULL;
    themes->count = 0;
}
static int listed(const GuiThemes* themes, const char* id) {
    size_t i;
    for (i = 0; i < themes->count; ++i)
        if (!strcmp(themes->ids[i], id)) return 1;
    return 0;
}
void gui_themes_scan(GuiThemes* themes, const char* const* dirs, size_t count) {
    size_t total, capacity, i, d;
    gui_themes_free(themes);
    total = sizeof(builtins) / sizeof(*builtins);
    capacity = total;
    themes->ids = calloc(capacity, sizeof *themes->ids);
    if (!themes->ids) return;
    for (i = 0; i < total; ++i) memcpy(themes->ids[themes->count++], builtins[i].id, sizeof *themes->ids);
    for (d = 0; d < count; ++d) {
        char** names = NULL;
        size_t found = 0;
        char (*grown)[sizeof *themes->ids];
        if (!dirs[d] || snx_list_dir(dirs[d], &names, &found)) continue; /* no such folder: nothing from it */
        grown = realloc(themes->ids, (capacity + found) * sizeof *themes->ids);
        if (!grown) {
            snx_free_strings(names, found);
            continue;
        }
        themes->ids = grown;
        capacity += found;
        for (i = 0; i < found; ++i) {
            char* folder = snx_path_join(dirs[d], names[i]);
            char* file = folder ? snx_path_join(folder, "theme.ini") : NULL;
            int usable = names[i][0] != '.' && !listed(themes, names[i]) && strlen(names[i]) < sizeof *themes->ids &&
                          file && snx_is_file(file);
            free(file);
            free(folder);
            if (usable) snprintf(themes->ids[themes->count++], sizeof *themes->ids, "%s", names[i]);
        }
        snx_free_strings(names, found);
    }
}
