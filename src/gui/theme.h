/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_THEME_H
#define SNX_GUI_THEME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t r, g, b;
} GuiRgb;
typedef enum { GUI_STYLE_PLAIN, GUI_STYLE_BAR, GUI_STYLE_NONE } GuiStyle;
/* a run's screens: the driver's output as it comes, or its steps as a loading screen draws them */
typedef enum { GUI_RUN_CONSOLE, GUI_RUN_STEPS } GuiRunLayout;
typedef enum { GUI_LINE_NONE, GUI_LINE_TYPING, GUI_LINE_SLIDE, GUI_LINE_FADE } GuiLineIn;
typedef enum { GUI_RING_NONE, GUI_RING_LEFT, GUI_RING_RIGHT, GUI_RING_NEAR_TEXT } GuiRing;
typedef enum { GUI_ALIGN_LEFT, GUI_ALIGN_CENTER, GUI_ALIGN_RIGHT } GuiAlign;
/* the lines that wait on the user: the keys screen's drop line, the ESC line after a run, a run being aborted */
typedef enum {
    GUI_PROMPT_KEYS,
    GUI_PROMPT_DONE,
    GUI_PROMPT_FAILED,
    GUI_PROMPT_ABORTING,
    GUI_PROMPT_COUNT
} GuiPromptLine;
/* how such a line is drawn when lines[] says so: framed, placed on its row, blinking at blink_ms; else plain */
typedef struct {
    GuiAlign align;
    char open[16], close[16];
    bool blink;
    bool lines[GUI_PROMPT_COUNT];
} GuiPrompt;
/* the theme.ini format this GUI writes and reads; a newer file's keys this one knows still apply */
#define GUI_THEME_FORMAT 1
typedef enum {
    GUI_SCREEN_MENU,
    GUI_SCREEN_GAMES,
    GUI_SCREEN_KEYS,
    GUI_SCREEN_COMPILING,
    GUI_SCREEN_LOADING,
    GUI_SCREEN_COUNT
} GuiScreen;
typedef struct {
    GuiRgb page, ink, soft, dim, failure, warning;
} GuiPalette;
typedef struct {
    char id[64], name[64], author[64];
    uint32_t format;
    GuiRgb page, ink, soft, focus, active, failure, dim, warning;
    GuiStyle head, footer;
    bool head_rule, footer_rule;
    bool focus_bar; /* the focused line as a band of ink, its text in the page colour */
    char marker[16];
    char title[GUI_SCREEN_COUNT][64];
    GuiRunLayout run;
    GuiPalette run_colors;   /* the steps screen's own: a loading screen, darker than the menus */
    bool texture, bar;       /* faint lines over the page; the progress bar and its percentage */
    bool detailed, collapse; /* each step's lines under it; folded once the step is done */
    GuiRing ring;
    GuiLineIn line_in;
    bool typing_sync; /* one line typed at a time, those already done three times faster */
    GuiPrompt prompt;
    uint32_t fade_in_ms, fade_out_ms, line_in_ms, typing_ms, blink_ms;
} GuiTheme;
typedef struct {
    char (*ids)[64]; /* the built-ins, then each folder of themes/ with a theme.ini, in one block */
    size_t count;
} GuiThemes;

/* "default" and "yorha"; NULL for any other id */
const GuiTheme* gui_theme_builtin(const char* id);
/* A theme.ini's text over the built-in its [theme] base names (default when absent): a key it lacks, or cannot
   read, keeps the base's value. Returns false only when out of memory. */
bool gui_theme_parse(GuiTheme* theme, const char* id, const char* text, size_t size);
/* The first dirs[i]/id/theme.ini that can be read, else the built-in of that id, else the default theme: a folder
   named like a built-in replaces it */
void gui_theme_load(GuiTheme* theme, const char* const* dirs, size_t count, const char* id);
/* the built-ins, then each other folder of dirs with a theme.ini, once */
void gui_themes_scan(GuiThemes* themes, const char* const* dirs, size_t count);
void gui_themes_free(GuiThemes* themes);
#endif
