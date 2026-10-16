/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's state, App, and what its files share: the screens' words, the small helpers of their drawing
   (static inline), and the functions one file calls in another (gui_app_). Private to src/gui */
#ifndef SNX_GUI_APP_H
#define SNX_GUI_APP_H
#include <SDL3/SDL.h>
#include "common.h"
#include "browser.h"
#include "editor.h"
#include "font.h"
#include "gui.h"
#include "model.h"
#include "place.h"
#include "gui_process.h"
#include "steps.h"
#include "theme.h"
#include "window_icon.h"
#include "build_info.h"
#include "recent.h"
#include "settings.h"
#include "pad_sdl.h"
#include "pad_screen.h"
#ifdef SDL_PLATFORM_ANDROID
#include "android.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
/* the space ScarletNX takes, counted on a thread of its own when the Settings open: its folders may hold many files,
   on a slow card. The thread's until done is set; one block with the folders' paths */
#define GUI_SPACE_DIRS 6 /* games/, work/ and themes/ of the data folder; the XDG folders, or the menu's own */
typedef struct {
    char* dirs[GUI_SPACE_DIRS];
    uint64_t used;
    int done, stop;
} GuiSpace;
/* the editor's screens: the Settings' groups, a group's settings, the App's rows, its game folders, the theme editor,
   and the Controllers' button map, the pad drawn (pad_screen.h) */
enum { GUI_FORM_SETTINGS, GUI_FORM_GROUP, GUI_FORM_APP, GUI_FORM_FOLDERS, GUI_FORM_THEME, GUI_FORM_PAD };
#define GUI_GROUPS_MAX  32
#define GUI_FOLDERS_MAX 16 /* the player's game folders, past games/ */
/* the App screen's values, as its rows show them, and the game folders' (games/ first) */
typedef struct {
    char theme[64], text[16], layout[16], folders[32];
    char folder[GUI_FOLDERS_MAX + 1][16];
} GuiAppForm;
typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* font;
    GuiProcess* process;
    GuiMenu menu;
    GuiBrowser browser;
    GuiGames games;
    GuiConsole console;
    GuiUsage usage;
    GuiTheme theme;
    GuiThemes themes;
    GuiSteps steps;
    GuiRecent recent;
    GuiSettings settings; /* read the first time a run or the Settings need them */
    char* settings_file;  /* the picks kept for every game (settings_path); NULL keeps none */
    char* app_file;       /* Settings > App's picks (app.ini), beside it; NULL keeps none */
    GuiSpace* space;      /* the latest count, its thread counter until joined */
    SDL_Thread* counter;
    uint64_t free_bytes; /* on the data folder's disk, when the count started */
    char* left;          /* what older builds left where this one no longer looks, a line each, lefts of them */
    int lefts;
    bool left_shown;   /* in a box as the menu opens, until a key, a button or a finger closes it */
    Uint64 left_at;    /* when they were looked for */
    char* recent_file; /* where the recent games are kept; NULL keeps none */
    char* run_log;     /* where a run's output goes as it comes, the last run's; NULL keeps none */
    FILE* run_out;
    int recent_named;           /* the run's own name went to its recent game */
    Uint64 clock;               /* fixed time for tests; 0 is the real clock */
    Uint64 fade_ms, leaving_ms; /* a screen fading in from black since; the run fading out since */
    Uint64 entering_ms;         /* the menu fading out since, before a run fades in */
    char* self;    /* the program that runs Switch inputs: the runtime the menu is part of, NULL to look for one */
    char* drivers; /* the folder the other drivers are looked for in first */
    char *cwd, *file, *games_dir, *keys_path;
    char* data_dir;       /* games/, work/ and themes/: the driver's too, which gets it as --repo (snx_data_dir) */
    char* themes_dirs[2]; /* themes/ in the data folder, then beside the GUI */
    char message[512];
    int cols, rows, scale, log_top;
    int auto_scale; /* Text size Auto's scale, chosen when the window opens: a resize keeps the letters as they were */
    Uint64 sampled;
    bool has_output, finished, aborting, abnormal_exit;
    GuiConfirm confirm; /* the question before a run is aborted, when ask_abort says to ask it */
    bool ask_abort;     /* ESC asks first: on a phone it is Back, an edge swipe easily made by mistake */
    int native;
    Uint64 finger;            /* the finger on the screen (finger_id), 0 for none */
    float finger_x, finger_y; /* where it went down, 0 to 1 across the window */
    float finger_mark;        /* the height a drag last moved the focus at, a row at a time */
    bool pen_held;            /* a pen lifted: its hover leaves the focus where the lift put it until it moves away */
    float pen_x, pen_y;       /* where it lifted, 0 to 1 across the window */
    Uint32 picked;            /* the event a file from the system's picker comes back as */
    SDL_Texture* splash;      /* the boot's splash, its icon, until splash_until */
    Uint64 splash_until;
    GuiRgb splash_page;
    bool editing; /* the editor's screen (form says which) instead of the menu's */
    GuiEditor editor;
    int form;            /* GUI_FORM_* */
    int group;           /* the group open, or the one the Settings come back to */
    int first, page;     /* a group's: the settings' row of its first, and of the page's row it opened (-1 none) */
    bool theme_from_app; /* the theme editor goes back to the App's rows */
    SnxPadScreen pad;    /* the button map's, on the rows gamepad_buttons and gamepad_map */
    SnxPadDrawing pad_drawing; /* as last drawn: what a click or a tap lands on */
    SDL_Gamepad* gamepad;      /* the pad it shows, the first one plugged in, while it is open */
    bool pads_read;            /* SDL's gamepads started for it */
    unsigned pad_pushed;       /* the triggers and the left stick's directions past half way, a bit each */
    /* the Settings' groups: App, then a group per section of the settings, its first row and rows; a row each */
    int groups, group_first[GUI_GROUPS_MAX], group_rows[GUI_GROUPS_MAX];
    GuiThemeKey group_keys[GUI_GROUPS_MAX];
    char group_about[GUI_GROUPS_MAX][128];
    GuiAppForm app_form;
    char* folders[GUI_FOLDERS_MAX]; /* the player's game folders, listed after games/ */
    int folder_count;
    char* folders_file; /* where they are kept; NULL keeps none */
    GuiThemeKey folder_keys[GUI_FOLDERS_MAX + 1];
    int folder_armed;       /* a folder's row Select was pressed on once, + 1: again takes it off */
    size_t games_total;     /* what Games counts, games/ and the folders: Games shows when there are any */
    uint64_t games_stamp;   /* what the game folders held when last counted (games_stamp) */
    Uint64 games_checked;   /* when they were last looked at */
    bool recent_all;        /* every recent game shows, not GUI_RECENT_SHOWN and a row for the rest */
    Uint32 picked_folder;   /* the event a folder from the system's picker comes back as */
    SDL_Texture* preview;   /* the editor's: the menu or a run drawn in its theme, at 1x */
    GuiSteps preview_steps; /* the run it shows, over again every GUI_PREVIEW_MS */
    GuiConsole preview_console;
    Uint64 preview_since;
#ifdef SDL_PLATFORM_ANDROID
    bool hidden;     /* behind other apps: nothing is drawn, a run is still read */
    bool play_later; /* a game made ready while hidden, which opens once the menu shows */
    char told[340];  /* the run's notification: its step's line, its percent, when; 0 before its first */
    int told_percent;
    Uint64 told_ms;
#endif
} App;
#define GUI_RECENT_SHOWN 3 /* the recent games the menu shows before a row that shows the rest */
/* what the screens say to press: a phone has a finger, the back button and the system's file picker, nothing to drop;
   a computer has the picker too (on Linux the desktop's portal, else zenity), on a screen of its own as the keys' */
#ifdef SDL_PLATFORM_ANDROID
#define SAY_MENU       "Tap: select. Back: go back."
#define SAY_LOAD       "Choose a game or what to load."
#define SAY_DROP       "Pick the file in the system's picker. Back: go back."
#define SAY_GAMES      "Tap: open. Swipe across: turn the page. Back: go back."
#define SAY_KEYS       "Setup device keys: tap Pick prod.keys to open the file picker"
#define SAY_KEYS_HOW   "Back: go back."
#define SAY_KEYS_WAIT  "Pick prod.keys"
#define SAY_PICK       "tap below to open the file picker" /* these two not shown: the menu keeps the load options */
#define SAY_PICK_BOX   "tap the box to open the file picker"
#define SAY_FILE_WAIT  SAY_KEYS_WAIT
#define SAY_DONE       "Tap to return to menu"
#define SAY_ABORT      "Tap here to abort and return to menu"
#define SAY_NO_GAMES   "No games in games/: put a game's .nro, .xci or .nsp there."
#define SAY_COUNT      "game(s)"
#define SAY_LOG_FAILED "The log could not be saved: allow the app access to files."
#define SAY_EDITOR     "Tap a key to change it or type it. Back: go back."
#define SAY_SETTINGS   SAY_EDITOR
#define SAY_GROUPS     "Tap a group to open it. Back: go back."
#define SAY_APP        "Tap a row to change or open it. Back: go back."
#define SAY_FOLDERS    "Add a folder: tap it. Tap a folder twice to take it off. Back: go back."
#define SAY_PAD        "Tap a preset or a control; tap it again for a press. Back: go back."
#else
#define SAY_MENU       "Arrows/mouse: move. Space/Enter/click: select."
#define SAY_LOAD       "Choose a game or what to load. ESC: exit."
#define SAY_DROP       SAY_LOAD /* not shown: the file is picked on a screen of its own */
#define SAY_GAMES      "Up/Down: select. Enter/Space: open. Left/Right/PgUp/PgDown: page."
#define SAY_PICK       "click below (or Enter) to open the file picker"
#define SAY_PICK_BOX   "click the box (or Enter) to open the file picker"
#define SAY_KEYS       "Setup device keys: " SAY_PICK
#define SAY_KEYS_HOW   "Ctrl+V: paste a path, or drop the file on the window. ESC: back."
#define SAY_KEYS_WAIT  "Pick or drop prod.keys"
#define SAY_FILE_WAIT  "Pick or drop a file"
#define SAY_DONE       "Press [ESC] to return to menu"
#define SAY_ABORT      "Press [ESC] to abort and return to menu"
#define SAY_NO_GAMES   "No games in games/ or the folders of Settings > App."
#define SAY_COUNT      "game(s)"
#define SAY_LOG_FAILED "The log could not be saved: check the folder's permissions."
#define SAY_EDITOR     "Up/Down: key. Left/Right: change. Enter: type. Ctrl+S: save. ESC: back."
#define SAY_SETTINGS   "Up/Down: setting. Left/Right: change. Enter: type. Del: default. ESC: back."
#define SAY_GROUPS     "Up/Down: group. Enter: open. ESC: back."
#define SAY_APP        "Up/Down: row. Left/Right: change. Enter: change or open. ESC: back."
#define SAY_FOLDERS    "Add a folder: pick, drop or paste one. Del: take one off. ESC: back."
#define SAY_PAD        "Arrows/pad: move, change. Enter/A: select. Del: the .ini's. ESC: back."
#endif
#define SAY_LEFT_BY    " was left by an older version: this one keeps it in "
#define SAY_LEFT_TITLE "An older version left these files: move each from - to ->"
/* the screens the tabs go round, Tab and Shift+Tab or a click on one: the menu, Games and the Settings */
enum { GUI_TAB_MENU, GUI_TAB_GAMES, GUI_TAB_SETTINGS, GUI_TABS };
/* what a screen has besides its title, list and bar: the status box when the theme has a footer, the tabs when it
   shows them */
enum { GUI_SHOWS_STATUS = 1, GUI_SHOWS_TABS = 2 };
enum { GUI_APP_THEME, GUI_APP_TEXT, GUI_APP_LAYOUT, GUI_APP_EDITOR, GUI_APP_FOLDERS, GUI_APP_ROWS };
extern const char* const gui_items[];         /* the menu's items, by GUI_GAMES and the others */
extern const GuiThemeKey gui_app_form_keys[]; /* Settings > App's rows */
static inline SDL_Color rgb(GuiRgb c) {
    SDL_Color out = {c.r, c.g, c.b, 255};
    return out;
}
static inline Uint64 now_ms(const App* app) { return app->clock ? app->clock : SDL_GetTicks(); }
/* the font's glyph c, its corner at x, y in pixels */
static inline void glyph(App* app, float x, float y, unsigned char c) {
    SDL_FRect source = {(float)(c % 16 * 8), (float)(c / 16 * 16), 8, 16};
    SDL_FRect destination = {x, y, 8.0f * (float)app->scale, 16.0f * (float)app->scale};
    SDL_RenderTexture(app->renderer, app->font, &source, &destination);
}
/* text from a pixel position, chars at most, alpha its opacity, cut at the last column; where it ends */
static inline float text_px(App* app, float x, float y, const char* value, size_t chars, SDL_Color color, float alpha) {
    size_t i;
    float w = 8.0f * (float)app->scale, limit = (float)(app->cols - 1) * w;
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(app->font, (Uint8)(alpha * 255));
    for (i = 0; *value && i < chars; ++i, x += w) {
        unsigned char c = gui_glyph(SDL_StepUTF8(&value, NULL));
        if (x + w <= limit) glyph(app, x, y, c);
    }
    SDL_SetTextureAlphaMod(app->font, 255);
    return x;
}
/* text at a cell, a line too long for its row sliding as the theme says */
static inline void text(App* app, int x, int y, const char* value, SDL_Color color) {
    int skip = gui_slide((int)SDL_utf8strlen(value) - (app->cols - 1 - x), now_ms(app), app->theme.scroll_ms);
    while (skip-- > 0 && *value) SDL_StepUTF8(&value, NULL);
    text_px(app, (float)(x * 8 * app->scale), (float)(y * 16 * app->scale), value, SIZE_MAX, color, 1);
}
static inline void fill(App* app, float x, float y, float w, float h, GuiRgb c, Uint8 alpha) {
    SDL_FRect r = {x, y, w, h};
    SDL_SetRenderDrawBlendMode(app->renderer, alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, alpha);
    SDL_RenderFillRect(app->renderer, &r);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
}
static inline void area(const App* app, int* width, int* height) {
    SDL_Rect r;
    SDL_GetRenderViewport(app->renderer, &r);
    *width = r.w;
    *height = r.h;
}
/* the screen a file is picked or dropped on: the keys', and on a computer the load options' file too (on a phone the
   menu keeps those, and a tap on the option opens the picker) */
static inline int pick_screen(const App* app) {
#ifdef SDL_PLATFORM_ANDROID
    return app->menu.view == GUI_LOAD_KEYS;
#else
    return app->menu.view == GUI_LOAD_KEYS || app->menu.view == GUI_LOAD_DROP;
#endif
}
/* the menu's screen, a run or not */
static inline GuiScreen menu_screen(const App* app) {
    return app->menu.view == GUI_LOAD_GAMES  ? GUI_SCREEN_GAMES
           : app->menu.view == GUI_LOAD_KEYS ? GUI_SCREEN_KEYS
                                             : GUI_SCREEN_MENU;
}
static inline GuiScreen screen(const App* app) {
    if (app->menu.running || app->finished)
        return app->native || gui_steps_last(&app->steps, "LAUNCH") ? GUI_SCREEN_LOADING : GUI_SCREEN_COMPILING;
    return menu_screen(app);
}
/* the recent games shown, and whether a row under them shows the rest */
static inline int recent_shown(const App* app) {
    return app->recent_all ? app->recent.count : SDL_min(app->recent.count, GUI_RECENT_SHOWN);
}
static inline int recent_more(const App* app) { return recent_shown(app) < app->recent.count; }
/* the recent games take a title row, a row each and a blank one above the items */
static inline int recent_rows(const App* app) {
    return app->recent.count ? recent_shown(app) + recent_more(app) + 2 : 0;
}
/* the menu's last row: below it a blank, the message, and the focused item's description */
static inline int menu_last_row(const App* app) { return app->rows - 7; }
/* an item's row; Games, hidden, takes none */
static inline int menu_row_in_full(const App* app, int item) { return 4 + recent_rows(app) + item - !app->menu.games; }
/* the focus's row in the whole menu: a recent row or an item */
static inline int menu_focus_row(const App* app) {
    return app->menu.recent_focus ? 4 + app->menu.recent_focus : menu_row_in_full(app, app->menu.selected);
}
/* how far the menu slides up for the focus to show: only when its letters are too big for the screen */
static inline int menu_offset(const App* app) { return SDL_max(0, menu_focus_row(app) - menu_last_row(app)); }
static inline int menu_row(const App* app, int item) { return menu_row_in_full(app, item) - menu_offset(app); }
static inline int recent_row(const App* app, int game) { return 5 + game - menu_offset(app); }
/* whether a row of the menu shows: under the head and above the message */
static inline int menu_shows(const App* app, int row) { return row >= 4 && row <= menu_last_row(app); }
/* a recent row: a game, or under the ones shown the row that shows the rest */
static inline void recent_label(const App* app, int index, char* label, size_t size) {
    if (index < recent_shown(app))
        snprintf(label, size, "%s%s", app->theme.marker, app->recent.game[index].name);
    else
        snprintf(label, size, "%s...show more", app->theme.marker);
}
static inline void menu_label(const App* app, int item, char* label, size_t size) {
    if (item == GUI_GAMES)
        snprintf(label, size, "%s%s (%zu game%s)", app->theme.marker, gui_items[item], app->games_total,
                 app->games_total == 1 ? "" : "s");
    else
        snprintf(label, size, "%s%s", app->theme.marker, gui_items[item]);
}
/* still the menu, fading out, while a run starts */
static inline int entering(const App* app) {
    return app->entering_ms && now_ms(app) < app->entering_ms + app->theme.fade_out_ms;
}
/* the screens in the theme's boxes: Settings > App's Layout on Panels, and a theme that has them */
static inline int boxed(const App* app) { return app->menu.panels && app->theme.layout == GUI_LAYOUT_PANELS; }
static inline int cell_w(const App* app) { return 8 * app->scale; }
static inline int cell_h(const App* app) { return 16 * app->scale; }
static inline GuiRect box_rect(int x, int y, int w, int h) {
    GuiRect r;
    r.x = x;
    r.y = y;
    r.w = w > 0 ? w : 0;
    r.h = h > 0 ? h : 0;
    return r;
}
static inline void fill_rect(App* app, GuiRect r, GuiRgb c) {
    if (r.w > 0 && r.h > 0) fill(app, (float)r.x, (float)r.y, (float)r.w, (float)r.h, c, 255);
}
/* r without a column at either side: where a box's text goes */
static inline GuiRect inner(const App* app, GuiRect r) {
    return box_rect(r.x + cell_w(app), r.y, r.w - 2 * cell_w(app), r.h);
}
/* the editor's rows between rows 4 and the last three, as many as fit */
static inline int editor_rows(const App* app) { return SDL_max(1, app->rows - 7); }

/* main.c */
char* gui_app_keys_in_use(const App* app);
void gui_app_layout(App* app);
void gui_app_values(App* app);
void gui_app_settings_top(App* app, int row);
void gui_app_open_group(App* app, int g);
void gui_app_leave_editor(App* app);
void gui_app_leave_form(App* app);
void gui_app_keep_settings(App* app);
void gui_app_editor_key(App* app, GuiKey key);
void gui_app_go_tab(App* app, int tab);
void gui_app_menu_key(App* app, GuiKey key);
void gui_app_confirmed(App* app);
bool gui_app_menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key);
/* draw.c */
SDL_Texture* gui_app_create_font(SDL_Renderer* renderer);
void gui_app_set_area(App* app);
void gui_app_band(App* app, int row, int rows, GuiRgb c);
void gui_app_disk_text(const App* app, char* out, size_t size);
void gui_app_run_name(const App* app, char* out, size_t size);
size_t gui_app_step_runs(const GuiStep* s, int how, Uint64 now, char run[3][112], size_t length[3]);
const char* gui_app_ask_question(const App* app);
void gui_app_ask_box(const App* app, int* col, int* row, int* cols);
void gui_app_left_paths(const char* line, char* old, char* now, size_t size);
void gui_app_splash_start(App* app);
void gui_app_preview_box(const App* app, int* col, int* row, int* cols, int* rows);
bool gui_app_preview_texture(App* app);
void gui_app_render(App* app);
/* boxes.c */
void gui_app_bar(App* app, GuiRect r, const char* about, SDL_Color color, const char* keys);
void gui_app_title_box(App* app, GuiRect r, const char* title);
int gui_app_tab_shown(const App* app);
const char* gui_app_screen_keys(const App* app, const char* keys, char* out, size_t size);
void gui_app_vignette(App* app);
void gui_app_chrome(App* app, const GuiPlace* p);
void gui_app_place_screen(App* app, GuiPlace* p, const char* title, const char* keys, int list_cols, int shows);
void gui_app_draw_menu_boxed(App* app);
void gui_app_ask_boxed(App* app);
void gui_app_left_boxed(App* app);
size_t gui_app_games_page(App* app);
void gui_app_draw_games_boxed(App* app);
void gui_app_draw_pick_boxed(App* app);
void gui_app_run_place(App* app, GuiPlace* p);
void gui_app_run_console(App* app, const GuiPlace* p, int* cols, int* rows);
void gui_app_draw_run_boxed(App* app);
void gui_app_draw_form_boxed(App* app);
void gui_app_form_boxed_pointer(App* app, float x, float y, bool touch);
void gui_app_ask_boxed_pointer(App* app, float x, float y, bool click);
void gui_app_boxed_pointer(App* app, float x, float y, bool click, bool touch);
bool gui_app_menu_scrolls(App* app);
/* button_map.c */
bool gui_app_pad_page(const App* app, int row);
void gui_app_open_pad(App* app, int row);
void gui_app_pad_close(App* app);
void gui_app_pad_did(App* app, int did);
bool gui_app_pad_event(App* app, const SDL_Event* e);
void gui_app_render_pad(App* app);
#endif
