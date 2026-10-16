/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_MODEL_H
#define SNX_GUI_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "font.h" /* gui_glyph */

/* the menu's items, in order: Games, the loads, the keys, Settings and Share log, Exit */
enum {
    GUI_GAMES, /* games/ and the player's folders' games: shown only when they hold any */
    GUI_SWITCH,
    GUI_LINUX,
    GUI_3DS,
    GUI_KEYS,
    GUI_SETTINGS, /* the game's .ini settings, the launcher's own for the runs and its App: settings.h */
    GUI_LOG,      /* the logs in one file, for a bug report: a phone has no terminal to copy them from */
    GUI_EXIT,
    GUI_ITEM_COUNT
};
typedef enum { GUI_UP, GUI_DOWN, GUI_LEFT, GUI_RIGHT, GUI_SELECT, GUI_CANCEL, GUI_PAGE_UP, GUI_PAGE_DOWN } GuiKey;
#define GUI_TEXT_MAX 4
/* the screen over the menu: none, a load's file to pick, the games' browser, the keys' */
typedef enum { GUI_LOAD_NONE, GUI_LOAD_DROP, GUI_LOAD_GAMES, GUI_LOAD_KEYS } GuiLoadView;
typedef struct {
    int selected; /* keyboard/mouse focus, not activation */
    int active;   /* the last explicitly activated item: the load a picked file goes to */
    bool activated;
    bool games; /* Games shows */
    GuiLoadView view;
    bool running, quit;
    int text_size; /* the font's scale, 1 to GUI_TEXT_MAX; 0 fits the screen */
    bool panels;   /* Settings > App's Layout: the theme's ([body] layout), else the lines (Classic) */
    /* the recent games above the items: how many, and the focused one + 1 (0: an item has the focus) */
    int recent, recent_focus;
} GuiMenu;
/* what a key on the menu asks of the GUI: nothing more, the Settings, the logs shared, or the focused recent row (a
   game, or the one that shows the rest) */
typedef enum { GUI_MENU_STAY, GUI_MENU_SETTINGS, GUI_MENU_LOG, GUI_MENU_RECENT } GuiMenuDo;
GuiMenuDo gui_menu_key(GuiMenu* menu, GuiKey key);
/* The question a running run's abort asks first, or leaving the menu on a computer: shown or not, which one, and the
   choice in focus, the one that stays first */
enum { GUI_CONFIRM_KEEP, GUI_CONFIRM_ABORT };
typedef struct {
    bool shown;
    bool exit; /* leaving the app, not aborting a run */
    int selected;
} GuiConfirm;
/* A key on the question: the arrows move between its choices, Select takes the one in focus and Cancel keeps going,
   both closing it. True when the run is to be aborted */
bool gui_confirm_key(GuiConfirm* confirm, GuiKey key);
const char* gui_setting_description(const GuiMenu* menu);
/* bytes in MB, or in GB from 1024 MB on */
void gui_size_text(char* out, size_t size, uint64_t bytes);
/* the footer's line: RAM in MB or GB, the CPU as a share of all cores and the cores in use (cpu_percent counts one
   core as 100%) */
void gui_usage_text(char* out, size_t size, bool available, uint64_t memory_bytes, double cpu_percent, int cores);
/* how many columns a line over columns too many has slid at ms, a column every step_ms: it waits at each end */
int gui_slide(int over, uint64_t ms, uint32_t step_ms);
/* the bytes of s's first row when it is wrapped at width characters (at least 1): all of it when it fits, else up to
   the last slash or backslash that fits, else the last space, else width characters */
size_t gui_wrap(const char* s, int width);

typedef struct {
    size_t count, page, page_size, selected; /* focus: selected == 0 is Back on every page */
    size_t active;                           /* absolute file index, preserved across page changes and resizing */
    bool activated;
} GuiBrowser;
typedef enum { GUI_BROWSER_NONE, GUI_BROWSER_BACK, GUI_BROWSER_OPEN } GuiBrowserAction;
size_t gui_browser_pages(const GuiBrowser* browser);
size_t gui_browser_items(const GuiBrowser* browser);
void gui_browser_resize(GuiBrowser* browser, size_t page_size);
GuiBrowserAction gui_browser_key(GuiBrowser* browser, GuiKey key);

#define GUI_MAX_COLS 240
#define GUI_MAX_ROWS 100
typedef struct {
    unsigned char cells[GUI_MAX_ROWS][GUI_MAX_COLS];
    int cols, rows, row, col, escape, utf8_left;
    uint32_t codepoint, utf8_min;
} GuiConsole;
void gui_console_init(GuiConsole* console, int cols, int rows);
void gui_console_resize(GuiConsole* console, int cols, int rows);
void gui_console_feed(GuiConsole* console, const char* data, size_t size);
/* SDL_DROP_FILE is already a path. DROP_TEXT/clipboard may contain a quoted path
   or a local file:// URI. Returns owned UTF-8, never shell-expanded. */
char* gui_drop_path(const char* text, bool text_drop);
/* The path of a file or folder a picker chose: on Android a document or a folder (a tree) of the device's storage, or a
   download it names by path, NULL for any other (an app's, a cloud's); elsewhere the path or file:// URI it gave.
   Owned. */
char* gui_document_path(const char* uri);
#endif
