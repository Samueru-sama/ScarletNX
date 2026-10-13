/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_MODEL_H
#define SNX_GUI_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { GUI_SWITCH, GUI_LINUX, GUI_FULLSCREEN, GUI_EXIT, GUI_ITEM_COUNT };
typedef enum { GUI_UP, GUI_DOWN, GUI_LEFT, GUI_RIGHT, GUI_SELECT, GUI_CANCEL, GUI_PAGE_UP, GUI_PAGE_DOWN } GuiKey;
typedef enum { GUI_LOAD_NONE, GUI_LOAD_OPTIONS, GUI_LOAD_DROP, GUI_LOAD_GAMES } GuiLoadView;
enum { GUI_LOAD_DRAG, GUI_LOAD_DIRECTORY };
typedef struct {
    int selected, load_selected;
    GuiLoadView view;
    bool running, fullscreen, quit;
} GuiMenu;
void gui_menu_key(GuiMenu* menu, GuiKey key);

typedef struct {
    size_t count, page, page_size, selected; /* selected == 0 is Back on every page */
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
/* The VGA BIOS font uses CP437. Unrepresentable Unicode becomes '?'. */
unsigned char gui_glyph(uint32_t codepoint);
/* SDL_DROP_FILE is already a path. DROP_TEXT/clipboard may contain a quoted path
   or a local file:// URI. Returns owned UTF-8, never shell-expanded. */
char* gui_drop_path(const char* text, bool text_drop);
#endif
