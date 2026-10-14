/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GUI themes: the built-ins, a theme.ini over its base, and the themes/ folders the GUI offers. */
#include "common.h"
#include "theme.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool same(GuiRgb a, GuiRgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static void parse(GuiTheme* theme, const char* text) { assert(gui_theme_parse(theme, "mine", text, strlen(text))); }
static void write(const char* dir, const char* name, const char* text) {
    char* folder = snx_path_join(dir, name);
    char* file = snx_path_join(folder, "theme.ini");
    assert(!snx_mkdirs(folder));
    if (text) assert(!snx_write_file(file, text, strlen(text)));
    free(file);
    free(folder);
}

int main(void) {
    const GuiTheme *def = gui_theme_builtin("default"), *yorha = gui_theme_builtin("yorha");
    const GuiTheme* loading = yorha; /* YoRHa's runs are its loading screen */
    assert(def && yorha && !gui_theme_builtin("yorha-loading") && !gui_theme_builtin("nothing") &&
           !gui_theme_builtin(NULL));
    assert(same(yorha->run_colors.page, (GuiRgb){0x14, 0x14, 0x16}) && !same(yorha->run_colors.page, yorha->page));
    assert(same(def->run_colors.page, def->page) && same(def->run_colors.ink, def->ink));
    assert(def->run == GUI_RUN_CONSOLE && !def->fade_in_ms && !def->fade_out_ms && def->line_in == GUI_LINE_NONE);
    assert(loading->run == GUI_RUN_STEPS && loading->ring == GUI_RING_RIGHT && loading->texture && !loading->bar);
    assert(loading->line_in == GUI_LINE_TYPING && loading->typing_ms == 5 && loading->fade_out_ms == 500);
    assert(loading->detailed && !loading->collapse && !def->detailed && !def->collapse);
    for (const GuiTheme* b = def; b; b = b == def ? loading : NULL) {
        const GuiPrompt* p = &b->prompt;
        assert(p->align == GUI_ALIGN_CENTER && !strcmp(p->open, "[ ") && !strcmp(p->close, " ]") && p->blink);
        assert(p->lines[GUI_PROMPT_KEYS] && !p->lines[GUI_PROMPT_DONE] && !p->lines[GUI_PROMPT_FAILED] &&
               !p->lines[GUI_PROMPT_ABORTING]);
    }
    assert(!strcmp(loading->title[GUI_SCREEN_LOADING], "BOOTING"));
    assert(def->head == GUI_STYLE_PLAIN && !def->focus_bar && !strcmp(def->marker, "- ") && !def->title[0][0]);
    assert(yorha->head == GUI_STYLE_BAR && yorha->focus_bar && yorha->title[GUI_SCREEN_COMPILING][0]);

    GuiTheme theme;
    parse(&theme, ""); /* nothing: the default, named after its folder */
    assert(same(theme.ink, def->ink) && !strcmp(theme.id, "mine") && !strcmp(theme.name, "mine"));

    /* the base first, wherever it is, then each key over it; what cannot be read keeps the base's */
    parse(&theme, "; a comment\r\n[colors]\r\npage = #0a0B0c\r\nink = #12345\r\nsoft = # 12 34\r\nfocus = #GG0000\r\n"
                  "[theme]\r\nname = Mine\r\nbase = yorha\r\n# another\r\n[head]\r\nstyle = none\r\nrule = maybe\r\n"
                  "loading = \"  BOOT  \"\r\nmenu =\r\n[footer]\r\nstyle = plain\r\nrule = true\r\n"
                  "[body]\r\nmarker = \"* \"\r\nfocus = color\r\n[nowhere]\r\nink = #FFFFFF\r\nstray line\r\n");
    assert(!strcmp(theme.name, "Mine") && !strcmp(theme.id, "mine"));
    assert(same(theme.page, (GuiRgb){0x0A, 0x0B, 0x0C}));
    assert(same(theme.ink, yorha->ink) && same(theme.soft, yorha->soft) && same(theme.focus, yorha->focus));
    assert(theme.head == GUI_STYLE_NONE && theme.head_rule == yorha->head_rule);
    assert(theme.footer == GUI_STYLE_PLAIN && theme.footer_rule && !theme.focus_bar && !strcmp(theme.marker, "* "));
    assert(!strcmp(theme.title[GUI_SCREEN_LOADING], "  BOOT  ") && !theme.title[GUI_SCREEN_MENU][0]);
    assert(!strcmp(theme.title[GUI_SCREEN_COMPILING], yorha->title[GUI_SCREEN_COMPILING]));

    /* the run's layout and its transitions; a time past 10 s, or not a number, keeps the base's */
    parse(&theme,
          "[theme]\nbase = yorha\n[run]\nlayout = console\nring = left\ntexture = false\nbar = true\npage = #050505\n"
          "detailed = false\ncollapse = true\n[transitions]\nline_in = slide\nline_in_ms = 300\nfade_in_ms = 10001\n"
          "fade_out_ms = 5s\ntyping_ms = 0\nblink_ms = -1\n[colors]\ndim = #010203\n");
    assert(theme.run == GUI_RUN_CONSOLE && theme.ring == GUI_RING_LEFT && !theme.texture && theme.bar);
    assert(!theme.detailed && theme.collapse);
    assert(theme.line_in == GUI_LINE_SLIDE && theme.line_in_ms == 300 && theme.typing_ms == 0);
    assert(theme.fade_in_ms == loading->fade_in_ms && theme.fade_out_ms == loading->fade_out_ms);
    assert(theme.blink_ms == loading->blink_ms && same(theme.dim, (GuiRgb){1, 2, 3}));
    assert(same(theme.run_colors.page, (GuiRgb){5, 5, 5}) && same(theme.run_colors.ink, yorha->run_colors.ink));
    assert(same(theme.page, yorha->page));

    /* an ErET file: its layout picks the base when no base does, ErET's own keys apply, the rest waits for it */
    parse(&theme, "[theme]\nformat = 2\nname = From rEden\nauthor = rEden\nlayout = menu\n[colors]\ntexture = true\n"
                  "warning = #D9B24A\n[header]\nring_side = right-near-text\n[transitions]\ntyping = sync\n"
                  "[loading]\ntitle = WAKING\nheader = SYSTEM  /  LOADING\n[plain]\nstage_build = %1 / %2\n"
                  "[sizes]\ntitle_scale = 0.75\n");
    assert(same(theme.page, yorha->page) && theme.format == 2 && !strcmp(theme.author, "rEden"));
    assert(theme.texture && theme.ring == GUI_RING_NEAR_TEXT && theme.typing_sync);
    assert(same(theme.warning, (GuiRgb){0xD9, 0xB2, 0x4A}) && !strcmp(theme.title[GUI_SCREEN_LOADING], "WAKING"));
    /* ErET's loading layout is YoRHa's runs: its [colors] are the run's, the menus keep YoRHa's */
    parse(&theme, "[theme]\nlayout = loading\n[colors]\npage = #101010\nink = #F0F0F0\ntexture = false\n");
    assert(same(theme.run_colors.page, (GuiRgb){16, 16, 16}) && same(theme.run_colors.ink, (GuiRgb){240, 240, 240}));
    assert(same(theme.page, yorha->page) && same(theme.ink, yorha->ink) && !theme.texture &&
           theme.run == GUI_RUN_STEPS);
    parse(&theme, "[theme]\nbase = yorha\nlayout = loading\n[colors]\npage = #101010\n"); /* a base: menus' */
    assert(same(theme.page, (GuiRgb){16, 16, 16}) && same(theme.run_colors.page, yorha->run_colors.page));
    parse(&theme, "[theme]\nbase = default\nlayout = menu\nformat = 0\n"); /* base wins; no format 0 */
    assert(same(theme.page, def->page) && theme.format == GUI_THEME_FORMAT);
    parse(&theme, "[header]\nring_side = nowhere\n[run]\nring = right-near-text\n[transitions]\ntyping = later\n");
    assert(theme.ring == GUI_RING_NEAR_TEXT && !theme.typing_sync);

    /* the lines that wait on the user: each one's switch, and their look; what cannot be read keeps the base's */
    parse(&theme, "[prompt]\nalign = right\nopen = \"<< \"\nclose = \"\"\nblink = false\nfailed = true\n"
                  "aborting = true\nkeys = maybe\ndone = false\nnowhere = true\n");
    assert(theme.prompt.align == GUI_ALIGN_RIGHT && !strcmp(theme.prompt.open, "<< ") && !theme.prompt.close[0]);
    assert(!theme.prompt.blink && theme.prompt.lines[GUI_PROMPT_FAILED] && theme.prompt.lines[GUI_PROMPT_ABORTING]);
    assert(theme.prompt.lines[GUI_PROMPT_KEYS] && !theme.prompt.lines[GUI_PROMPT_DONE]);
    parse(&theme, "[prompt]\nalign = middle\nopen = \"[[[[[[[[[[[[[[[[[[[[\"\nkeys = false\n");
    assert(theme.prompt.align == GUI_ALIGN_CENTER && !strcmp(theme.prompt.open, "[ ") &&
           !theme.prompt.lines[GUI_PROMPT_KEYS]);
    parse(&theme, "[prompt]\nalign = left\n");
    assert(theme.prompt.align == GUI_ALIGN_LEFT);

    /* a value longer than its field keeps the base's; an unknown base leaves the default */
    char longer[200] = "[theme]\nbase = elsewhere\n[body]\nmarker = ";
    memset(longer + strlen(longer), 'x', 40);
    parse(&theme, longer);
    assert(!strcmp(theme.marker, def->marker) && same(theme.page, def->page));

    char* dir = snx_temp_dir(NULL);
    char* second = snx_temp_dir(NULL);
    assert(dir && second);
    write(dir, "alpha", "[theme]\nbase = yorha\nname = Alpha\n");
    write(dir, "beta", NULL); /* a folder without theme.ini */
    write(dir, ".hidden", "[theme]\n");
    write(dir, "yorha", "[theme]\nbase = yorha\n[colors]\npage = #FFFFFF\n"); /* replaces the built-in */
    write(second, "alpha", "[theme]\nname = Alpha too\n");                    /* the first folder's wins */
    write(second, "gamma", "[theme]\nname = Gamma\n");
    const char* dirs[] = {dir, NULL, second};
    GuiThemes themes = {0};
    gui_themes_scan(&themes, dirs, 3);
    assert(themes.count == 4 && !strcmp(themes.ids[0], "default") && !strcmp(themes.ids[1], "yorha") &&
           !strcmp(themes.ids[2], "alpha") && !strcmp(themes.ids[3], "gamma"));
    gui_theme_load(&theme, dirs, 3, "alpha");
    assert(!strcmp(theme.name, "Alpha") && same(theme.page, yorha->page));
    gui_theme_load(&theme, dirs, 3, "gamma");
    assert(!strcmp(theme.name, "Gamma") && same(theme.page, def->page));
    gui_theme_load(&theme, dirs, 3, "yorha");
    assert(same(theme.page, (GuiRgb){255, 255, 255}) && same(theme.ink, yorha->ink));
    gui_theme_load(&theme, dirs, 3, "beta");
    assert(!strcmp(theme.id, "default"));
    gui_themes_free(&themes);
    char* missing = snx_path_join(dir, "missing");
    const char* none[] = {missing};
    gui_themes_scan(&themes, none, 1); /* no themes/ folder: the built-ins */
    assert(themes.count == 2);
    gui_themes_free(&themes);
    free(missing);
    assert(!snx_remove_tree(dir) && !snx_remove_tree(second));
    free(dir);
    free(second);

    /* the versioned theme files are the built-ins, key for key */
    static const char* const ids[] = {"default", "yorha"};
    const char* shipped[] = {SNX_GUI_THEMES_DIR};
    for (size_t i = 0; i < sizeof ids / sizeof *ids; ++i) {
        const GuiTheme* b = gui_theme_builtin(ids[i]);
        GuiTheme f;
        gui_theme_load(&f, shipped, 1, ids[i]);
        assert(!strcmp(f.id, b->id) && !strcmp(f.name, b->name) && !strcmp(f.marker, b->marker));
        assert(same(f.page, b->page) && same(f.ink, b->ink) && same(f.soft, b->soft) && same(f.focus, b->focus));
        assert(same(f.active, b->active) && same(f.failure, b->failure) && same(f.dim, b->dim));
        assert(f.head == b->head && f.footer == b->footer && f.head_rule == b->head_rule);
        assert(f.footer_rule == b->footer_rule && f.focus_bar == b->focus_bar && f.run == b->run);
        assert(f.texture == b->texture && f.bar == b->bar && f.ring == b->ring && f.line_in == b->line_in);
        assert(f.fade_in_ms == b->fade_in_ms && f.fade_out_ms == b->fade_out_ms && f.line_in_ms == b->line_in_ms);
        assert(f.typing_ms == b->typing_ms && f.blink_ms == b->blink_ms && f.typing_sync == b->typing_sync);
        assert(f.detailed == b->detailed && f.collapse == b->collapse);
        assert(f.prompt.align == b->prompt.align && !strcmp(f.prompt.open, b->prompt.open));
        assert(!strcmp(f.prompt.close, b->prompt.close) && f.prompt.blink == b->prompt.blink);
        for (int l = 0; l < GUI_PROMPT_COUNT; ++l) assert(f.prompt.lines[l] == b->prompt.lines[l]);
        assert(f.format == GUI_THEME_FORMAT && b->format == GUI_THEME_FORMAT && !strcmp(f.author, b->author));
        assert(same(f.warning, b->warning) && same(f.run_colors.page, b->run_colors.page));
        assert(same(f.run_colors.ink, b->run_colors.ink) && same(f.run_colors.soft, b->run_colors.soft));
        assert(same(f.run_colors.dim, b->run_colors.dim) && same(f.run_colors.failure, b->run_colors.failure));
        assert(same(f.run_colors.warning, b->run_colors.warning));
        for (int s = 0; s < GUI_SCREEN_COUNT; ++s) assert(!strcmp(f.title[s], b->title[s]));
    }
    puts("GUI themes: built-ins, theme.ini over its base, themes/ folders passed");
    return 0;
}
