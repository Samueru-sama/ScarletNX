/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <SDL3/SDL.h>
#ifndef SNX_GUI_NO_MAIN
#include <SDL3/SDL_main.h>
#endif
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

/* the checkout the drivers were built from and its build tree: a build tree's menu lists the games/ its driver makes */
#ifdef SNX_SOURCE_DIR
#define GUI_CHECKOUT SNX_SOURCE_DIR
#define GUI_BUILD    SNX_BUILD_DIR
#else
#define GUI_CHECKOUT NULL
#define GUI_BUILD    NULL
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
    char theme[64], text[16], folders[32];
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
static const char* const items[] = {"Games",
                                    "Load Switch NSP/XCI/NRO",
                                    "Load Linux ARM ELF",
                                    "Load 3DS CIA",
                                    "Setup device keys",
                                    "Settings",
                                    "Share log",
                                    "Exit"};
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
static SDL_Color rgb(GuiRgb c) {
    SDL_Color out = {c.r, c.g, c.b, 255};
    return out;
}
static Uint64 now_ms(const App* app) { return app->clock ? app->clock : SDL_GetTicks(); }

static SDL_Texture* create_font(SDL_Renderer* renderer) {
    SDL_Surface* surface = SDL_CreateSurface(128, 256, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;
    for (int glyph = 0; glyph < 256; ++glyph)
        for (int y = 0; y < 16; ++y) {
            Uint32* line = (Uint32*)((char*)surface->pixels + ((glyph / 16) * 16 + y) * surface->pitch);
            for (int x = 0; x < 8; ++x)
                line[(glyph % 16) * 8 + x] = (gui_vga_font[glyph * 16 + y] & (0x80 >> x)) ? 0xffffffffu : 0;
        }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }
    return texture;
}
/* the font's glyph c, its corner at x, y in pixels */
static void glyph(App* app, float x, float y, unsigned char c) {
    SDL_FRect source = {(float)(c % 16 * 8), (float)(c / 16 * 16), 8, 16};
    SDL_FRect destination = {x, y, 8.0f * (float)app->scale, 16.0f * (float)app->scale};
    SDL_RenderTexture(app->renderer, app->font, &source, &destination);
}
/* text from a pixel position, chars at most, alpha its opacity, cut at the last column; where it ends */
static float text_px(App* app, float x, float y, const char* value, size_t chars, SDL_Color color, float alpha) {
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
static void text(App* app, int x, int y, const char* value, SDL_Color color) {
    int skip = gui_slide((int)SDL_utf8strlen(value) - (app->cols - 1 - x), now_ms(app), app->theme.scroll_ms);
    while (skip-- > 0 && *value) SDL_StepUTF8(&value, NULL);
    text_px(app, (float)(x * 8 * app->scale), (float)(y * 16 * app->scale), value, SIZE_MAX, color, 1);
}
/* a line that waits on the user: plain at x, or framed, placed and blinking as the theme's prompt says */
static void prompt(App* app, GuiPromptLine line, int x, int y, const char* value, SDL_Color color) {
    char framed[160];
    int width;
    const GuiPrompt* p = &app->theme.prompt;
    if (!p->lines[line]) {
        text(app, x, y, value, color);
        return;
    }
    if (p->blink && app->theme.blink_ms && (now_ms(app) / app->theme.blink_ms) % 2) return;

    snprintf(framed, sizeof framed, "%s%s%s", p->open, value, p->close);
    width = (int)SDL_utf8strlen(framed);
    if (p->align == GUI_ALIGN_CENTER) x = (app->cols - width) / 2;
    if (p->align == GUI_ALIGN_RIGHT) x = app->cols - 1 - width;
    text(app, SDL_max(0, x), y, framed, color);
}
/* the part of the window no system bar or camera covers, in the renderer's pixels: everything is drawn in it */
static void set_area(App* app) {
    SDL_Rect r;
    int ww, wh, ow, oh;
    SDL_GetWindowSize(app->window, &ww, &wh);
    SDL_GetRenderOutputSize(app->renderer, &ow, &oh);
    if (ww > 0 && wh > 0 && SDL_GetWindowSafeArea(app->window, &r)) {
        r.x = r.x * ow / ww;
        r.w = r.w * ow / ww;
        r.y = r.y * oh / wh;
        r.h = r.h * oh / wh;
        SDL_SetRenderViewport(app->renderer, &r);
    } else
        SDL_SetRenderViewport(app->renderer, NULL);
}
static void fill(App* app, float x, float y, float w, float h, GuiRgb c, Uint8 alpha) {
    SDL_FRect r = {x, y, w, h};
    SDL_SetRenderDrawBlendMode(app->renderer, alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, alpha);
    SDL_RenderFillRect(app->renderer, &r);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
}
static void area(const App* app, int* width, int* height) {
    SDL_Rect r;
    SDL_GetRenderViewport(app->renderer, &r);
    *width = r.w;
    *height = r.h;
}
/* rows of the whole width, as a band behind text: on to the window's edges, over the camera's cutout beside the
   text and what is left below the last row */
static void band(App* app, int row, int rows, GuiRgb c) {
    SDL_Rect safe;
    int width, height, bottom = row + rows == app->rows && app->rows < GUI_MAX_ROWS;
    float top;
    SDL_GetRenderViewport(app->renderer, &safe);
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    top = row ? (float)(safe.y + row * 16 * app->scale) : 0;
    SDL_SetRenderViewport(app->renderer, NULL);
    fill(app, 0, top, (float)width, (bottom ? (float)height : (float)(safe.y + (row + rows) * 16 * app->scale)) - top,
         c, 255);
    SDL_SetRenderViewport(app->renderer, &safe);
}
/* a line between two rows, across the text area */
static void rule(App* app, int row, GuiRgb c) {
    float s = (float)app->scale;
    fill(app, 8 * s, (float)(row * 16) * s - s, (float)((app->cols - 2) * 8) * s, s, c, 255);
}
/* ~/.switch/prod.keys (%USERPROFILE%\.switch on Windows), where hactool and other tools keep them */
static char* home_keys_path(void) {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    const char* home = getenv("HOME");
#endif
    if (!home || !home[0]) return NULL;
    char* directory = snx_path_join(home, ".switch");
    char* path = directory ? snx_path_join(directory, "prod.keys") : NULL;
    free(directory);
    return path;
}
#ifdef SNX_XDG
/* the menu of an installed build, whose folder is the user's data folder (snx_data_dir) */
static bool installed(const App* app) {
    char* user = snx_user_data_dir();
    bool same = user && app->data_dir && !strcmp(user, app->data_dir);
    free(user);
    return same;
}
#endif
/* where the keys are installed: an installed build's data folder, which the driver reads first, so nothing is written
   in the home folder itself (XDG); else ~/.switch */
static char* device_keys_path(const App* app) {
#ifdef SNX_XDG
    if (installed(app)) return snx_path_join(app->data_dir, "prod.keys");
#else
    (void)app;
#endif
    return home_keys_path();
}
/* the prod.keys the driver reads: those installed here, else for an installed build the ones in ~/.switch */
static char* keys_in_use(const App* app) {
    if (app->keys_path && snx_is_file(app->keys_path)) return snx_strdup(app->keys_path);
#ifdef SNX_XDG
    char* home = installed(app) ? home_keys_path() : NULL;
    if (home && snx_is_file(home)) return home;
    free(home);
#endif
    return NULL;
}
/* the footer from row: the usage, then the working folder, which games/ and themes/ are found in. The theme's bar style
   puts them on a band of ink over rows rows, its plain one in soft and ink; a rule above when it says so */
/* what ScarletNX's folders take, counted on a thread, and what is left on their disk; "" before the first count */
static void disk_text(const App* app, char* out, size_t size) {
    char used[32], left[32];
    out[0] = 0;
    if (!app->space) return;
    gui_size_text(left, sizeof left, app->free_bytes);
    if (__atomic_load_n(&app->space->done, __ATOMIC_ACQUIRE)) {
        gui_size_text(used, sizeof used, app->space->used);
        snprintf(out, size, "Disk: %s, %s free", used, left);
    } else
        snprintf(out, size, "Disk: counting, %s free", left);
}
static void footer(App* app, int row, int rows, GuiRgb page, GuiRgb ink, GuiRgb soft) {
    char usage[256], disk[96];
    int bar = app->theme.footer == GUI_STYLE_BAR;
    size_t n;
    if (bar) band(app, row, rows, ink);
    if (app->theme.footer_rule) rule(app, row, soft);
    gui_usage_text(usage, sizeof(usage), app->usage.available, app->usage.memory_bytes, app->usage.cpu_percent,
                   SDL_GetNumLogicalCPUCores());
    disk_text(app, disk, sizeof disk);
    n = strlen(usage);
    if (disk[0]) snprintf(usage + n, sizeof usage - n, "  %s", disk);
    text(app, 1, row, usage, rgb(bar ? page : soft));
    text(app, 1, row + 1, "CWD:", rgb(bar ? page : soft));
    text(app, 6, row + 1, app->cwd ? app->cwd : "(unavailable)", rgb(bar ? page : ink));
}
/* a selectable line: activated, focused (in the focus colour, or on a band) or plain */
static void entry(App* app, int x, int y, const char* label, int activated, int focused) {
    if (focused && app->theme.focus_bar) {
        band(app, y, 1, app->theme.focus);
        text(app, x, y, label, rgb(app->theme.page));
    } else
        text(app, x, y, label, rgb(activated ? app->theme.active : focused ? app->theme.focus : app->theme.ink));
}
/* a list shown from row first to row last: in the margin, an arrow up on the first while rows are hidden above, and
   one down on the last while rows are hidden below */
static void more_marks(App* app, int first, int last, int above, int below) {
    SDL_Color soft = rgb(app->theme.soft);
    if (above && below && first == last)
        text(app, 0, first, "\x12", soft); /* the font's up and down arrow */
    else {
        if (above) text(app, 0, first, "\x1e", soft);
        if (below) text(app, 0, last, "\x1f", soft);
    }
}
/* the screen a file is picked or dropped on: the keys', and on a computer the load options' file too (on a phone the
   menu keeps those, and a tap on the option opens the picker) */
static int pick_screen(const App* app) {
#ifdef SDL_PLATFORM_ANDROID
    return app->menu.view == GUI_LOAD_KEYS;
#else
    return app->menu.view == GUI_LOAD_KEYS || app->menu.view == GUI_LOAD_DROP;
#endif
}
/* the menu's screen, a run or not */
static GuiScreen menu_screen(const App* app) {
    return app->menu.view == GUI_LOAD_GAMES  ? GUI_SCREEN_GAMES
           : app->menu.view == GUI_LOAD_KEYS ? GUI_SCREEN_KEYS
                                             : GUI_SCREEN_MENU;
}
static GuiScreen screen(const App* app) {
    if (app->menu.running || app->finished)
        return app->native || gui_steps_last(&app->steps, "LAUNCH") ? GUI_SCREEN_LOADING : GUI_SCREEN_COMPILING;
    return menu_screen(app);
}
/* the theme step themes after or before the one shown, round the list */
static void next_theme(App* app, int step) {
    size_t i;
    const char* const* dirs = (const char* const*)app->themes_dirs;
    gui_themes_scan(&app->themes, dirs, 2);
    i = 0;
    while (i < app->themes.count && strcmp(app->themes.ids[i], app->theme.id)) ++i;
    if (app->themes.count)
        gui_theme_load(&app->theme, dirs, 2, app->themes.ids[(i + app->themes.count + step) % app->themes.count]);
}
/* the recent games take a title row, a row each and a blank one above the items */
/* the recent games shown, and whether a row under them shows the rest */
static int recent_shown(const App* app) {
    return app->recent_all ? app->recent.count : SDL_min(app->recent.count, GUI_RECENT_SHOWN);
}
static int recent_more(const App* app) { return recent_shown(app) < app->recent.count; }
static int recent_rows(const App* app) { return app->recent.count ? recent_shown(app) + recent_more(app) + 2 : 0; }
/* the menu's last row: below it a blank, the message, and the focused item's description */
static int menu_last_row(const App* app) { return app->rows - 7; }
/* an item's row; Games, hidden, takes none */
static int menu_row_in_full(const App* app, int item) { return 4 + recent_rows(app) + item - !app->menu.games; }
/* the focus's row in the whole menu: a recent row or an item */
static int menu_focus_row(const App* app) {
    return app->menu.recent_focus ? 4 + app->menu.recent_focus : menu_row_in_full(app, app->menu.selected);
}
/* how far the menu slides up for the focus to show: only when its letters are too big for the screen */
static int menu_offset(const App* app) { return SDL_max(0, menu_focus_row(app) - menu_last_row(app)); }
static int menu_row(const App* app, int item) { return menu_row_in_full(app, item) - menu_offset(app); }
static int recent_row(const App* app, int game) { return 5 + game - menu_offset(app); }
/* whether a row of the menu shows: under the head and above the message */
static int menu_shows(const App* app, int row) { return row >= 4 && row <= menu_last_row(app); }
/* a recent row: a game, or under the ones shown the row that shows the rest */
static void recent_label(const App* app, int index, char* label, size_t size) {
    if (index < recent_shown(app))
        snprintf(label, size, "%s%s", app->theme.marker, app->recent.game[index].name);
    else
        snprintf(label, size, "%s...show more", app->theme.marker);
}
/* the list as the menu and the file have it */
static void recent_changed(App* app) {
    app->menu.recent = recent_shown(app) + recent_more(app);
    if (app->menu.recent_focus > app->menu.recent) app->menu.recent_focus = app->menu.recent;
    if (app->recent_file) gui_recent_save(&app->recent, app->recent_file);
}
static void recent_open(App* app, char* file) {
    free(app->recent_file);
    app->recent_file = file;
    gui_recent_load(&app->recent, file);
    app->menu.recent = recent_shown(app) + recent_more(app);
    app->menu.recent_focus = 0;
}
static void menu_label(const App* app, int item, char* label, size_t size) {
    if (item == GUI_GAMES)
        snprintf(label, size, "%s%s (%zu game%s)", app->theme.marker, items[item], app->games_total,
                 app->games_total == 1 ? "" : "s");
    else
        snprintf(label, size, "%s%s", app->theme.marker, items[item]);
}
/* still the menu, fading out, while a run starts */
static int entering(const App* app) {
    return app->entering_ms && now_ms(app) < app->entering_ms + app->theme.fade_out_ms;
}
/* the rows Auto keeps at the least: a phone held wide is short, and its menu needs room for its items */
#ifdef SDL_PLATFORM_ANDROID
#define GUI_AUTO_ROWS 20
#else
#define GUI_AUTO_ROWS 16
#endif
/* Text size Auto's scale: twice the display's own (Android's dpi / 160), so a computer at 100% gets 2x and a phone's
   letters come out as big to the eye, as long as rows rows fit in a window height pixels high */
static int pick_auto_scale(float display_scale, int height, int rows) {
    int scale = SDL_max(1, (int)(2.0f * display_scale));
    while (scale > 1 && height / (16 * scale) < rows) --scale;
    return scale;
}
static int boxed(const App* app);
static size_t games_page(App* app);
static void run_place(App* app, GuiPlace* p);
static void run_console(App* app, const GuiPlace* p, int* cols, int* rows);
static void layout(App* app) {
    int run;
    int width, height, whole_width, whole_height;
    set_area(app);
    area(app, &width, &height);
    /* Auto's letters are the opening window's, measured whole: a phone's system bars may still show over it then, and
       the safe area be short. The window then keeps room for 16 rows of them */
    if (!app->auto_scale && SDL_GetWindowSizeInPixels(app->window, &whole_width, &whole_height) && whole_height > 0) {
        app->auto_scale = pick_auto_scale(SDL_GetWindowDisplayScale(app->window), whole_height, GUI_AUTO_ROWS);
        SDL_SetWindowMinimumSize(
            app->window, 800,
            SDL_max(480, (int)SDL_ceilf(256.0f * (float)app->auto_scale / SDL_GetWindowPixelDensity(app->window))));
    }
    app->scale = app->menu.text_size ? app->menu.text_size : app->auto_scale;
    /* as long as 16 rows still fit: Auto's always do on a computer, a phone's not while Fullscreen is off */
    while (app->scale > 1 && height / (16 * app->scale) < 16) --app->scale;
    if (app->scale < 1) app->scale = 1;
    app->cols = SDL_min(width / (8 * app->scale), GUI_MAX_COLS);
    app->rows = SDL_min(height / (16 * app->scale), GUI_MAX_ROWS);
    run = (app->menu.running || app->finished) && !entering(app);
    app->log_top = run ? 4 : menu_row(app, GUI_EXIT) + 6;
    gui_browser_resize(&app->browser, boxed(app) && app->menu.view == GUI_LOAD_GAMES
                                          ? games_page(app)
                                          : (size_t)SDL_max(1, app->rows - 10));
    int log_rows = SDL_max(1, app->rows - app->log_top - 4);
    if (boxed(app) && run && app->theme.run != GUI_RUN_STEPS) { /* the run's output box */
        GuiPlace p;
        int cols, rows;
        run_place(app, &p);
        run_console(app, &p, &cols, &rows);
        gui_console_resize(&app->console, cols, rows);
    } else if (!boxed(app))
        gui_console_resize(&app->console, SDL_max(1, app->cols - 2), log_rows);
}
/* an arc of a circle, from and sweep in degrees, thickness pixels inward */
static void arc(App* app, float cx, float cy, float r, float from, float sweep, float thickness, GuiRgb c) {
    float t;
    SDL_FPoint p[49];
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, 255);
    for (t = 0; t < thickness; t += 1.0f) {
        int i;
        for (i = 0; i <= 48; ++i) {
            float a = (from + sweep * (float)i / 48) * SDL_PI_F / 180;
            p[i].x = cx + (r - t) * SDL_cosf(a);
            p[i].y = cy + (r - t) * SDL_sinf(a);
        }
        SDL_RenderLines(app->renderer, p, 49);
    }
}
/* the ring that turns while a run goes: a thin outer ring, a thicker inner one and a bar going round it */
static void ring(App* app, float cx, float cy) {
    float r = 7.0f * (float)app->scale, inner = r * 0.62f, thick = SDL_max(1.0f, 1.5f * (float)app->scale);
    float angle = app->finished ? 0 : (float)((now_ms(app) / 4) % 360);
    arc(app, cx, cy, r, 0, 360, SDL_max(1.0f, 0.5f * (float)app->scale), app->theme.run_colors.dim);
    arc(app, cx, cy, inner, 0, 360, thick, app->theme.run_colors.soft);
    arc(app, cx, cy, inner, angle, 70, thick, app->theme.run_colors.ink);
}
/* "", ".", "..", "..." round and round; "..." once the run is over */
static const char* dots(const App* app) {
    static const char* const turn[] = {"", ".", "..", "..."};
    return app->finished ? "..." : turn[(now_ms(app) / 400) % 4];
}
/* the game's name as the run knows it: the runner's banner, else the file's own, in capitals */
static void run_name(const App* app, char* out, size_t size) {
    const char* q;
    size_t n;
    const char* dot;
    const GuiStep* launch = gui_steps_last(&app->steps, "LAUNCH");
    const char* name = launch ? launch->detail : app->file ? app->file : "";
    for (q = name; *q; ++q)
        if (*q == '/' || *q == '\\') name = q + 1;
    n = 0;
    dot = launch ? NULL : strrchr(name, '.');
    for (q = name; *q && q != dot && n + 1 < size; ++q) out[n++] = *q >= 'a' && *q <= 'z' ? (char)(*q - 32) : *q;
    out[n] = 0;
}
/* a step's line: its label, its detail in capitals and its status */
static size_t step_runs(const GuiStep* s, int how, Uint64 now, char run[3][112], size_t length[3]) {
    char* q;
    size_t total;
    int r;
    snprintf(run[0], 112, "%s:", s->label);
    snprintf(run[1], 112, " %s", s->detail);
    for (q = run[1]; *q; ++q)
        if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    if (s->state == GUI_STEP_FAILED)
        snprintf(run[2], 112, "...FAILED");
    else if (s->state == GUI_STEP_DONE)
        snprintf(run[2], 112, "...OK");
    else {
        char progress[64];
        gui_step_progress(s, how, now, progress, sizeof progress);
        snprintf(run[2], 112, "%s%s", progress[0] ? " " : "", progress);
    }
    total = 0;
    for (r = 0; r < 3; ++r) total += length[r] = SDL_utf8strlen(run[r]);
    return total;
}
/* a row cut to the columns it has: a step's detail, keeping its label and state, or a line, ending in "..." */
static void clip_row(char run[3][112], size_t length[3], int note, int room) {
    int cut;
    size_t marker, other, left, i, at;
    const char* end;
    size_t total = length[0] + length[1] + length[2];
    if (room < 8 || total <= (size_t)room) return;
    cut = note ? 0 : 1;
    marker = note ? 3 : 0;
    other = total - length[cut];
    left = (size_t)room > other + marker ? (size_t)room - other - marker : 0;
    end = run[cut];
    for (i = 0; i < left; ++i) SDL_StepUTF8(&end, NULL);
    at = (size_t)(end - run[cut]);
    snprintf(run[cut] + at, 112 - at, "%s", note ? "..." : "");
    length[cut] = left + marker;
}
/* how far each of rows rows has typed at now, total[r] characters that came at shown[r]: async, each from when it came;
   sync, one after the other. A slow row types a character every typing_ms, any other the whole row in typing_ms, and
   one above first, off the screen, at once. Returns the row being typed, -1 for none */
static int type_rows(const GuiTheme* theme, int rows, int first, const size_t* total, const Uint64* shown,
                     const int* slow, Uint64 now, size_t* typed) {
    int r, typing = -1;
    Uint64 free_at = 0;
    for (r = 0; r < rows; ++r) {
        Uint64 start, span = slow[r] ? (Uint64)theme->typing_ms * total[r] : theme->typing_ms;
        typed[r] = total[r];
        if (r < first || theme->line_in != GUI_LINE_TYPING || !theme->typing_ms) continue;
        start = theme->typing_sync ? SDL_max(shown[r], free_at) : shown[r];
        free_at = start + span;
        if (now < start)
            typed[r] = 0;
        else if (now - start < span)
            typed[r] = (size_t)((now - start) * total[r] / span);
        if (now >= start && typed[r] < total[r]) typing = r;
    }
    return typing;
}
/* a run as a loading screen: the title, the name and the ring; the steps typed down the page, their status last;
   the progress bar and its percentage */
static void render_steps(App* app) {
    /* the rows: each step, then every line it kept when detailed, folded once it is done when collapse says so;
       each ends before the right edge, with room for a step's dots and the cursor */
    enum { ROWS = GUI_STEPS_MAX + GUI_NOTES_MAX, SEGMENTS = 40 };
    static char runs[ROWS][3][112];
    static size_t lengths[ROWS][3], typed[ROWS], totals[ROWS];
    static const GuiStep* owner[ROWS];
    static int is_note[ROWS], slow[ROWS];
    static Uint64 shown[ROWS];
    static const GuiRgb white = {255, 255, 255};
    const GuiTheme* theme = &app->theme;
    const GuiPalette* colors = &theme->run_colors;
    /* since: a line that came while the menu faded out comes in with the run */
    Uint64 now = now_ms(app), since = app->fade_ms;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale;
    int width, height, name_col, col, y, i, r, rows = 0, last_step = -1, typing, top = 3, bottom, fit, first, cursor;
    int foot = theme->footer != GUI_STYLE_NONE ? 2 : 0; /* the footer's rows, over the ESC line */
    const char* title;
    char name[96], head[128];
    SDL_Color ink, soft, failure;
    area(app, &width, &height);
    if (theme->texture)
        for (y = 3; y < height; y += 4) fill(app, 0, (float)y, (float)width, 1, white, 14);
    ink = rgb(colors->ink);
    soft = rgb(colors->soft);
    failure = rgb(colors->failure);

    run_name(app, name, sizeof name);
    col = theme->ring == GUI_RING_LEFT ? 4 : 1;
    title = theme->title[screen(app)];
    name_col = col + (int)SDL_utf8strlen(title) + (title[0] ? 1 : 0);
    if (theme->ring == GUI_RING_LEFT) ring(app, cw * 2, ch * 1.5f);
    if (theme->ring == GUI_RING_RIGHT) ring(app, (float)width - cw * 2, ch * 1.5f);
    if (theme->ring == GUI_RING_NEAR_TEXT) /* after the name and the dots' whole width, so it stays put */
        ring(app, cw * (float)(name_col + (int)SDL_utf8strlen(name) + 4) + 7.0f * (float)app->scale, ch * 1.5f);
    text(app, col, 1, title, ink);
    snprintf(head, sizeof head, "%s%s", name, dots(app));
    text(app, name_col, 1, head, soft);
    if (app->finished) text(app, 1, 2, app->message, app->abnormal_exit ? failure : soft);
    for (i = 0; i < app->steps.count; ++i) {
        int n;
        const GuiNote* notes[GUI_NOTES_MAX];
        int k;
        const GuiStep* s = &app->steps.step[i];
        owner[rows] = s;
        is_note[rows] = 0;
        shown[rows] = SDL_max(s->shown_ms, since);
        step_runs(s, theme->progress, now, runs[rows], lengths[rows]);
        clip_row(runs[rows], lengths[rows], 0, app->cols - 6);
        last_step = rows++;
        if (!theme->detailed || (theme->collapse && s->state == GUI_STEP_DONE)) continue;

        n = gui_step_notes(&app->steps, s, notes, GUI_NOTES_MAX);
        for (k = 0; k < n; ++k, ++rows) {
            owner[rows] = s;
            is_note[rows] = 1;
            shown[rows] = SDL_max(notes[k]->shown_ms, since);
            snprintf(runs[rows][0], sizeof runs[rows][0], "%s", notes[k]->text);
            runs[rows][1][0] = runs[rows][2][0] = 0;
            lengths[rows][0] = SDL_utf8strlen(runs[rows][0]);
            lengths[rows][1] = lengths[rows][2] = 0;
            clip_row(runs[rows], lengths[rows], 1, app->cols - 5);
        }
    }
    bottom = (theme->bar ? app->rows - 5 : app->rows - 2) - foot;
    fit = SDL_max(1, bottom - top);
    first = SDL_max(0, rows - fit);
    /* the newest row of a step still running types readably; a done step's, and one a newer row of its step passed,
       at once but for a glimpse: a fast log keeps the cursor on its latest lines */
    for (r = 0; r < rows; ++r) {
        totals[r] = lengths[r][0] + lengths[r][1] + lengths[r][2];
        slow[r] = owner[r]->state == GUI_STEP_ACTIVE && (r + 1 == rows || owner[r + 1] != owner[r]);
    }
    typing = type_rows(theme, rows, first, totals, shown, slow, now, typed);
    cursor = typing >= 0 ? typing : last_step;
    for (r = first; r < rows; ++r) {
        int working, blink, k;
        size_t left;
        const GuiStep* s = owner[r];
        int last = s == &app->steps.step[app->steps.count - 1];
        int failed = s->state == GUI_STEP_FAILED, active = s->state == GUI_STEP_ACTIVE;
        size_t total = lengths[r][0] + lengths[r][1] + lengths[r][2];
        Uint64 age = now > shown[r] ? now - shown[r] : 0;
        int moving = theme->line_in == GUI_LINE_SLIDE || theme->line_in == GUI_LINE_FADE;
        float p = moving && theme->line_in_ms ? SDL_min(1.0f, (float)age / (float)theme->line_in_ms) : 1.0f;
        float x = cw * (is_note[r] ? 3 : 1) -
                  (theme->line_in == GUI_LINE_SLIDE ? 16.0f * (float)app->scale * (1 - p) * (1 - p) : 0);
        float y = ch * (float)(top + r - first);
        SDL_Color color[3];
        color[0] = failed ? failure : last ? ink : soft;
        color[1] = failed ? failure : last || active ? ink : soft;
        color[2] = failed ? failure : ink;
        if (is_note[r]) color[0] = soft;
        left = typed[r];
        for (k = 0; k < 3; ++k) {
            size_t drawn = SDL_min(left, lengths[r][k]);
            x = text_px(app, x, y, runs[r][k], drawn, color[k], p);
            left -= drawn;
        }
        if (!is_note[r] && typed[r] == total && active) x = text_px(app, x, y, dots(app), 3, ink, p);
        blink = !theme->blink_ms || (now / theme->blink_ms) % 2 == 0;
        working = typing == r || (typing < 0 && active);
        if (r == cursor && working && blink) text_px(app, x, y, "_", 1, ink, p);
    }
    if (theme->bar) {
        int k, percent = gui_steps_percent(&app->steps);
        int filled = percent < 0 ? 0 : (percent * SEGMENTS + 50) / 100;
        float span = (float)(app->cols - 8) * cw, gap = 2.0f * (float)app->scale;
        float seg = (span - gap * (SEGMENTS - 1)) / SEGMENTS, sy = ch * (float)(app->rows - foot - 3) + ch * 0.3f;
        char number[16];
        fill(app, cw, ch * (float)(app->rows - foot - 4) + ch / 2, (float)(app->cols - 2) * cw, (float)app->scale,
             colors->dim, 255);
        for (k = 0; k < SEGMENTS; ++k) {
            SDL_FRect r = {cw + (float)k * (seg + gap), sy, seg, ch * 0.4f};
            if (k < filled)
                fill(app, r.x, r.y, r.w, r.h, colors->ink, 255);
            else {
                SDL_SetRenderDrawColor(app->renderer, colors->dim.r, colors->dim.g, colors->dim.b, 255);
                SDL_RenderRect(app->renderer, &r);
            }
        }
        if (percent < 0)
            snprintf(number, sizeof number, " --%%");
        else
            snprintf(number, sizeof number, "%3d%%", percent);
        text(app, app->cols - 6, app->rows - foot - 3, number, ink);
        text(app, 1, app->rows - foot - 2, name, ink);
    }
    if (foot) footer(app, app->rows - 3, 2, colors->page, colors->ink, colors->soft);
    if (app->finished)
        prompt(app, app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, app->rows - 1, SAY_DONE, soft);
    else if (app->aborting)
        prompt(app, GUI_PROMPT_ABORTING, 1, app->rows - 1, "Aborting process tree...", rgb(colors->warning));
    else
        text(app, 1, app->rows - 1, SAY_ABORT, soft);
}
/* the screen fading in from black, or the menu or the run fading out to it */
static void overlay(App* app) {
    static const GuiRgb black = {0, 0, 0};
    SDL_Rect safe;
    int width;
    int height;
    const GuiTheme* theme = &app->theme;
    Uint64 now = now_ms(app);
    float a = 0;
    if (entering(app))
        a = (float)(now > app->entering_ms ? now - app->entering_ms : 0) / (float)theme->fade_out_ms;
    else if (app->leaving_ms && theme->fade_out_ms)
        a = SDL_min(1.0f, (float)(now > app->leaving_ms ? now - app->leaving_ms : 0) / (float)theme->fade_out_ms);
    else if (app->fade_ms && theme->fade_in_ms && now < app->fade_ms + theme->fade_in_ms)
        a = 1.0f - (float)(now > app->fade_ms ? now - app->fade_ms : 0) / (float)theme->fade_in_ms;
    if (a <= 0) return;

    /* the whole window, the cutout beside the text too */
    SDL_GetRenderViewport(app->renderer, &safe);
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    SDL_SetRenderViewport(app->renderer, NULL);
    fill(app, 0, 0, (float)width, (float)height, black, (Uint8)(a * 255));
    SDL_SetRenderViewport(app->renderer, &safe);
}
static const char* ask_question(const App* app) {
    if (app->confirm.exit) return "Exit ScarletNX?";
    return screen(app) == GUI_SCREEN_COMPILING ? "Abort the recompile? Its work is lost." : "Abort the run?";
}
/* the question's box, in cells: a blank row, the question, a blank row, its two choices and a blank row, two columns
   either side of the question */
static void ask_box(const App* app, int* col, int* row, int* cols) {
    *cols = SDL_min(app->cols - 2, (int)SDL_utf8strlen(ask_question(app)) + 4);
    *col = (app->cols - *cols) / 2;
    *row = SDL_max(0, (app->rows - 6) / 2);
}
#define SAY_LEFT_BY    " was left by an older version: this one keeps it in "
#define SAY_LEFT_TITLE "An older version left these files: move each from - to ->"
/* s at column x of row y, wrapped to width columns: first before its first row, as many spaces before the others. A
   NULL color only counts them. The rows it takes */
static int wrapped(App* app, int x, int y, const char* first, const char* s, int width, const SDL_Color* color) {
    char row[1100];
    int indent = (int)strlen(first), rows = 0;
    do {
        size_t n = gui_wrap(s, width - indent);
        if (color) {
            snprintf(row, sizeof row, "%*s%.*s", indent, rows ? "" : first, (int)n, s);
            text(app, x, y + rows, row, *color);
        }
        s += n;
        ++rows;
    } while (*s);
    return rows;
}
/* a line of app->left: the file where an older build left it, and where this one keeps it */
static void left_paths(const char* line, char* old, char* now, size_t size) {
    const char* by = strstr(line, SAY_LEFT_BY);
    snprintf(old, size, "%.*s", (int)(by - line), line);
    by += strlen(SAY_LEFT_BY);
    snprintf(now, size, "%.*s", (int)strcspn(by, "\n"), by);
}
/* the box with what older builds left: each file where it is (-) and where this one would keep it (->, a proposal, so
   not in a diff's added colour), whole over as many rows as it needs, as many files as fit, and OK. Nothing is moved:
   the player does that */
static void left_boxed(App* app);
static void render_left(App* app) {
    const GuiTheme* t = &app->theme;
    SDL_Color warning = rgb(t->warning), failure = rgb(t->failure), ink = rgb(t->ink), soft = rgb(t->soft);
    int width = SDL_max(8, app->cols - 7), title, room, used = 0, shown, rows, row, y;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale, line = (float)app->scale;
    const char* at;
    char old[4200], now[4200], label[64];
    if (!app->left_shown || !app->lefts) return;
    if (boxed(app)) {
        left_boxed(app);
        return;
    }
    /* a blank row, the title, a blank row, the files, a blank row or how many more, OK and a blank row */
    title = wrapped(app, 0, 0, "", SAY_LEFT_TITLE, width, NULL);
    room = app->rows - 2 - (title + 5);
    for (shown = 0, at = app->left; shown < app->lefts; ++shown, at = strchr(at, '\n') + 1) {
        int need;
        left_paths(at, old, now, sizeof old);
        need = wrapped(app, 0, 0, "- ", old, width, NULL) + wrapped(app, 0, 0, "-> ", now, width, NULL);
        if (shown && used + need > room) break;
        used += need;
    }
    rows = title + used + 5;
    row = SDL_max(0, (app->rows - rows) / 2);
    fill(app, 2 * cw, (float)row * ch, (float)(app->cols - 3) * cw, (float)rows * ch, t->soft, 255); /* framed */
    fill(app, 2 * cw + line, (float)row * ch + line, (float)(app->cols - 3) * cw - 2 * line,
         (float)rows * ch - 2 * line, t->page, 255);
    y = row + 1 + wrapped(app, 4, row + 1, "", SAY_LEFT_TITLE, width, &warning) + 1;
    at = app->left;
    for (int i = 0; i < shown; ++i, at = strchr(at, '\n') + 1) {
        left_paths(at, old, now, sizeof old);
        y += wrapped(app, 4, y, "- ", old, width, &failure);
        y += wrapped(app, 4, y, "-> ", now, width, &ink);
    }
    if (shown < app->lefts) {
        snprintf(label, sizeof label, "and %d more, which Share log lists", app->lefts - shown);
        text(app, 4, y, label, soft);
    }
    snprintf(label, sizeof label, "%sOK, nothing is moved", t->marker);
    text(app, 4, row + rows - 2, label, rgb(t->focus));
}
/* the question over a running run, in the run screen's colours, its choices as the menu's entries */
static void ask_boxed(App* app);
static void render_ask(App* app) {
    static const char* const choices[2][2] = {{"Keep going", "Abort"}, {"Stay", "Exit"}}; /* by exit, GUI_CONFIRM_* */
    const GuiTheme* t = &app->theme;
    int steps = t->run == GUI_RUN_STEPS && !app->confirm.exit, col, row, cols, i;
    GuiRgb page = steps ? t->run_colors.page : t->page, ink = steps ? t->run_colors.ink : t->ink;
    GuiRgb soft = steps ? t->run_colors.soft : t->soft, warning = steps ? t->run_colors.warning : t->warning;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale, line = (float)app->scale;
    if (!app->confirm.shown || (!app->menu.running && !app->confirm.exit)) return;
    if (boxed(app)) {
        ask_boxed(app);
        return;
    }

    ask_box(app, &col, &row, &cols);
    fill(app, (float)col * cw, (float)row * ch, (float)cols * cw, 6 * ch, soft, 255); /* framed in soft */
    fill(app, (float)col * cw + line, (float)row * ch + line, (float)cols * cw - 2 * line, 6 * ch - 2 * line, page,
         255);
    text(app, col + 2, row + 1, ask_question(app), rgb(warning));
    for (i = 0; i < 2; ++i) {
        char label[64];
        int focused = app->confirm.selected == i, bar = focused && (steps || t->focus_bar);
        snprintf(label, sizeof label, "%s%s", t->marker, choices[app->confirm.exit][i]);
        if (bar)
            fill(app, (float)col * cw + line, (float)(row + 3 + i) * ch, (float)cols * cw - 2 * line, ch, ink, 255);
        text(app, col + 2, row + 3 + i, label, rgb(bar ? page : focused ? t->focus : ink));
    }
}
/* the boot's splash: the program's icon on a theme's splash page, then the menu as the theme brings it in */
static void splash_start(App* app) {
    const GuiTheme* t = gui_splash_theme(&app->theme, (int)SDL_rand(100));
    SDL_Surface* icon = t->splash_ms ? snx_window_icon_surface() : NULL;
    if (!icon) return;
    app->splash = SDL_CreateTextureFromSurface(app->renderer, icon);
    SDL_DestroySurface(icon);
    app->splash_page = t->splash_page;
    app->splash_until = SDL_GetTicks() + t->splash_ms;
}
static int splash_shown(App* app) {
    SDL_Rect view;
    float size;
    if (!app->splash) return 0;
    if (now_ms(app) >= app->splash_until) {
        SDL_DestroyTexture(app->splash);
        app->splash = NULL;
        app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
        return 0;
    }
    SDL_GetRenderViewport(app->renderer, &view);
    size = 128.0f * (float)SDL_max(1, SDL_min(view.w, view.h) / 3 / 128); /* whole steps of its 128 pixels */
    SDL_SetRenderDrawColor(app->renderer, app->splash_page.r, app->splash_page.g, app->splash_page.b, 255);
    SDL_RenderClear(app->renderer);
    {
        SDL_FRect at = {((float)view.w - size) / 2, ((float)view.h - size) / 2, size, size};
        SDL_RenderTexture(app->renderer, app->splash, NULL, &at);
    }
    SDL_RenderPresent(app->renderer);
    return 1;
}
/* ---- the menus in boxes ([body] boxes line or fill): place.c puts each box, and nothing goes over another ---- */
#ifdef SDL_PLATFORM_ANDROID
#define SAY_MENU_KEYS SAY_MENU
#else
#define SAY_MENU_KEYS SAY_MENU " ESC: exit."
#endif
static int boxed(const App* app) { return app->theme.boxes != GUI_BOXES_NONE; }
static int cell_w(const App* app) { return 8 * app->scale; }
static int cell_h(const App* app) { return 16 * app->scale; }
static GuiRect box_rect(int x, int y, int w, int h) {
    GuiRect r;
    r.x = x;
    r.y = y;
    r.w = w > 0 ? w : 0;
    r.h = h > 0 ? h : 0;
    return r;
}
static void fill_rect(App* app, GuiRect r, GuiRgb c) {
    if (r.w > 0 && r.h > 0) fill(app, (float)r.x, (float)r.y, (float)r.w, (float)r.h, c, 255);
}
static void outline(App* app, GuiRect r, GuiRgb c) {
    int s = app->scale;
    fill_rect(app, box_rect(r.x, r.y, r.w, s), c);
    fill_rect(app, box_rect(r.x, r.y + r.h - s, r.w, s), c);
    fill_rect(app, box_rect(r.x, r.y, s, r.h), c);
    fill_rect(app, box_rect(r.x + r.w - s, r.y, s, r.h), c);
}
/* value from pixel x, y up to the right edge of r: a line too long for it slides as the theme says */
static void text_in(App* app, GuiRect r, int x, int y, const char* value, SDL_Color color) {
    int w = cell_w(app), room = (r.x + r.w - x) / w, skip;
    if (room <= 0) return;
    skip = gui_slide((int)SDL_utf8strlen(value) - room, now_ms(app), app->theme.scroll_ms);
    while (skip-- > 0 && *value) SDL_StepUTF8(&value, NULL);
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    for (; *value && room > 0; --room, x += w) glyph(app, (float)x, (float)y, gui_glyph(SDL_StepUTF8(&value, NULL)));
}
/* the bytes of s's first row at width columns: a path's up to a slash (gui_wrap), other text's up to a space */
static size_t wrap_at(const char* s, int width, int path) {
    const char *p = s, *space = NULL;
    int n;
    if (path) return gui_wrap(s, width);
    for (n = 0; *p && n < width; ++n) {
        SDL_StepUTF8(&p, NULL);
        if (*p == ' ') space = p;
    }
    return !*p ? strlen(s) : space ? (size_t)(space - s) : gui_wrap(s, width);
}
/* s from pixel x, y wrapped to the right edge of r (path: at its slashes), a row under the other while they fit above
   its bottom: the rows it took */
static int wrap_in(App* app, GuiRect r, int x, int y, const char* s, int path, SDL_Color color) {
    char row[1100];
    int width = (r.x + r.w - x) / cell_w(app), rows = 0;
    if (width <= 0) return 0;
    while (*s && y + (rows + 1) * cell_h(app) <= r.y + r.h) {
        size_t n = wrap_at(s, width, path);
        snprintf(row, sizeof row, "%.*s", (int)n, s);
        text_in(app, r, x, y + rows * cell_h(app), row, color);
        for (s += n; *s == ' ';) ++s; /* a row starts at a word */
        ++rows;
    }
    return rows;
}
/* the rows s takes wrapped to width columns */
static int wrap_rows(const char* s, int width, int path) {
    int rows = 0;
    if (width <= 0) return 1;
    for (; *s; ++rows)
        for (s += wrap_at(s, width, path); *s == ' ';) ++s;
    return SDL_max(rows, 1);
}
/* r without a column at either side: where a box's text goes */
static GuiRect inner(const App* app, GuiRect r) { return box_rect(r.x + cell_w(app), r.y, r.w - 2 * cell_w(app), r.h); }
/* a box head's height: its label, two pixels of the text above and below */
static int head_h(const App* app) { return cell_h(app) + 4 * app->scale; }
/* a box framed in the dim colour, filled with the page's (fill) or the frame alone (line); with a head when label is
   not NULL, its label on a band of ink (fill) or over a rule (line). Where its text starts */
static int pane(App* app, GuiRect r, const char* label) {
    const GuiTheme* t = &app->theme;
    int s = app->scale, filled = t->boxes == GUI_BOXES_FILL;
    if (filled) fill_rect(app, r, t->page);
    if (label && filled) fill_rect(app, box_rect(r.x, r.y, r.w, head_h(app)), t->ink);
    if (label && !filled) fill_rect(app, box_rect(r.x, r.y + head_h(app) - s, r.w, s), t->dim);
    outline(app, r, t->dim);
    if (label) text_in(app, inner(app, r), r.x + cell_w(app), r.y + 2 * s, label, rgb(filled ? t->page : t->ink));
    return r.y + (label ? head_h(app) : 0) + 2 * s;
}

/* a list of boxes in a column of the screen: its box, the cursor's margin before the boxes and the arrows' column after
   them when entries are hidden, each box's height and the pitch from one to the next, the first shown and how many */
typedef struct {
    GuiRect at;
    int count, focus, margin, arrows, item, pitch, first, shown;
} GuiList;
/* a list entry's box: its text under a frame's width, the frame at its foot over the font's last row, which is empty,
   so many fit; its text's row in it */
static int item_h(const App* app) { return cell_h(app) + app->scale; }
static int item_text_y(const App* app, GuiRect b) { return b.y + app->scale; }
static void list_place(const App* app, GuiList* l, GuiRect at, int count, int focus) {
    int s = app->scale;
    l->at = at;
    l->count = count;
    l->focus = focus;
    l->margin = app->theme.cursor[0] ? ((int)SDL_utf8strlen(app->theme.cursor) * cell_w(app)) : 0;
    l->item = item_h(app);
    l->pitch = gui_place_pitch(at.h, count, l->item + 2 * s, l->item + 14 * s);
    l->shown = gui_place_list(at.h, l->item, l->pitch, count, focus, &l->first);
    l->arrows = l->shown < count ? cell_w(app) : 0;
}
static GuiRect list_box(const GuiList* l, int i) {
    return box_rect(l->at.x + l->margin, l->at.y + (i - l->first) * l->pitch, l->at.w - l->margin - l->arrows, l->item);
}
/* the entry under pixel x, y, -1 for none; a finger takes the whole width */
static int list_hit(const GuiList* l, float x, float y, bool touch) {
    int i;
    for (i = l->first; i < l->first + l->shown; ++i) {
        GuiRect b = list_box(l, i);
        if (y >= (float)b.y && y < (float)(b.y + b.h + (l->pitch - l->item)) &&
            (touch ? x >= (float)l->at.x && x < (float)(l->at.x + l->at.w) : x >= (float)b.x && x < (float)(b.x + b.w)))
            return i;
    }
    return -1;
}
/* an entry as a box of its own: on the dim colour (fill) or framed in it (line); the focused one inverted on the focus
   colour with a rule over and under it (fill), or framed and written in it; the cursor in the margin */
static void list_item(App* app, const GuiList* l, int i, const char* label, int focused, int activated) {
    const GuiTheme* t = &app->theme;
    GuiRect b = list_box(l, i);
    int s = app->scale, filled = t->boxes == GUI_BOXES_FILL, inverted = focused && t->focus_bar;
    if (inverted)
        fill_rect(app, b, t->focus);
    else if (filled)
        fill_rect(app, b, t->dim);
    if (!inverted && (!filled || focused)) outline(app, b, focused ? t->focus : t->dim);
    if (focused && filled && l->pitch - l->item >= 6 * s) { /* where the gap has room for them */
        fill_rect(app, box_rect(b.x, b.y - 3 * s, b.w, s), t->focus);
        fill_rect(app, box_rect(b.x, b.y + b.h + 2 * s, b.w, s), t->focus);
    }
    text_in(app, inner(app, b), b.x + cell_w(app) / 2, item_text_y(app, b), label,
            rgb(inverted    ? t->page
                : activated ? t->active
                : focused   ? t->focus
                            : t->ink));
    if (focused && t->cursor[0]) text_in(app, l->at, l->at.x, item_text_y(app, b), t->cursor, rgb(t->focus));
}
/* a label between a list's boxes, as "Recent games" */
static void list_label(App* app, const GuiList* l, int i, const char* label) {
    GuiRect b = list_box(l, i);
    text_in(app, b, b.x, item_text_y(app, b), label, rgb(app->theme.soft));
}
/* arrows past the boxes' right edge while entries are hidden above or below them */
static void list_marks(App* app, const GuiList* l) {
    int x = l->at.x + l->at.w - cell_w(app);
    if (l->first > 0) text_in(app, l->at, x, item_text_y(app, list_box(l, l->first)), "\x1e", rgb(app->theme.soft));
    if (l->first + l->shown < l->count)
        text_in(app, l->at, x, item_text_y(app, list_box(l, l->first + l->shown - 1)), "\x1f", rgb(app->theme.soft));
}

/* The bar at the foot: what the focus is about, or the screen's message, over the screen's keys. keys are "Keys: what
   they do." sentences, each drawn as the keys, inverted (the theme's focus bar) or in the focus colour, and what they
   do; a row of the bar holds as many as fit, against its right edge */
static int hint_next(const char** s, char* key, char* what, size_t size) {
    const char *p = *s, *end, *colon;
    size_t n;
    while (*p == ' ') ++p;
    if (!*p) return 0;
    end = strstr(p, ". ");
    if (!end) end = p + strlen(p) - (p[strlen(p) - 1] == '.');
    colon = strstr(p, ": ");
    if (colon && colon > end) colon = NULL;
    n = colon ? (size_t)(colon - p) : 0;
    snprintf(key, size, "%.*s", (int)n, p);
    p = colon ? colon + 2 : p;
    snprintf(what, size, "%.*s", (int)(end - p), p);
    *s = *end ? end + 1 : end;
    return 1;
}
static int hint_width(const char* key, const char* what) {
    return (int)SDL_utf8strlen(key) + (key[0] ? 1 : 0) + (int)SDL_utf8strlen(what);
}
/* the rows keys take in width columns; with draw, drawn from pixel y in r */
static int hints(App* app, const char* keys, int width, GuiRect r, int y, int draw) {
    char key[128], what[128];
    const char* row = keys;
    int rows = 0, s = app->scale;
    for (;;) {
        const char *at = row, *end = row, *u = row;
        int used = 0, x;
        while (hint_next(&at, key, what, sizeof key)) {
            int w = hint_width(key, what) + (used ? 2 : 0);
            if (used && used + w > width) break;
            used += w;
            end = at;
        }
        if (end == row) break;
        for (x = r.x + r.w - used * cell_w(app); draw && u != end && hint_next(&u, key, what, sizeof key);) {
            int cy = y + rows * cell_h(app), k = (int)SDL_utf8strlen(key);
            if (k && app->theme.focus_bar)
                fill_rect(app, box_rect(x - s, cy, k * cell_w(app) + 2 * s, cell_h(app)), app->theme.focus);
            if (k) text_in(app, r, x, cy, key, rgb(app->theme.focus_bar ? app->theme.page : app->theme.focus));
            x += (k + (k ? 1 : 0)) * cell_w(app);
            text_in(app, r, x, cy, what, rgb(app->theme.ink));
            x += ((int)SDL_utf8strlen(what) + 2) * cell_w(app);
        }
        ++rows;
        row = end;
    }
    return SDL_max(rows, 1);
}
/* the bar's text columns, in a box width pixels wide: past its tick, a column in from either side */
static int bar_cols(const App* app, int width) { return width / cell_w(app) - 4; }
static int bar_h(App* app, int width, const char* keys) {
    GuiRect none = {0, 0, 0, 0};
    return (1 + hints(app, keys, bar_cols(app, width), none, 0, 0)) * cell_h(app) + 4 * app->scale;
}
static void bar(App* app, GuiRect r, const char* about, SDL_Color color, const char* keys) {
    const GuiTheme* t = &app->theme;
    int s = app->scale;
    GuiRect text = box_rect(r.x + 3 * cell_w(app), r.y, r.w - 4 * cell_w(app), r.h);
    pane(app, r, NULL);
    fill_rect(app, box_rect(r.x + 2 * s, r.y + 2 * s, 3 * s, r.h - 4 * s),
              t->boxes == GUI_BOXES_FILL ? t->ink : t->soft);
    text_in(app, text, text.x, r.y + 2 * s, about, color);
    hints(app, keys, text.w / cell_w(app), text, r.y + 2 * s + cell_h(app), 1);
}
/* the screen's title in its box: on a band of ink with the head's bar style */
/* the title's letters, times the text's */
static int title_size(const App* app) { return (int)SDL_clamp(app->theme.title_size, 1, 3); }
/* value from pixel x, y in letters size times the text's, cut at the right edge of r */
static void big_text(App* app, GuiRect r, int x, int y, const char* value, int size, SDL_Color color) {
    float w = 8.0f * (float)(app->scale * size), h = 16.0f * (float)(app->scale * size);
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    for (; *value && (float)x + w <= (float)(r.x + r.w); x += (int)w) {
        unsigned char c = gui_glyph(SDL_StepUTF8(&value, NULL));
        SDL_FRect source = {(float)(c % 16 * 8), (float)(c / 16 * 16), 8, 16}, at = {(float)x, (float)y, w, h};
        SDL_RenderTexture(app->renderer, app->font, &source, &at);
    }
}
/* the screen's title in its box, its letters as big as the head's size says over their shadow; on a band of ink with
   the head's bar style */
static void title_box(App* app, GuiRect r, const char* title) {
    const GuiTheme* t = &app->theme;
    int bar = t->head == GUI_STYLE_BAR, size = title_size(app), x = r.x + (bar ? cell_w(app) : 0);
    if (!r.h) return;
    if (bar) fill_rect(app, r, t->ink);
    if (t->title_shadow) big_text(app, r, x + size * app->scale, r.y + size * app->scale, title, size, rgb(t->dim));
    big_text(app, r, x, r.y, title, size, rgb(bar ? t->page : t->ink));
}
/* the theme's title for a screen, "" when the head shows none */
static const char* boxed_title(const App* app, GuiScreen s) {
    return app->theme.head == GUI_STYLE_NONE ? "" : app->theme.title[s];
}
/* the status box: the usage, the disk and the working folder, wrapped to width columns; with draw, drawn in r */
static int status_rows(App* app, int width, GuiRect r, int y, int draw) {
    char usage[256], disk[96], cwd[1100];
    const char* lines[3];
    int i, rows = 0;
    SDL_Color soft = rgb(app->theme.soft), ink = rgb(app->theme.ink);
    gui_usage_text(usage, sizeof usage, app->usage.available, app->usage.memory_bytes, app->usage.cpu_percent,
                   SDL_GetNumLogicalCPUCores());
    disk_text(app, disk, sizeof disk);
    snprintf(cwd, sizeof cwd, "CWD: %s", app->cwd ? app->cwd : "(unavailable)");
    lines[0] = usage;
    lines[1] = disk;
    lines[2] = cwd;
    for (i = 0; i < 3; ++i) {
        if (!lines[i][0]) continue;
        if (draw)
            rows += wrap_in(app, r, r.x, y + rows * cell_h(app), lines[i], i == 2, i == 2 ? ink : soft);
        else
            rows += wrap_rows(lines[i], width, i == 2);
    }
    return rows;
}

/* the screens the tabs go round, Tab and Shift+Tab or a click on one: the menu, Games and the Settings */
enum { GUI_TAB_MENU, GUI_TAB_GAMES, GUI_TAB_SETTINGS, GUI_TABS };
static const char* const tab_names[GUI_TABS] = {"Menu", "Games", "Settings"};
static int tab_shown(const App* app) {
    if (app->editing) return GUI_TAB_SETTINGS;
    return app->menu.view == GUI_LOAD_GAMES ? GUI_TAB_GAMES : GUI_TAB_MENU;
}
static GuiRect tab_rect(const App* app, GuiRect row, int i) {
    int x = row.x, k;
    for (k = 0; k < i; ++k) x += ((int)strlen(tab_names[k]) + 3) * cell_w(app);
    return box_rect(x, row.y, ((int)strlen(tab_names[i]) + 2) * cell_w(app), row.h);
}
/* the tabs, the one shown as the focused box is, over a rule */
static void draw_tabs(App* app, GuiRect row) {
    const GuiTheme* t = &app->theme;
    int i, s = app->scale, shown = tab_shown(app);
    if (!row.h) return;
    for (i = 0; i < GUI_TABS; ++i) {
        GuiRect r = tab_rect(app, row, i);
        int on = i == shown, inverted = on && t->focus_bar;
        if (inverted)
            fill_rect(app, r, t->focus);
        else if (t->boxes == GUI_BOXES_FILL)
            fill_rect(app, r, t->dim);
        if (!inverted && (t->boxes == GUI_BOXES_LINE || on)) outline(app, r, on ? t->focus : t->dim);
        text_in(app, r, r.x + cell_w(app), r.y + 2 * s, tab_names[i], rgb(inverted ? t->page : on ? t->focus : t->ink));
    }
    fill_rect(app, box_rect(row.x, row.y + row.h + cell_h(app) / 4 - s / 2, row.w, s), t->dim);
}
/* a screen's keys, Tab's after them where the tabs show */
static const char* screen_keys(const App* app, const char* keys, char* out, size_t size) {
#ifdef SDL_PLATFORM_ANDROID
    (void)app;
    (void)out;
    (void)size;
    return keys; /* a tap on a tab */
#else
    if (!app->theme.tabs) return keys;
    snprintf(out, size, "%s Tab: screen.", keys);
    return out;
#endif
}
/* an edge's band: its rule, the ticks under it and the marks */
static int edge_h(const App* app) { return 12 * app->scale; }
/* an edge along r: a line with a tick between the marks when the theme's rule says so, its mark repeated under it
   spacing pixels (at 1x) apart */
static void edge(App* app, GuiRect r) {
    const GuiTheme* t = &app->theme;
    int s = app->scale, step = (int)t->mark_spacing * s, x;
    GuiRgb c = t->edge_color;
    if (!r.h) return;
    if (t->edge_rule) fill_rect(app, box_rect(r.x, r.y + 2 * s, r.w, s), c);
    for (x = r.x + step / 2; step && x < r.x + r.w; x += step) {
        if (t->edge_rule) fill_rect(app, box_rect(x - step / 2 - 2 * s, r.y + 3 * s, 4 * s, 2 * s), c);
        if (t->mark == GUI_MARK_DOT) fill_rect(app, box_rect(x - s, r.y + 6 * s, 2 * s, 2 * s), c);
        if (t->mark == GUI_MARK_DASH) fill_rect(app, box_rect(x - 3 * s, r.y + 6 * s, 6 * s, 2 * s), c);
        if (t->mark == GUI_MARK_DOTS) {
            fill_rect(app, box_rect(x - 3 * s, r.y + 5 * s, 2 * s, 2 * s), c);
            fill_rect(app, box_rect(x + s, r.y + 5 * s, 2 * s, 2 * s), c);
            fill_rect(app, box_rect(x - s, r.y + 8 * s, 2 * s, 2 * s), c);
        }
    }
}
/* what a screen in boxes draws before its own: the edges, the tabs */
static void chrome(App* app, const GuiPlace* p) {
    edge(app, p->edge_top);
    edge(app, p->edge_bottom);
    draw_tabs(app, p->tabs);
}
/* what a screen has besides its title, list and bar: the status box when the theme has a footer, the tabs when it
   shows them */
enum { GUI_SHOWS_STATUS = 1, GUI_SHOWS_TABS = 2 };
/* a screen's boxes: the tabs, its title, the bar for its keys, a list asking for list_cols columns, three fifths of the
   width at most (0: one box, the whole width), and the status box, as shows says */
static void place_screen(App* app, GuiPlace* p, const char* title, const char* keys, int list_cols, int shows) {
    GuiPlaceIn in;
    int width, height;
    area(app, &width, &height);
    memset(&in, 0, sizeof in);
    in.width = width;
    in.height = height;
    in.cw = cell_w(app);
    in.ch = cell_h(app);
    in.tabs = shows & GUI_SHOWS_TABS && app->theme.tabs ? cell_h(app) + 4 * app->scale : 0;
    in.edge_top = app->theme.edges == GUI_EDGES_TOP || app->theme.edges == GUI_EDGES_BOTH ? edge_h(app) : 0;
    in.edge_bottom = app->theme.edges == GUI_EDGES_BOTTOM || app->theme.edges == GUI_EDGES_BOTH ? edge_h(app) : 0;
    in.title =
        title[0] ? title_size(app) * cell_h(app) + (app->theme.title_shadow ? title_size(app) * app->scale : 0) : 0;
    in.bar = bar_h(app, width - 2 * cell_w(app), keys);
    in.list_cols = list_cols ? SDL_min(SDL_max(list_cols, 20), SDL_max(20, (width / cell_w(app) - 2) * 3 / 5)) : 0;
    gui_place(&in, p);
    if (shows & GUI_SHOWS_STATUS && in.list_cols && app->theme.footer != GUI_STYLE_NONE) { /* at the side's width */
        GuiRect none = {0, 0, 0, 0};
        in.status = head_h(app) + 4 * app->scale +
                    status_rows(app, inner(app, p->side).w / cell_w(app), none, 0, 0) * cell_h(app);
        gui_place(&in, p);
    }
}
static void status_box(App* app, GuiRect r) {
    GuiRect text = inner(app, r);
    int y;
    if (!r.h) return;
    y = pane(app, r, "Status");
    status_rows(app, text.w / cell_w(app), box_rect(text.x, y, text.w, r.y + r.h - y), y, 1);
}
/* the bar's first row: the screen's message while there is one, a failure's in its colour; else about */
static void bar_for(App* app, GuiRect r, const char* about, const char* keys) {
    const GuiTheme* t = &app->theme;
    bar(app, r, app->message[0] ? app->message : about,
        rgb(app->message[0] ? (app->abnormal_exit ? t->failure : t->focus) : t->ink), keys);
}

/* the menu's list in boxes: "Recent games" over the recent rows, then the items */
enum { GUI_ENTRY_LABEL, GUI_ENTRY_RECENT, GUI_ENTRY_ITEM };
typedef struct {
    int kind, index;
} GuiEntry;
#define GUI_ENTRIES_MAX (GUI_RECENT_MAX + 2 + GUI_ITEM_COUNT)
typedef struct {
    GuiPlace place;
    GuiList list;
    GuiEntry entry[GUI_ENTRIES_MAX];
} GuiMenuBoxes;
static void entry_label(const App* app, GuiEntry e, char* label, size_t size) {
    if (e.kind == GUI_ENTRY_LABEL)
        snprintf(label, size, "Recent games");
    else if (e.kind == GUI_ENTRY_RECENT)
        recent_label(app, e.index, label, size);
    else
        menu_label(app, e.index, label, size);
}
/* what the focused entry is about, for the bar and the side box */
static const char* entry_about(const App* app, GuiEntry e) {
    static const char* const loads[] = {"", "Recompiles a game's .nsp, .xci or .nro, then runs it",
                                        "Runs an ARM64 Linux program on the CPU", "Opens a 3DS .cia"};
    if (e.kind == GUI_ENTRY_RECENT)
        return e.index < recent_shown(app) ? app->recent.game[e.index].path : "The recent games not shown";
    if (e.kind != GUI_ENTRY_ITEM) return "";
    if (e.index >= GUI_SWITCH && e.index <= GUI_3DS) return loads[e.index];
    if (e.index == GUI_KEYS) return "Installs prod.keys, which games are extracted with";
    if (e.index == GUI_EXIT) return "Closes ScarletNX";
    {
        GuiMenu menu = app->menu;
        menu.selected = e.index;
        return gui_setting_description(&menu);
    }
}
static void menu_boxes(App* app, GuiMenuBoxes* m) {
    int count = 0, focus = 0, i, longest = 0;
    char label[160], keys[256];
    if (app->recent.count) m->entry[count++] = (GuiEntry){GUI_ENTRY_LABEL, 0};
    for (i = 0; i < app->menu.recent; ++i) {
        if (app->menu.recent_focus == i + 1) focus = count;
        m->entry[count++] = (GuiEntry){GUI_ENTRY_RECENT, i};
    }
    for (i = 0; i < GUI_ITEM_COUNT; ++i) {
        if (i == GUI_GAMES && !app->menu.games) continue;
        if (!app->menu.recent_focus && app->menu.selected == i) focus = count;
        m->entry[count++] = (GuiEntry){GUI_ENTRY_ITEM, i};
    }
    for (i = 0; i < count; ++i) {
        entry_label(app, m->entry[i], label, sizeof label);
        longest = SDL_max(longest, (int)SDL_utf8strlen(label));
    }
    place_screen(app, &m->place, boxed_title(app, menu_screen(app)), screen_keys(app, SAY_MENU_KEYS, keys, sizeof keys),
                 longest + 2 + (int)SDL_utf8strlen(app->theme.cursor), GUI_SHOWS_STATUS | GUI_SHOWS_TABS);
    list_place(app, &m->list, m->place.list, count, focus);
}
static GuiEntry menu_focus(const GuiMenuBoxes* m) { return m->entry[m->list.focus]; }
/* the side box: the focused entry, what it is about, and for Games its folders, for the keys the ones in use; the last
   run's output under it */
static void menu_side(App* app, const GuiMenuBoxes* m) {
    GuiEntry e = menu_focus(m);
    GuiRect r = inner(app, m->place.side);
    char label[160], line[1100];
    const char* about = entry_about(app, e);
    SDL_Color ink = rgb(app->theme.ink), soft = rgb(app->theme.soft);
    int y, i, x;
    if (!m->place.side.h) return;
    entry_label(app, e, label, sizeof label);
    y = pane(app, m->place.side, label + (e.kind != GUI_ENTRY_LABEL ? strlen(app->theme.marker) : 0));
    r = box_rect(r.x, y, r.w, m->place.side.y + m->place.side.h - y);
    y += wrap_in(app, r, r.x, y, about, e.kind == GUI_ENTRY_RECENT, ink) * cell_h(app);
    if (e.kind == GUI_ENTRY_RECENT && e.index < recent_shown(app)) {
        const GuiRecentGame* g = &app->recent.game[e.index];
        static const char* const kinds[] = {"", "Switch", "Linux ARM", "3DS"};
        snprintf(line, sizeof line, "%s, %s", kinds[g->driver >= GUI_SWITCH && g->driver <= GUI_3DS ? g->driver : 0],
                 g->prepared ? "ready to run" : "prepared when it runs");
        y += wrap_in(app, r, r.x, y, line, 0, soft) * cell_h(app);
    } else if (e.kind == GUI_ENTRY_ITEM && e.index == GUI_GAMES) {
        y += wrap_in(app, r, r.x, y, app->games_dir ? app->games_dir : "games/", 1, soft) * cell_h(app);
        for (i = 0; i < app->folder_count; ++i) y += wrap_in(app, r, r.x, y, app->folders[i], 1, soft) * cell_h(app);
    } else if (e.kind == GUI_ENTRY_ITEM && e.index == GUI_KEYS) {
        char* in_use = keys_in_use(app);
        snprintf(line, sizeof line, "In use: %s", in_use ? in_use : "none");
        y += wrap_in(app, r, r.x, y, line, 1, in_use ? soft : rgb(app->theme.failure)) * cell_h(app);
        free(in_use);
    }
    if (!app->has_output || entering(app)) return;
    y += cell_h(app);
    y += wrap_in(app, r, r.x, y, "Last output (no scrollback):", 0, soft) * cell_h(app);
    SDL_SetTextureColorMod(app->font, app->theme.ink.r, app->theme.ink.g, app->theme.ink.b);
    for (i = 0; i < app->console.rows && y + cell_h(app) <= r.y + r.h; ++i, y += cell_h(app))
        for (x = 0; x < app->console.cols && (x + 1) * cell_w(app) <= r.w; ++x)
            if (app->console.cells[i][x] != ' ')
                glyph(app, (float)(r.x + x * cell_w(app)), (float)y, app->console.cells[i][x]);
}
static void draw_menu_boxed(App* app) {
    GuiMenuBoxes m;
    GuiEntry focus;
    char label[160], keys[256];
    int i;
    menu_boxes(app, &m);
    focus = menu_focus(&m);
    chrome(app, &m.place);
    title_box(app, m.place.title, boxed_title(app, menu_screen(app)));
    for (i = m.list.first; i < m.list.first + m.list.shown; ++i) {
        GuiEntry e = m.entry[i];
        entry_label(app, e, label, sizeof label);
        if (e.kind == GUI_ENTRY_LABEL)
            list_label(app, &m.list, i, label);
        else
            list_item(app, &m.list, i, label, i == m.list.focus,
                      e.kind == GUI_ENTRY_ITEM && app->menu.activated && app->menu.active == e.index);
    }
    list_marks(app, &m.list);
    menu_side(app, &m);
    status_box(app, m.place.status);
    bar_for(app, m.place.bar, entry_about(app, focus), screen_keys(app, SAY_MENU_KEYS, keys, sizeof keys));
}
/* the screen darkened under a box over it, to the window's edges as the fades */
static void darken(App* app) {
    static const GuiRgb black = {0, 0, 0};
    SDL_Rect safe;
    int width, height;
    SDL_GetRenderViewport(app->renderer, &safe);
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    SDL_SetRenderViewport(app->renderer, NULL);
    fill(app, 0, 0, (float)width, (float)height, black, 128);
    SDL_SetRenderViewport(app->renderer, &safe);
}
/* a box over the screen, cols columns of text wide and h pixels high, in its middle */
static GuiRect over_rect(const App* app, int cols, int h) {
    int width, height, w;
    area(app, &width, &height);
    w = SDL_min((cols + 2) * cell_w(app), width - 2 * cell_w(app));
    return box_rect((width - w) / 2, SDL_max(0, (height - h) / 2), w, h);
}
/* the box over the darkened screen, on the page colour whatever the theme's boxes, so nothing shows through it */
static int over_box(App* app, GuiRect r, const char* label) {
    darken(app);
    fill_rect(app, r, app->theme.page);
    return pane(app, r, label);
}
/* the question over a run or the menu: the question, its two choices as boxes, the one kept focused */
static void ask_boxes(App* app, GuiRect* r, GuiList* choices) {
    int s = app->scale, item = item_h(app);
    *r = over_rect(app, SDL_max((int)SDL_utf8strlen(ask_question(app)), 16), cell_h(app) + 2 * (item + 6 * s) + 12 * s);
    list_place(app, choices,
               box_rect(r->x + cell_w(app), r->y + cell_h(app) + 10 * s, r->w - 2 * cell_w(app), 2 * (item + 6 * s)), 2,
               app->confirm.selected);
}
static void ask_boxed(App* app) {
    static const char* const choices[2][2] = {{"Keep going", "Abort"}, {"Stay", "Exit"}}; /* by exit, GUI_CONFIRM_* */
    GuiRect r;
    GuiList l;
    char label[64];
    int i;
    ask_boxes(app, &r, &l);
    over_box(app, r, NULL);
    text_in(app, inner(app, r), r.x + cell_w(app), r.y + 4 * app->scale, ask_question(app), rgb(app->theme.warning));
    for (i = 0; i < 2; ++i) {
        snprintf(label, sizeof label, "%s%s", app->theme.marker, choices[app->confirm.exit][i]);
        list_item(app, &l, i, label, app->confirm.selected == i, false);
    }
}
/* the files an older version left, over the menu: each where it is (-) and where this one would keep it (->), as many
   as fit, how many more, and OK, focused: any key closes it */
static void left_boxed(App* app) {
    char old[4200], now[4200], label[64];
    const char* at;
    int width, height, cols, room, used = 0, shown, i, y, s = app->scale, item = item_h(app), more;
    GuiRect r, text;
    GuiList ok;
    SDL_Color failure = rgb(app->theme.failure), ink = rgb(app->theme.ink);
    area(app, &width, &height);
    cols = width / cell_w(app) - 6;
    room = (height - head_h(app) - item - 24 * s) / cell_h(app) - 2; /* the files' rows, and one for how many more */
    for (shown = 0, at = app->left; shown < app->lefts; ++shown, at = strchr(at, '\n') + 1) {
        int need;
        left_paths(at, old, now, sizeof old);
        need = wrap_rows(old, cols - 2, 1) + wrap_rows(now, cols - 3, 1);
        if (shown && used + need > room) break;
        used += need;
    }
    more = app->lefts - shown;
    r = over_rect(app, cols, head_h(app) + (used + (more > 0)) * cell_h(app) + item + 16 * s);
    y = over_box(app, r, "An older version left these files: move each from - to ->") + 2 * s;
    text = inner(app, r);
    for (i = 0, at = app->left; i < shown; ++i, at = strchr(at, '\n') + 1) {
        left_paths(at, old, now, sizeof old);
        text_in(app, text, text.x, y, "-", failure);
        y += wrap_in(app, text, text.x + 2 * cell_w(app), y, old, 1, failure) * cell_h(app);
        text_in(app, text, text.x, y, "->", ink);
        y += wrap_in(app, text, text.x + 3 * cell_w(app), y, now, 1, ink) * cell_h(app);
    }
    if (more > 0) {
        snprintf(label, sizeof label, "and %d more, which Share log lists", more);
        text_in(app, text, text.x, y, label, rgb(app->theme.soft));
    }
    list_place(app, &ok, box_rect(text.x, r.y + r.h - item - 6 * s, SDL_min(text.w, 26 * cell_w(app)), item), 1, 0);
    snprintf(label, sizeof label, "%sOK, nothing is moved", app->theme.marker);
    list_item(app, &ok, 0, label, true, false);
}
/* the games' screen in boxes: Back, then a page of the games, as many as the list's box holds */
typedef struct {
    GuiPlace place;
    GuiList list;
    size_t start; /* the page's first game */
} GuiGamesBoxes;
static void games_place(App* app, GuiPlace* p) {
    size_t i;
    int longest = 10;
    char keys[256];
    for (i = 0; i < app->games.count; ++i) longest = SDL_max(longest, (int)SDL_utf8strlen(app->games.names[i]));
    place_screen(app, p, boxed_title(app, GUI_SCREEN_GAMES), screen_keys(app, SAY_GAMES, keys, sizeof keys),
                 longest + 2 + (int)SDL_utf8strlen(app->theme.marker) + (int)SDL_utf8strlen(app->theme.cursor),
                 GUI_SHOWS_STATUS | GUI_SHOWS_TABS);
}
/* the games a page of the boxed list holds: those that fit under Back */
static size_t games_page(App* app) {
    GuiPlace p;
    GuiList l;
    games_place(app, &p);
    list_place(app, &l, p.list, 1 << 16, -1);
    return (size_t)SDL_max(1, l.shown - 1);
}
static void games_boxes(App* app, GuiGamesBoxes* g) {
    games_place(app, &g->place);
    g->start = app->browser.page * app->browser.page_size;
    list_place(app, &g->list, g->place.list, 1 + (int)gui_browser_items(&app->browser), (int)app->browser.selected);
}
static void draw_games_boxed(App* app) {
    GuiGamesBoxes g;
    GuiRect r;
    char label[400], line[1100], keys[256];
    const char* about;
    size_t i, items = gui_browser_items(&app->browser);
    SDL_Color soft = rgb(app->theme.soft), ink = rgb(app->theme.ink);
    int y;
    games_boxes(app, &g);
    chrome(app, &g.place);
    title_box(app, g.place.title, boxed_title(app, GUI_SCREEN_GAMES));
    list_item(app, &g.list, 0, "Back [ESC]", app->browser.selected == 0, false);
    for (i = 0; i < items; ++i) {
        snprintf(label, sizeof label, "%s%s", app->theme.marker, app->games.names[g.start + i]);
        list_item(app, &g.list, 1 + (int)i, label, app->browser.selected == i + 1,
                  app->browser.activated && app->browser.active == g.start + i);
    }
    /* beside it the focused game: where it is and what runs it; the page, and where the games are looked for */
    r = inner(app, g.place.side);
    if (g.place.side.h) {
        const char* name = app->browser.selected ? app->games.names[g.start + app->browser.selected - 1] : "Back";
        y = pane(app, g.place.side, name);
        r = box_rect(r.x, y, r.w, g.place.side.y + g.place.side.h - y);
        if (app->browser.selected) {
            y += wrap_in(app, r, r.x, y, app->games.paths[g.start + app->browser.selected - 1], 1, ink) * cell_h(app);
            y += wrap_in(app, r, r.x, y,
                         gui_is_cia(name)    ? "3DS CIA, opened with scarletnx-3ds"
                         : gui_is_dump(name) ? "A dump, prepared when it runs"
                                             : "Ready to run",
                         0, soft) *
                 cell_h(app);
        } else
            y += wrap_in(app, r, r.x, y, "Back to the menu", 0, ink) * cell_h(app);
        y += cell_h(app);
        if (!app->games.count)
            y += wrap_in(app, r, r.x, y, SAY_NO_GAMES, 0, soft) * cell_h(app);
        else {
            snprintf(line, sizeof line, "Page %zu/%zu, %zu " SAY_COUNT, app->browser.page + 1,
                     gui_browser_pages(&app->browser), app->games.count);
            y += wrap_in(app, r, r.x, y, line, 0, soft) * cell_h(app);
        }
        y += wrap_in(app, r, r.x, y, app->games_dir ? app->games_dir : "games/", 1, soft) * cell_h(app);
        for (i = 0; i < (size_t)app->folder_count; ++i)
            y += wrap_in(app, r, r.x, y, app->folders[i], 1, soft) * cell_h(app);
    }
    list_marks(app, &g.list);
    status_box(app, g.place.status);
    about = app->browser.selected ? app->games.paths[g.start + app->browser.selected - 1] : "Back to the menu";
    bar_for(app, g.place.bar, about, screen_keys(app, SAY_GAMES, keys, sizeof keys));
}
/* a line that waits on the user, at pixel row y of r: plain at its left, or framed, placed and blinking as the theme's
   prompt says */
static void prompt_in(App* app, GuiRect r, int y, GuiPromptLine line, const char* value, SDL_Color color) {
    char framed[160];
    const GuiPrompt* p = &app->theme.prompt;
    int x = r.x, w;
    if (p->lines[line]) {
        if (p->blink && app->theme.blink_ms && (now_ms(app) / app->theme.blink_ms) % 2) return;
        snprintf(framed, sizeof framed, "%s%s%s", p->open, value, p->close);
        value = framed;
        w = (int)SDL_utf8strlen(framed) * cell_w(app);
        if (p->align == GUI_ALIGN_CENTER) x = r.x + (r.w - w) / 2;
        if (p->align == GUI_ALIGN_RIGHT) x = r.x + r.w - w;
    }
    text_in(app, r, SDL_max(r.x, x), y, value, color);
}
/* the screen a file is picked or dropped on in boxes: one box with where the keys go and the ones in use, the line that
   waits in its middle, Back at its foot */
typedef struct {
    GuiPlace place;
    GuiList back;
    int prompt; /* the waiting line's pixel row */
} GuiPickBoxes;
static void pick_boxes(App* app, GuiPickBoxes* k) {
    GuiRect r;
    int item = item_h(app);
    char keys[256];
    place_screen(app, &k->place, boxed_title(app, menu_screen(app)), screen_keys(app, SAY_KEYS_HOW, keys, sizeof keys),
                 0, GUI_SHOWS_TABS);
    r = inner(app, k->place.list);
    list_place(
        app, &k->back,
        box_rect(r.x, k->place.list.y + k->place.list.h - item - 4 * app->scale, SDL_min(r.w, 24 * cell_w(app)), item),
        1, -1);
    k->prompt = k->place.list.y + k->place.list.h / 2;
}
static void draw_pick_boxed(App* app) {
    GuiPickBoxes k;
    GuiRect r;
    bool keys = app->menu.view == GUI_LOAD_KEYS;
    char line[1100], hints_line[256], *in_use = keys ? keys_in_use(app) : NULL;
    int y;
    pick_boxes(app, &k);
    chrome(app, &k.place);
    title_box(app, k.place.title, boxed_title(app, menu_screen(app)));
    y = pane(app, k.place.list, items[keys ? GUI_KEYS : app->menu.active]);
    r = inner(app, k.place.list);
    r = box_rect(r.x, y, r.w, k.back.at.y - y);
    if (keys) {
        snprintf(line, sizeof line, "Installs to: %s", app->keys_path ? app->keys_path : "Home directory unavailable.");
        y += wrap_in(app, r, r.x, y, line, 1, rgb(app->theme.ink)) * cell_h(app);
    }
    if (in_use && app->keys_path && !strcmp(in_use, app->keys_path))
        y += wrap_in(app, r, r.x, y, "Keys already setup. This will overwrite the previous ones", 0,
                     rgb(app->theme.failure)) *
             cell_h(app);
    else if (in_use) {
        snprintf(line, sizeof line, "Keys in use: %s. Keys dropped here are read before them", in_use);
        y += wrap_in(app, r, r.x, y, line, 1, rgb(app->theme.soft)) * cell_h(app);
    }
    free(in_use);
    prompt_in(app, r, SDL_max(y, k.prompt), GUI_PROMPT_KEYS, keys ? SAY_KEYS_WAIT : SAY_FILE_WAIT,
              rgb(app->theme.focus));
    list_item(app, &k.back, 0, "Back [ESC]", false, false); /* nothing to move to: ESC's */
    snprintf(line, sizeof line, "%s: " SAY_PICK_BOX, items[keys ? GUI_KEYS : app->menu.active]);
    bar_for(app, k.place.bar, line, screen_keys(app, SAY_KEYS_HOW, hints_line, sizeof hints_line));
}
/* a run of the console layout in boxes: the driver's output in the wide box, beside it what runs, its file and how it
   ended, the status under them; the ESC line in the bar, as the theme's prompt frames it */
static void run_place(App* app, GuiPlace* p) {
    int width, height;
    area(app, &width, &height);
    place_screen(app, p, boxed_title(app, screen(app)), "", width / cell_w(app), GUI_SHOWS_STATUS);
}
/* the output box's cells under its head */
static void run_console(App* app, const GuiPlace* p, int* cols, int* rows) {
    GuiRect r = inner(app, p->list);
    *cols = SDL_max(1, r.w / cell_w(app));
    *rows = SDL_max(1, (p->list.h - head_h(app) - 4 * app->scale) / cell_h(app));
}
static void draw_run_boxed(App* app) {
    GuiPlace p;
    GuiRect r, text;
    const GuiTheme* t = &app->theme;
    int y, x, i, s = app->scale, cols, rows;
    GuiPromptLine line =
        app->finished ? (app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE) : GUI_PROMPT_ABORTING;
    run_place(app, &p);
    chrome(app, &p);
    title_box(app, p.title, boxed_title(app, screen(app)));
    y = pane(app, p.list, "Output");
    r = inner(app, p.list);
    run_console(app, &p, &cols, &rows);
    SDL_SetTextureColorMod(app->font, t->ink.r, t->ink.g, t->ink.b);
    for (i = 0; i < SDL_min(rows, app->console.rows); ++i)
        for (x = 0; x < SDL_min(cols, app->console.cols); ++x)
            if (app->console.cells[i][x] != ' ')
                glyph(app, (float)(r.x + x * cell_w(app)), (float)(y + i * cell_h(app)), app->console.cells[i][x]);
    y = pane(app, p.side, items[app->menu.selected]);
    text = inner(app, p.side);
    text = box_rect(text.x, y, text.w, p.side.y + p.side.h - y);
    y += wrap_in(app, text, text.x, y, app->file ? app->file : "", 1, rgb(t->ink)) * cell_h(app);
    if (app->finished)
        wrap_in(app, text, text.x, y + cell_h(app) / 2, app->message, 0,
                rgb(app->abnormal_exit ? t->failure : t->focus));
    else if (app->aborting)
        wrap_in(app, text, text.x, y + cell_h(app) / 2, "Aborting process tree...", 0, rgb(t->failure));
    status_box(app, p.status);
    bar(app, p.bar, app->file ? app->file : "", rgb(t->soft), "");
    text = box_rect(p.bar.x + 3 * cell_w(app), p.bar.y, p.bar.w - 4 * cell_w(app), p.bar.h);
    if (app->finished || app->aborting)
        prompt_in(app, text, p.bar.y + 2 * s + cell_h(app), line, app->finished ? SAY_DONE : "Aborting process tree...",
                  rgb(t->failure));
    else
        text_in(app, text, text.x + text.w - (int)strlen(SAY_ABORT) * cell_w(app), p.bar.y + 2 * s + cell_h(app),
                SAY_ABORT, rgb(t->failure));
}
/* the menu's screen or a run's, in app->theme, on the render target as it is */
static void draw_screen(App* app) {
    SDL_Color ink, soft, focus, paper, active, failure;
    const char* title;
    int head, foot, idle;
    const GuiTheme* theme;
    theme = &app->theme;
    idle = (!app->menu.running && !app->finished) || entering(app);
    head = theme->head != GUI_STYLE_NONE;
    foot = theme->footer != GUI_STYLE_NONE;
    ink = rgb(theme->ink);
    soft = rgb(theme->soft);
    focus = rgb(theme->focus);
    paper = rgb(theme->page);
    active = rgb(theme->active);
    failure = rgb(theme->failure);
    SDL_SetRenderDrawColor(app->renderer, theme->page.r, theme->page.g, theme->page.b, 255);
    SDL_RenderClear(app->renderer);
    if (!idle && theme->run == GUI_RUN_STEPS) {
        GuiRgb page = theme->run_colors.page;
        SDL_SetRenderDrawColor(app->renderer, page.r, page.g, page.b, 255);
        SDL_RenderClear(app->renderer);
        render_steps(app);
        render_ask(app);
        overlay(app);
        return;
    }
    if (boxed(app) && !idle) {
        draw_run_boxed(app);
        render_ask(app);
        overlay(app);
        return;
    }
    if (boxed(app) && idle) {
        if (app->menu.view == GUI_LOAD_GAMES)
            draw_games_boxed(app);
        else if (pick_screen(app))
            draw_pick_boxed(app);
        else
            draw_menu_boxed(app);
        render_ask(app);
        render_left(app);
        overlay(app);
        return;
    }
    title = theme->title[idle ? menu_screen(app) : screen(app)];
    if (head && title[0]) {
        if (theme->head == GUI_STYLE_BAR) band(app, 0, 1, theme->ink);
        text(app, 1, 0, title, theme->head == GUI_STYLE_BAR ? paper : ink);
    }
    if (head && theme->head_rule) rule(app, 4, theme->soft);
    if (app->menu.view == GUI_LOAD_GAMES && idle) {
        size_t i;
        if (head) {
            char where[1100];
            text(app, 1, 1, SAY_GAMES, ink);
            if (app->folder_count)
                snprintf(where, sizeof where, "%s and %d folder%s of Settings > App", app->games_dir, app->folder_count,
                         app->folder_count == 1 ? "" : "s");
            else
                snprintf(where, sizeof where, "%s", app->games_dir);
            text(app, 1, 2, where, soft);
        }
        entry(app, 1, 3, "Back [ESC]", false, app->browser.selected == 0);
        size_t start = app->browser.page * app->browser.page_size;
        size_t count = gui_browser_items(&app->browser);
        for (i = 0; i < count; ++i)
            entry(app, 1, 5 + (int)i, app->games.names[start + i],
                  app->browser.activated && app->browser.active == start + i, app->browser.selected == i + 1);
        if (count)
            more_marks(app, 5, 5 + (int)count - 1, app->browser.page > 0,
                       app->browser.page + 1 < gui_browser_pages(&app->browser));
        if (!app->games.count) text(app, 1, 5, SAY_NO_GAMES, soft);
        char page[128];
        snprintf(page, sizeof(page), "Page %zu/%zu   %zu " SAY_COUNT, app->browser.page + 1,
                 gui_browser_pages(&app->browser), app->games.count);
        text(app, 1, app->rows - 5, page, soft);
        text(app, 1, app->rows - 4, app->message, focus);
    } else if (pick_screen(app) && idle) {
        bool keys = app->menu.view == GUI_LOAD_KEYS;
        if (head && keys) {
            text(app, 1, 1, SAY_KEYS, ink);
            text(app, 1, 3, app->keys_path ? app->keys_path : "Home directory unavailable.", ink);
        } else if (head) {
            char line[128];
            snprintf(line, sizeof line, "%s: " SAY_PICK, items[app->menu.active]);
            text(app, 1, 1, line, ink);
        }
        if (head) text(app, 1, 2, SAY_KEYS_HOW, soft);
        char* in_use = keys ? keys_in_use(app) : NULL;
        if (in_use && app->keys_path && !strcmp(in_use, app->keys_path))
            text(app, 1, 4, "Keys already setup. This will overwrite the previous ones", failure);
        else if (in_use) {
            char line[1100];
            snprintf(line, sizeof(line), "Keys in use: %s. Keys dropped here are read before them", in_use);
            text(app, 1, 4, line, soft);
        }
        free(in_use);
        text(app, 1, 6, app->message, focus);
        entry(app, 1, 8, "Back [ESC]", false, app->browser.selected == 0);
        prompt(app, GUI_PROMPT_KEYS, 1, SDL_max(app->rows / 2, 10), keys ? SAY_KEYS_WAIT : SAY_FILE_WAIT, focus);
    } else if (idle) {
        if (head) {
            text(app, 1, 1, SAY_MENU, ink);
            text(app, 1, 2, app->menu.view == GUI_LOAD_DROP ? SAY_DROP : SAY_LOAD, soft);
        }
        if (app->recent.count && menu_shows(app, recent_row(app, -1)))
            text(app, 1, recent_row(app, -1), "Recent games", soft);
        {
            int game;
            char label[160];
            for (game = 0; game < app->menu.recent; ++game) {
                if (!menu_shows(app, recent_row(app, game))) continue;
                recent_label(app, game, label, sizeof label);
                entry(app, 1, recent_row(app, game), label, false, app->menu.recent_focus == game + 1);
            }
        }
        for (int i = 0; i < GUI_ITEM_COUNT; ++i) {
            bool is_active = app->menu.activated && app->menu.active == i;
            bool is_focused = app->menu.selected == i && !app->menu.recent_focus;
            int row = menu_row(app, i);
            char label[100];
            if (i == GUI_GAMES && !app->menu.games) continue;
            menu_label(app, i, label, sizeof(label));
            if (menu_shows(app, row)) entry(app, 1, row, label, is_active, is_focused);
        }
        more_marks(app, 4, menu_last_row(app), menu_offset(app) > 0,
                   menu_row(app, GUI_ITEM_COUNT - 1) > menu_last_row(app));
        /* with recent games a small window has no room left for these: they would run into the footer */
        if (app->log_top - 4 < app->rows - 4)
            text(app, 1, app->log_top - 4, app->message, app->abnormal_exit ? failure : focus);
        if (app->has_output && !entering(app) && app->log_top - 2 < app->rows - 4)
            text(app, 1, app->log_top - 2, "Last output (no scrollback):", soft);
    } else if (head) {
        text(app, 1, 1, app->finished ? app->message : items[app->menu.selected],
             app->abnormal_exit ? failure
             : app->finished    ? focus
                                : active);
        text(app, 1, 2, app->file ? app->file : "", ink);
        if (app->finished)
            prompt(app, app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, 3, SAY_DONE, failure);
        else if (app->aborting)
            prompt(app, GUI_PROMPT_ABORTING, 1, 3, "Aborting process tree...", failure);
        else
            text(app, 1, 3, SAY_ABORT, failure);
    }
    if (app->has_output && app->menu.view != GUI_LOAD_GAMES && !pick_screen(app) && !entering(app)) {
        SDL_SetTextureColorMod(app->font, theme->ink.r, theme->ink.g, theme->ink.b);
        for (int y = 0; y < app->console.rows; ++y)
            for (int x = 0; x < app->console.cols; ++x)
                if (app->log_top + y < app->rows - 4 && app->console.cells[y][x] != ' ')
                    glyph(app, (float)((x + 1) * 8 * app->scale), (float)((app->log_top + y) * 16 * app->scale),
                          app->console.cells[y][x]);
    }
    if (idle && !pick_screen(app) && (app->menu.view == GUI_LOAD_NONE || app->menu.view == GUI_LOAD_DROP)) {
        char more[64];
        snprintf(more, sizeof more, "The other %d recent games", app->recent.count - recent_shown(app));
        text(app, 1, app->rows - 4,
             !app->menu.recent_focus                       ? gui_setting_description(&app->menu)
             : app->menu.recent_focus <= recent_shown(app) ? app->recent.game[app->menu.recent_focus - 1].path
                                                           : more,
             ink);
    }
    if (foot) footer(app, app->rows - 3, 3, theme->page, theme->ink, theme->soft);
    render_ask(app);
    render_left(app);
    overlay(app);
}

static const char* driver_name(int item) {
    switch (item) {
#ifdef SDL_PLATFORM_ANDROID
        /* the app's libraries folder only takes lib*.so: the drivers go in as ones */
        case GUI_SWITCH: return "libscarletnx-run.so";
        case GUI_LINUX: return "libscarletnx-gnuabi.so";
        case GUI_3DS: return "libscarletnx-3ds.so";
#else
        case GUI_SWITCH: return "scarletnx";
        case GUI_LINUX: return "scarletnx-gnuabi";
        case GUI_3DS: return "scarletnx-3ds";
#endif
        default: return NULL;
    }
}
static char* find_driver(const App* app, int item) {
    const char* name = driver_name(item);
    if (app->self && item == GUI_SWITCH) return snx_strdup(app->self);
    if (!name) return NULL;
#ifdef _WIN32
    char* filename = snx_format("%s.exe", name);
#else
    char* filename = snx_strdup(name);
#endif
    char* local = snx_path_join(app->drivers, filename);
    char* found = snx_find_tool(local);
    free(filename);
    free(local);
    return found ? found : snx_find_tool(name);
}
/* what argv prints, NULL unless it ends with 0, killed after ten seconds; owned */
static char* capture(App* app, const char* const* argv) {
    char buffer[4096], *out = snx_strdup("");
    size_t size = 0;
    int status = 1, n;
    Uint64 until = SDL_GetTicks() + 10000;
    if (!gui_process_start(app->process, argv, app->message, sizeof(app->message))) status = -1;
    while (status >= 0 && !gui_process_finished(app->process, &status) && SDL_GetTicks() < until + 5000) {
        char* grown;
        n = gui_process_read(app->process, buffer, sizeof buffer);
        if (n > 0 && (grown = realloc(out, size + (size_t)n + 1)) != NULL) {
            out = grown;
            memcpy(out + size, buffer, (size_t)n);
            out[size += (size_t)n] = 0;
        } else if (n <= 0)
            SDL_Delay(1);
        if (SDL_GetTicks() > until) gui_process_terminate(app->process);
    }
    if (status) {
        free(out);
        return NULL;
    }
    return out;
}
/* the Settings' rows: the runtime's list, read the first time from the program that runs Switch inputs, after the
   launcher's own */
static GuiSettings* settings_of(App* app) {
    char* driver;
    char* list;
    if (app->settings.keys) return &app->settings;
    driver = find_driver(app, GUI_SWITCH);
    {
        const char* argv[] = {driver, "--settings", NULL};
        list = driver ? capture(app, argv) : NULL;
    }
    gui_settings_read(&app->settings, list ? list : "");
    free(list);
    free(driver);
    /* the launcher's own choices win over the game's .ini, as its items did: a phone plays on the whole screen, its
       pad shown */
#ifdef SDL_PLATFORM_ANDROID
    gui_settings_set(&app->settings, "fullscreen", "1");
    gui_settings_set(&app->settings, "controls", "1");
#else
    gui_settings_set(&app->settings, "fullscreen", "0");
    gui_settings_set(&app->settings, "controls", "0");
#endif
    gui_settings_mark(&app->settings);
    gui_settings_load(&app->settings, app->settings_file);
    return &app->settings;
}
/* ---- the editor's forms (editor.c): the Settings, or the theme editor's rows on the left and the menu or a run in
   its theme on the right ---- */
#define GUI_PREVIEW_MS 8000
static const char preview_run[] = "Extracting Cult of the Lamb.nsp\n"
                                  "Extracted 3 NCAs, the program's 4 modules\n"
                                  "Recompiling Cult of the Lamb (4 modules)\n"
                                  "Functions: 48211  instructions: 2915002\n"
                                  "LLVM: 5 code units and the module descriptor\n"
                                  "  compiled block 1/6: code_000, 9874 functions (8.1 s)\n"
                                  "  compiled block 2/6: code_001, 10022 functions (8.4 s)\n"
                                  "  compiled block 3/6: code_002, 9567 functions (7.9 s)\n";
/* the preview's cells: the right half, its page 640 x 400 (80 x 25 at 1x) kept 1.6 times as wide as high */
static void preview_box(const App* app, int* col, int* row, int* cols, int* rows) {
    *cols = SDL_max(16, app->cols / 2 - 2);
    *rows = *cols * 5 / 16;
    *col = app->cols - *cols;
    *row = 4;
}
/* the menu or a run drawn in the theme being edited, at 1x, in app->preview; false when it cannot be made */
static bool preview_texture(App* app) {
    static App saved; /* the whole app, put back after: drawing the preview borrows it */
    int run = app->editor.preview != GUI_SCREEN_MENU;
    Uint64 now = now_ms(app);
    if (!app->preview) {
        app->preview = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 400);
        if (!app->preview) return false;
    }
    if (!app->preview_since || now - app->preview_since >= GUI_PREVIEW_MS) {
        app->preview_since = SDL_max(now, 1);
        gui_steps_reset(&app->preview_steps);
        gui_steps_feed(&app->preview_steps, preview_run, sizeof preview_run - 1, app->preview_since);
        gui_console_init(&app->preview_console, 78, 17);
        gui_console_feed(&app->preview_console, preview_run, sizeof preview_run - 1);
    }
    saved = *app;
    app->theme = app->editor.theme;
    app->scale = 1;
    app->cols = 80;
    app->rows = 25;
    app->menu.view = GUI_LOAD_NONE;
    app->menu.running = run;
    app->editing = false; /* the menu's own tabs */
    app->finished = app->aborting = app->abnormal_exit = app->confirm.shown = false;
    app->native = 0;
    app->entering_ms = app->leaving_ms = 0;
    app->fade_ms = run ? app->preview_since : 0; /* a run fades in from black, as after the menu */
    app->message[0] = 0;
    app->has_output = run;
    if (run) {
        app->steps = app->preview_steps;
        app->console = app->preview_console;
        app->file = (char*)"games/Cult of the Lamb.nsp";
        app->menu.selected = GUI_SWITCH;
    }
    app->log_top = run ? 4 : menu_row(app, GUI_EXIT) + 6;
    SDL_SetRenderTarget(app->renderer, app->preview);
    SDL_SetRenderViewport(app->renderer, NULL);
    draw_screen(app);
    SDL_SetRenderTarget(app->renderer, NULL);
    *app = saved;
    set_area(app);
    return true;
}
static void render_preview(App* app) {
    SDL_FRect box;
    int col, row, cols, rows, run = app->editor.preview != GUI_SCREEN_MENU;
    preview_box(app, &col, &row, &cols, &rows);
    if (!preview_texture(app)) return;
    box.x = (float)(col * 8 * app->scale);
    box.y = (float)(row * 16 * app->scale);
    box.w = (float)(cols * 8 * app->scale);
    box.h = (float)(rows * 16 * app->scale);
    SDL_RenderTexture(app->renderer, app->preview, NULL, &box);
    text(app, col, row + rows, run ? "Preview: a run (Ctrl+P: the menu)" : "Preview: the menu (Ctrl+P: a run)",
         rgb(app->theme.soft));
}
/* the editor's rows between rows 4 and the last three, as many as fit */
static int editor_rows(const App* app) { return SDL_max(1, app->rows - 7); }
static void render_editor(App* app) {
    GuiEditor* e = &app->editor;
    const GuiTheme* theme = &app->theme;
    const GuiThemeKey* focused = gui_editor_key_of(e, e->row);
    char label[64], value[96], title[96];
    int r, col, row, cols, rows, width, labels = 24;
    SDL_SetRenderDrawColor(app->renderer, theme->page.r, theme->page.g, theme->page.b, 255);
    SDL_RenderClear(app->renderer);
    preview_box(app, &col, &row, &cols, &rows);
    width = app->form != GUI_FORM_THEME ? app->cols - 2 : col - 2; /* the rows' text, left of the theme's preview */
    for (r = 0; r < e->count; ++r) {                               /* the values' column, past the widest key's label */
        gui_editor_label(e, r + !e->object, label, sizeof label);
        labels = SDL_max(labels, (int)strlen(label));
    }
    if (app->form == GUI_FORM_THEME)
        snprintf(title, sizeof title, "THEME EDITOR  /  %s", e->id);
    else if (app->form == GUI_FORM_SETTINGS)
        snprintf(title, sizeof title, "SETTINGS");
    else if (app->form == GUI_FORM_FOLDERS)
        snprintf(title, sizeof title, "SETTINGS  /  APP  /  GAME FOLDERS");
    else {
        char* c;
        snprintf(title, sizeof title, "SETTINGS  /  %s%s%s", app->group_keys[app->group].key,
                 app->page < 0 ? "" : "  /  ", app->page < 0 ? "" : app->settings.keys[app->page].key);
        for (c = title; *c; ++c) *c = (char)SDL_toupper((unsigned char)*c);
    }
    if (theme->head == GUI_STYLE_BAR) band(app, 0, 1, theme->ink);
    text(app, 1, 0, title, rgb(theme->head == GUI_STYLE_BAR ? theme->page : theme->ink));
    text(app, 1, 1,
         app->form == GUI_FORM_THEME      ? SAY_EDITOR
         : app->form == GUI_FORM_SETTINGS ? SAY_GROUPS
         : app->form == GUI_FORM_APP      ? SAY_APP
         : app->form == GUI_FORM_FOLDERS  ? SAY_FOLDERS
                                          : SAY_SETTINGS,
         rgb(theme->ink));
    if (e->note[0])
        text(app, 1, 2, e->note, rgb(e->leaving ? theme->failure : theme->focus));
    else if (focused)
        text(app, 1, 2, focused->about, rgb(theme->soft));
    gui_editor_scroll(e, editor_rows(app));
    for (r = e->top; r < gui_editor_rows(&app->editor) && r - e->top < editor_rows(app); ++r) {
        const GuiThemeKey* k = gui_editor_key_of(e, r);
        int y = 4 + r - e->top, at = 1 + labels + 1;
        char line[160];
        gui_editor_label(e, r, label, sizeof label);
        if (r == e->row && e->typing)
            snprintf(value, sizeof value, "%s_", e->text);
        else if (app->form == GUI_FORM_GROUP && k)
            gui_settings_shown(&app->settings, app->first + r, value, sizeof value);
        else
            gui_editor_value(e, r, value, sizeof value);
        snprintf(line, sizeof line, "%s%-*s %s", theme->marker, labels, label, value);
        {
            int n = 0; /* what fits left of the preview, a character at a time */
            char* c;
            for (c = line; *c && n < width; ++c) n += ((unsigned char)*c & 0xC0) != 0x80;
            while (((unsigned char)*c & 0xC0) == 0x80) ++c;
            *c = 0;
        }
        entry(app, 1, y, line, false, r == e->row);
        if (k && k->type == GUI_KEY_COLOUR) {
            const GuiRgb* c = (const GuiRgb*)((const char*)&e->theme + k->at);
            float px = (float)app->scale, x0, y0 = (float)y * 16 * px + 2 * px;
            at += (int)SDL_utf8strlen(theme->marker) + 8;
            x0 = (float)at * 8 * px;
            if (at + 2 <= width) { /* framed in soft, so a page-coloured one shows */
                fill(app, x0, y0, 16 * px, 12 * px, theme->soft, 255);
                fill(app, x0 + px, y0 + px, 14 * px, 10 * px, *c, 255);
            }
        }
    }
    more_marks(app, 4, 4 + SDL_min(editor_rows(app), gui_editor_rows(e) - e->top) - 1, e->top > 0,
               e->top + editor_rows(app) < gui_editor_rows(e));
    if (app->form != GUI_FORM_THEME) {
        if (app->form != GUI_FORM_APP && app->form != GUI_FORM_FOLDERS)
            text(app, 1, app->rows - 2,
                 app->settings.count > GUI_SETTINGS_OWN ? "For the next runs. [default]: as the game's .ini says"
                                                        : "The runtime listed no settings",
                 rgb(theme->soft));
        return;
    }
    snprintf(value, sizeof value, "Saves in %s", app->themes_dirs[0] ? app->themes_dirs[0] : "(no themes folder)");
    text(app, 1, app->rows - 2, value, rgb(theme->soft));
    render_preview(app);
}
static bool menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key);
static void screen_tab(App* app, int step);
static void go_tab(App* app, int tab);
static int tab_hit(App* app, float x, float y);
/* the editor on the theme the menu shows: a folder of themes/ keeps its file's extras and its name, a built-in is
   saved as a new theme */
static void open_editor(App* app) {
    GuiThemeFile file;
    size_t size = 0;
    char* text = gui_theme_text((const char* const*)app->themes_dirs, 2, app->theme.id, &size);
    int from_file = text && gui_theme_file_read(&file, text, size);
    free(text);
    gui_editor_start(&app->editor, &app->theme, from_file ? &file : NULL, from_file ? app->theme.id : "mytheme");
    if (from_file) gui_theme_file_free(&file);
    app->editing = true;
    app->form = GUI_FORM_THEME;
    app->preview_since = 0;
}
static int SDLCALL count_space(void* data) {
    GuiSpace* space = (GuiSpace*)data;
    uint64_t used = 0;
    for (int i = 0; i < GUI_SPACE_DIRS; ++i)
        used += space->dirs[i][0] ? gui_tree_size(space->dirs[i], &space->stop) : 0;
    space->used = used;
    __atomic_store_n(&space->done, 1, __ATOMIC_RELEASE);
    return 0;
}
static void space_stop(App* app) {
    if (app->counter) {
        __atomic_store_n(&app->space->stop, 1, __ATOMIC_RELAXED);
        SDL_WaitThread(app->counter, NULL);
        app->counter = NULL;
    }
    free(app->space);
    app->space = NULL;
}
/* the space left on the disk of path, or of the folder it will be made in */
static uint64_t free_space(const char* path) {
    char* at = snx_strdup(path);
    uint64_t left;
    while (!snx_is_dir(at)) {
        char* up = snx_dirname(at);
        int top = !strcmp(up, at);
        free(at);
        at = up;
        if (top) break;
    }
    left = snx_free_space(at);
    free(at);
    return left;
}
static void space_start(App* app) {
    char* dirs[GUI_SPACE_DIRS] = {snx_strdup(app->games_dir),
                                  snx_path_join(app->data_dir, "work"),
                                  app->themes_dirs[0] ? snx_strdup(app->themes_dirs[0]) : NULL,
#ifdef SNX_XDG /* settings, logs and caches, the menu's files among them; an older version's work/ is the data folder's */
                                  snx_user_dir(SNX_USER_CONFIG),
                                  snx_user_dir(SNX_USER_STATE),
                                  snx_user_dir(SNX_USER_CACHE)
#else
                                  app->recent_file ? snx_dirname(app->recent_file) : NULL,
                                  NULL,
                                  NULL
#endif
    };
    size_t size = sizeof *app->space;
    char* at;
    int i;
    space_stop(app);
    for (i = 0; i < GUI_SPACE_DIRS; ++i) size += (dirs[i] ? strlen(dirs[i]) : 0) + 1;
    app->space = (GuiSpace*)calloc(1, size);
    for (i = 0, at = (char*)(app->space + 1); app->space && i < GUI_SPACE_DIRS; at += strlen(at) + 1, ++i)
        app->space->dirs[i] = strcpy(at, dirs[i] ? dirs[i] : ""); /* "": none */
    for (i = 0; i < GUI_SPACE_DIRS; ++i) free(dirs[i]);
    app->free_bytes = free_space(app->data_dir);
    if (app->space && !(app->counter = SDL_CreateThread(count_space, "space", app->space)))
        count_space(app->space); /* no thread to count it: here, then */
}
/* ---- the Settings: its groups, App then a group per section of the settings; a group's rows; the App's ---- */
static const GuiThemeKey app_keys[] = {
    {"", "Theme", GUI_KEY_ACTION, offsetof(GuiAppForm, theme), sizeof(((GuiAppForm*)0)->theme), NULL, 0,
     "Built-in themes, then each themes/<name>/theme.ini"},
    {"", "Text size", GUI_KEY_ACTION, offsetof(GuiAppForm, text), sizeof(((GuiAppForm*)0)->text), NULL, 0,
     "Bigger letters scroll the menu. Auto fits the screen"},
    {"", "Theme editor", GUI_KEY_ACTION, 0, 0, NULL, 0,
     "This theme's keys with a preview, saved as themes/<name>/theme.ini"},
    {"", "Game folders", GUI_KEY_ACTION, offsetof(GuiAppForm, folders), sizeof(((GuiAppForm*)0)->folders), NULL, 0,
     "Folders of games listed with games/"}};
enum { GUI_APP_THEME, GUI_APP_TEXT, GUI_APP_EDITOR, GUI_APP_FOLDERS, GUI_APP_ROWS };
static void app_values(App* app) {
    GuiAppForm* f = &app->app_form;
    snprintf(f->theme, sizeof f->theme, "%s", app->theme.name);
    if (app->menu.text_size)
        snprintf(f->text, sizeof f->text, "%dx", app->menu.text_size);
    else
        snprintf(f->text, sizeof f->text, "Auto");
    if (app->folder_count)
        snprintf(f->folders, sizeof f->folders, "games/ and %d more", app->folder_count);
    else
        snprintf(f->folders, sizeof f->folders, "games/");
}
/* ---- the player's game folders: listed after games/, kept a line each in folders_file ---- */
static void folders_load(App* app) {
    size_t size = 0;
    char *text = app->folders_file ? (char*)snx_read_file(app->folders_file, &size) : NULL, *line, *next;
    for (line = text; line && *line && app->folder_count < GUI_FOLDERS_MAX; line = next) {
        next = line + strcspn(line, "\n");
        if (*next) *next++ = 0;
        if (*line) app->folders[app->folder_count++] = snx_strdup(line);
    }
    free(text);
}
/* 0, else the note says why not */
static int folders_save(App* app) {
    size_t size = 1, n;
    char *text, *dir;
    int i, failed;
    if (!app->folders_file) return 0;
    for (i = 0; i < app->folder_count; ++i) size += strlen(app->folders[i]) + 1;
    text = (char*)malloc(size);
    for (i = 0, n = 0; text && i < app->folder_count; ++i)
        n += (size_t)snprintf(text + n, size - n, "%s\n", app->folders[i]);
    dir = snx_dirname(app->folders_file);
    failed = !text || !dir || snx_mkdirs(dir) || snx_write_file(app->folders_file, text, n);
    if (failed)
        snprintf(app->editor.note, sizeof app->editor.note, "Not saved: %s could not be written", app->folders_file);
    free(dir);
    free(text);
    return failed;
}
static void open_folders(App* app, int row);
static void count_games(App* app);
/* a folder the player picked, dropped or pasted: a folder that is there, once. What happened goes on the folders'
   screen, or in the menu's message when the picker answered after the player left it */
static void folder_add(App* app, const char* path) {
    char *absolute = path ? snx_absolute(path) : NULL, note[sizeof app->editor.note];
    int i, added = 0, here = app->editing && app->form == GUI_FORM_FOLDERS;
    for (i = 0; absolute && i < app->folder_count && strcmp(app->folders[i], absolute); ++i) {}
    if (!absolute || !snx_is_dir(absolute))
        snprintf(note, sizeof note, "Not a folder: %s", path ? path : "(none)");
    else if (app->games_dir && !strcmp(absolute, app->games_dir))
        snprintf(note, sizeof note, "games/ is always listed");
    else if (i < app->folder_count)
        snprintf(note, sizeof note, "Already listed: %s", absolute);
    else if (app->folder_count == GUI_FOLDERS_MAX)
        snprintf(note, sizeof note, "At most %d folders", GUI_FOLDERS_MAX);
    else {
        app->folders[app->folder_count++] = absolute;
        absolute = NULL;
        added = 1;
        snprintf(note, sizeof note, "%s", folders_save(app) ? app->editor.note : "Added");
        count_games(app);
    }
    free(absolute);
    if (here && added) open_folders(app, app->folder_count);
    if (here)
        snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
    else
        snprintf(app->message, sizeof app->message, "Game folders: %s", note);
}
static void folder_remove(App* app, int folder) {
    char note[160];
    free(app->folders[folder]);
    memmove(app->folders + folder, app->folders + folder + 1, (size_t)(app->folder_count - folder - 1) * sizeof(char*));
    --app->folder_count;
    count_games(app);
    note[0] = 0;
    if (folders_save(app)) snprintf(note, sizeof note, "%s", app->editor.note);
    open_folders(app, SDL_min(folder + 1, app->folder_count));
    snprintf(app->editor.note, sizeof app->editor.note, "%s", note[0] ? note : "Taken off the list; the folder stays");
}
/* the system's folder picker answers on its own thread, as the file picker */
static void SDLCALL folder_picked(void* userdata, const char* const* files, int filter) {
    SDL_Event e;
    (void)filter;
    SDL_zero(e);
    e.type = *(const Uint32*)userdata;
    e.user.code = files == NULL; /* an error, which SDL_GetError names */
    e.user.data1 = files && files[0] ? SDL_strdup(files[0]) : NULL;
    e.user.data2 = files ? NULL : SDL_strdup(SDL_GetError());
    SDL_PushEvent(&e);
}
/* a group's row of the Settings, its help the keys it holds */
static void group_add(App* app, const char* name, int first, const char* about) {
    int g = app->groups++;
    GuiThemeKey* k = &app->group_keys[g];
    memset(k, 0, sizeof *k);
    k->section = "";
    k->key = name;
    k->type = GUI_KEY_ACTION;
    k->about = app->group_about[g];
    snprintf(app->group_about[g], sizeof app->group_about[g], "%s", about);
    app->group_first[g] = first;
    app->group_rows[g] = 0;
}
static void settings_groups(App* app) {
    GuiSettings* s = settings_of(app);
    int r;
    app->groups = 0;
    group_add(app, "App", 0, "The theme, the menu's letters, the theme editor and the game folders");
    for (r = 0; r < s->count; ++r) {
        int g = app->groups - 1;
        char* about = app->group_about[g];
        size_t n = strlen(about);
        if (strchr(s->keys[r].section, '/')) continue; /* a page's: its row in the group opens it */
        /* past the most groups the rest go in the last one: every row is still reached */
        if (g == 0 || (strcmp(s->keys[r].section, app->group_keys[g].key) && app->groups < GUI_GROUPS_MAX)) {
            group_add(app, s->keys[r].section, r, "");
            g = app->groups - 1;
            about = app->group_about[g];
            n = 0;
        }
        snprintf(about + n, sizeof app->group_about[g] - n, "%s%s", n ? ", " : "", s->keys[r].key);
        ++app->group_rows[g];
    }
}
/* the groups, row on the focus */
static void settings_top(App* app, int row) {
    settings_groups(app);
    gui_editor_form(&app->editor, app->group_keys, app->groups, &app->app_form, "Reset all to the defaults");
    app->editor.flat = true;
    app->editor.row = SDL_min(row, gui_editor_rows(&app->editor) - 1);
    app->form = GUI_FORM_SETTINGS;
    app->editing = true;
}
static void open_app(App* app, int row) {
    app_values(app);
    gui_editor_form(&app->editor, app_keys, GUI_APP_ROWS, &app->app_form, NULL);
    app->editor.flat = true;
    app->editor.row = row;
    app->form = GUI_FORM_APP;
    app->group = 0;
    app->editing = true;
}
/* games/, then the player's folders, a row each with its state, and Add a folder */
static void open_folders(App* app, int row) {
    int i;
    for (i = 0; i <= app->folder_count; ++i) {
        GuiThemeKey* k = &app->folder_keys[i];
        const char* path = i ? app->folders[i - 1] : app->games_dir;
        memset(k, 0, sizeof *k);
        k->section = "";
        k->key = path ? path : "games/";
        k->type = GUI_KEY_ACTION;
        k->at = offsetof(GuiAppForm, folder) + (size_t)i * sizeof app->app_form.folder[0];
        k->size = sizeof app->app_form.folder[0];
        k->about = i ? "Del, or Enter twice: take it off the list; the folder stays"
                     : "The data folder's games/, always "
                       "listed";
        snprintf(app->app_form.folder[i], sizeof app->app_form.folder[i], "%s",
                 path && snx_is_dir(path) ? "" : "not found");
    }
    gui_editor_form(&app->editor, app->folder_keys, app->folder_count + 1, &app->app_form, "Add a folder");
    app->editor.flat = true;
    app->editor.row = SDL_min(row, gui_editor_rows(&app->editor) - 1);
    app->form = GUI_FORM_FOLDERS;
    app->folder_armed = 0;
    app->editing = true;
}
static void open_folder_picker(App* app) {
    if (!app->picked_folder) app->picked_folder = SDL_RegisterEvents(1);
    if (!app->picked_folder) return;
#ifdef SDL_PLATFORM_ANDROID
    { /* SDL has no folder picker there: the system's own, through the app */
        char message[sizeof app->editor.note - 40];
        if (!gui_android_pick_folder(folder_picked, &app->picked_folder, message, sizeof message))
            snprintf(app->editor.note, sizeof app->editor.note, "The folder picker could not open: %s", message);
    }
#else
    SDL_ShowOpenFolderDialog(folder_picked, &app->picked_folder, app->window, NULL, false);
#endif
}
/* Select on a folder's row: once arms it, twice takes it off; games/ stays */
static void folder_select(App* app, int row) {
    if (!row)
        snprintf(app->editor.note, sizeof app->editor.note, "games/ is always listed");
    else if (app->folder_armed == row)
        folder_remove(app, row - 1);
    else {
        app->folder_armed = row;
        snprintf(app->editor.note, sizeof app->editor.note, "Enter again takes it off the list");
    }
}
static void open_group(App* app, int g) {
    if (!g) {
        open_app(app, 0);
        return;
    }
    gui_editor_form(&app->editor, app->settings.keys + app->group_first[g], app->group_rows[g], app->settings.values,
                    NULL);
    app->editor.flat = true;
    app->form = GUI_FORM_GROUP;
    app->group = g;
    app->first = app->group_first[g];
    app->page = -1;
}
static bool pad_page(const App* app, int row);
static void open_pad(App* app, int row);
static void pad_close(App* app);
/* a page's rows (settings.h), from its row in the group; the button map's, the pad drawn */
static void open_page(App* app, int row) {
    const GuiSetting* page = &app->settings.values[row];
    if (pad_page(app, row)) {
        open_pad(app, row);
        return;
    }
    gui_editor_form(&app->editor, app->settings.keys + page->first, page->count, app->settings.values, NULL);
    app->editor.flat = true;
    app->first = page->first;
    app->page = row;
}
static void open_settings(App* app) {
    app->group = 0;
    settings_top(app, 0);
    space_start(app);
}
static void leave_editor(App* app) {
    gui_editor_stop(&app->editor);
    app->editing = false;
    SDL_StopTextInput(app->window);
}
/* ESC on a screen: a group's and the App's go back to the groups, the theme editor to where it was opened */
static void leave_form(App* app) {
    int form = app->form;
    leave_editor(app);
    if (form == GUI_FORM_PAD) pad_close(app);
    if ((form == GUI_FORM_GROUP || form == GUI_FORM_PAD) && app->page >= 0) { /* a page: back to its row */
        int row = app->page;
        open_group(app, app->group);
        app->editor.row = row - app->first;
        app->editing = true;
    } else if (form == GUI_FORM_GROUP || form == GUI_FORM_APP)
        settings_top(app, app->group);
    else if (form == GUI_FORM_FOLDERS)
        open_app(app, GUI_APP_FOLDERS);
    else if (form == GUI_FORM_THEME && app->theme_from_app) {
        app->theme_from_app = false;
        open_app(app, GUI_APP_EDITOR);
    }
}
/* the App's rows: the theme and the letters go round, either way, and the editor opens */
static void app_action(App* app, int row, int step) {
    if (row == GUI_APP_THEME)
        next_theme(app, step < 0 ? -1 : 1);
    else if (row == GUI_APP_TEXT)
        app->menu.text_size = (app->menu.text_size + (step < 0 ? GUI_TEXT_MAX : 1)) % (GUI_TEXT_MAX + 1);
    else if (row == GUI_APP_EDITOR && !step) {
        app->theme_from_app = true;
        open_editor(app);
        return;
    } else if (row == GUI_APP_FOLDERS && !step) {
        open_folders(app, 0);
        return;
    }
    app_values(app);
}
/* saved in themes/ of the data folder, and the menu's theme from then on */
static void save_editor(App* app) {
    const char* const* dirs = (const char* const*)app->themes_dirs;
    if (gui_editor_save(&app->editor, app->themes_dirs[0])) return;
    gui_themes_scan(&app->themes, dirs, 2);
    gui_theme_load(&app->theme, dirs, 2, app->editor.id);
}
/* a Settings pick, kept at once */
static void keep_settings(App* app) {
    if ((app->form != GUI_FORM_GROUP && app->form != GUI_FORM_SETTINGS && app->form != GUI_FORM_PAD) ||
        !app->editor.dirty)
        return;
    app->editor.dirty = false;
    gui_settings_sync(&app->settings);
    if (app->settings_file && gui_settings_save(&app->settings, app->settings_file))
        snprintf(app->editor.note, sizeof app->editor.note, "Not saved: %s could not be written", app->settings_file);
}
/* ---- the Controllers' button map: the pad drawn with what each control sends (pad_screen.h), as the game's menu has
   it, its preset the row gamepad_buttons and its own map the row gamepad_map ---- */
static int setting_row(const App* app, const char* key) {
    int i;
    for (i = 0; i < app->settings.count; ++i)
        if (app->settings.values[i].key && !strcmp(app->settings.values[i].key, key)) return i;
    return -1;
}
/* a page of gamepad_map's, while gamepad_buttons has the presets' words */
static bool pad_page(const App* app, int row) {
    const GuiSetting* page = &app->settings.values[row];
    int map = setting_row(app, "gamepad_map"), buttons = setting_row(app, "gamepad_buttons");
    return map >= page->first && map < page->first + page->count && buttons >= 0 &&
           app->settings.keys[buttons].type == GUI_KEY_WORD && app->settings.values[buttons].values;
}
/* the pad's name, its labels and what it holds now */
static void pad_sync(App* app) {
    SnxPadScreen* p = &app->pad;
    const char* name = app->gamepad ? SDL_GetGamepadName(app->gamepad) : NULL;
    size_t n = 0;
    int c;
    if (app->gamepad && !name) name = "Controller";
    while (name && *name && n + 1 < sizeof p->pad) /* in the font's glyphs, as the page's other texts */
        p->pad[n++] = (char)gui_glyph(SDL_StepUTF8(&name, NULL));
    p->pad[n] = 0;
    p->held = 0;
    if (!app->gamepad) {
        memset(p->labels, 0, sizeof p->labels);
        p->symmetric = 0;
        return;
    }
    snx_pad_labels(app->gamepad, p->labels);
    p->symmetric = snx_pad_symmetric(app->gamepad);
    for (c = 0; c < SNX_PAD_CONTROLS; ++c)
        if (snx_pad_held(app->gamepad, c)) p->held |= 1u << c;
}
/* the presets, gamepad_buttons' words after the .ini's, and the map as gamepad_map says; a map it cannot read is
   named, and left out */
static void pad_start(App* app) {
    const GuiSettings* s = &app->settings;
    int buttons = setting_row(app, "gamepad_buttons"), map = setting_row(app, "gamepad_map"), modes[SNX_PAD_PRESETS], w;
    const GuiSetting* b = &s->values[buttons];
    const char* names[SNX_PAD_PRESETS];
    char error[64];
    for (w = 0; w < SNX_PAD_PRESETS && s->keys[buttons].words[w]; ++w) {
        names[w] = w ? s->keys[buttons].words[w] : "Each game's";
        modes[w] = w ? atoi(b->values[w]) : -1;
    }
    snx_pad_screen_start(&app->pad, w, names, modes, b->word < w ? b->word : 0);
    snx_pad_map_unset(&app->pad.own);
    if (snx_pad_map_read(&app->pad.own, s->values[map].text, error, sizeof error))
        snprintf(app->editor.note, sizeof app->editor.note,
                 "gamepad_map's \"%s\" is not a control and the button it sends (south:B)", error);
    pad_sync(app);
}
/* the first pad plugged in, when none is open */
static void pad_attach(App* app) {
    SDL_JoystickID* ids;
    int n = 0;
    if (app->gamepad || !app->pads_read) return;
    ids = SDL_GetGamepads(&n);
    if (n > 0 && !(app->gamepad = SDL_OpenGamepad(ids[0])))
        snprintf(app->editor.note, sizeof app->editor.note, "The controller could not be opened: %s", SDL_GetError());
    SDL_free(ids);
    app->pad_pushed = 0;
    pad_sync(app);
}
static void render(App* app);
static void open_pad(App* app, int row) {
    leave_editor(app);
    app->editing = true;
    app->form = GUI_FORM_PAD;
    app->page = row;
    app->editor.note[0] = 0;
    pad_start(app);
    render(app); /* SDL looks for pads for most of a second: the page shows first, saying so */
    app->pads_read = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!app->pads_read)
        snprintf(app->editor.note, sizeof app->editor.note, "Controllers cannot be read: %s", SDL_GetError());
    pad_attach(app);
}
static void pad_close(App* app) {
    if (app->gamepad) SDL_CloseGamepad(app->gamepad);
    app->gamepad = NULL;
    if (app->pads_read) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    app->pads_read = false;
}
/* what the page did: a preset or a map kept at once, as the Settings' other picks */
static void pad_did(App* app, int did) {
    GuiSettings* s = &app->settings;
    int map = setting_row(app, "gamepad_map");
    if (did == SNX_PAD_DID_LEAVE) {
        leave_form(app);
        return;
    }
    if (did != SNX_PAD_DID_PRESET && did != SNX_PAD_DID_MAP) return;
    s->values[setting_row(app, "gamepad_buttons")].word = app->pad.in_use;
    if (did == SNX_PAD_DID_MAP) {
        snx_pad_map_write(&app->pad.own, s->values[map].text, sizeof s->values[map].text);
        app->editor.note[0] = 0; /* a map it could not read is replaced */
    }
    app->editor.dirty = true;
    keep_settings(app);
}
/* a control of the pad: while the page waits, the press it waits for; else the key the console's button it sends
   is in the game's menu: A selects, B goes back, the D-pad moves */
static void pad_control(App* app, int control) {
    static const int KEYS[SNX_PAD_BUTTONS] = {
        SNX_PAD_KEY_SELECT, SNX_PAD_KEY_BACK, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, SNX_PAD_KEY_LEFT, SNX_PAD_KEY_UP,
        SNX_PAD_KEY_RIGHT,  SNX_PAD_KEY_DOWN};
    SnxPadMap map;
    int bit;
    if (control < 0) return;
    if (app->pad.listening) {
        pad_did(app, snx_pad_screen_press(&app->pad, control));
        return;
    }
    snx_pad_screen_map(&app->pad, &map);
    bit = map.bit[control];
    if (bit >= 0 && bit < SNX_PAD_BUTTONS && KEYS[bit] >= 0) pad_did(app, snx_pad_screen_key(&app->pad, KEYS[bit]));
}
/* a trigger past SNX_PAD_TRIGGER is its control pressed, the left stick past half way the D-pad: once as each crosses */
static void pad_axis(App* app, const SDL_GamepadAxisEvent* a) {
    int side;
    if (a->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        unsigned bit = 1u << (a->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        bool on = a->value > SNX_PAD_TRIGGER, was = (app->pad_pushed & bit) != 0;
        app->pad_pushed = on ? app->pad_pushed | bit : app->pad_pushed & ~bit;
        if (on && !was) pad_control(app, a->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? SNX_PAD_RT : SNX_PAD_LT);
        return;
    }
    if (a->axis != SDL_GAMEPAD_AXIS_LEFTX && a->axis != SDL_GAMEPAD_AXIS_LEFTY) return;
    for (side = 0; side < 2; ++side) {
        bool y = a->axis == SDL_GAMEPAD_AXIS_LEFTY, on = side ? a->value > 16384 : a->value < -16384;
        unsigned bit = 4u << (2 * y + side);
        bool was = (app->pad_pushed & bit) != 0;
        app->pad_pushed = on ? app->pad_pushed | bit : app->pad_pushed & ~bit;
        if (on && !was)
            pad_did(app, snx_pad_screen_key(&app->pad, y ? (side ? SNX_PAD_KEY_DOWN : SNX_PAD_KEY_UP)
                                                         : (side ? SNX_PAD_KEY_RIGHT : SNX_PAD_KEY_LEFT)));
    }
}
/* the page's keys; Del puts the preset and the map back to the .ini's */
static void pad_keyboard(App* app, const SDL_KeyboardEvent* k) {
    GuiKey key;
    if (k->key == SDLK_DELETE || k->key == SDLK_BACKSPACE) {
        int focus = app->pad.focus;
        gui_settings_reset(&app->settings, setting_row(app, "gamepad_buttons"));
        gui_settings_reset(&app->settings, setting_row(app, "gamepad_map"));
        app->editor.note[0] = 0;
        pad_start(app);
        app->pad.focus = focus;
        app->editor.dirty = true;
        keep_settings(app);
        return;
    }
    if (!menu_key_of(k, &key)) return;
    switch (key) {
        case GUI_UP: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_UP)); break;
        case GUI_DOWN: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_DOWN)); break;
        case GUI_LEFT: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_LEFT)); break;
        case GUI_RIGHT: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_RIGHT)); break;
        case GUI_SELECT: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_SELECT)); break;
        case GUI_CANCEL: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_BACK)); break;
        default: break;
    }
}
/* the page's events: keys, its pad's presses, pads plugged in and out; false for what the rest takes */
static bool pad_event(App* app, const SDL_Event* e) {
    SDL_JoystickID ours = app->gamepad ? SDL_GetGamepadID(app->gamepad) : 0;
    switch (e->type) {
        case SDL_EVENT_KEY_DOWN: pad_keyboard(app, &e->key); return true;
        case SDL_EVENT_TEXT_INPUT: return true;
        case SDL_EVENT_GAMEPAD_ADDED: pad_attach(app); return true;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (ours && e->gdevice.which == ours) {
                SDL_CloseGamepad(app->gamepad);
                app->gamepad = NULL;
                app->pad.listening = 0;
                pad_attach(app);
            }
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (ours && e->gbutton.which == ours) pad_control(app, snx_pad_of_button(e->gbutton.button));
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (ours && e->gaxis.which == ours) pad_axis(app, &e->gaxis);
            return true;
        default: return false;
    }
}
/* a text of the page, in the font's glyphs (CP437) a byte each */
static void pad_text(App* app, const SnxPadShape* p, GuiRgb c) {
    float x = p->x;
    const char* s;
    SDL_SetTextureColorMod(app->font, c.r, c.g, c.b);
    for (s = p->text; *s; ++s, x += 8.0f * (float)app->scale) glyph(app, x, p->y, (unsigned char)*s);
}
/* a round part of the pad */
static void pad_disc(App* app, const SnxPadShape* p, GuiRgb c) {
    enum { SIDES = 32 };
    SDL_Vertex v[SIDES + 1];
    int index[SIDES * 3], k;
    float r = SDL_min(p->w, p->h) / 2;
    SDL_FColor color = {(float)c.r / 255, (float)c.g / 255, (float)c.b / 255, 1};
    for (k = 0; k <= SIDES; ++k) {
        float a = 2 * SDL_PI_F * (float)k / SIDES;
        v[k].position.x = p->x + p->w / 2 + (k ? r * SDL_cosf(a) : 0);
        v[k].position.y = p->y + p->h / 2 + (k ? r * SDL_sinf(a) : 0);
        v[k].color = color;
        v[k].tex_coord.x = v[k].tex_coord.y = 0;
    }
    for (k = 0; k < SIDES; ++k) {
        index[3 * k] = 0;
        index[3 * k + 1] = k + 1;
        index[3 * k + 2] = k + 1 < SIDES ? k + 2 : 1;
    }
    SDL_RenderGeometry(app->renderer, NULL, v, SIDES + 1, index, SIDES * 3);
}
/* the page in the theme's colours: the panels a little darker than the page, the focus as the menus' and what is held
   on the pad in the active colour */
static void render_pad(App* app) {
    const GuiTheme* theme = &app->theme;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale;
    char title[160], *c;
    int i;
    SDL_SetRenderDrawColor(app->renderer, theme->page.r, theme->page.g, theme->page.b, 255);
    SDL_RenderClear(app->renderer);
    snprintf(title, sizeof title, "SETTINGS  /  %s  /  %s", app->group_keys[app->group].key,
             app->settings.keys[app->page].key);
    for (c = title; *c; ++c) *c = (char)SDL_toupper((unsigned char)*c);
    if (theme->head == GUI_STYLE_BAR) band(app, 0, 1, theme->ink);
    text(app, 1, 0, title, rgb(theme->head == GUI_STYLE_BAR ? theme->page : theme->ink));
    text(app, 1, 1, SAY_PAD, rgb(theme->ink));
    if (app->editor.note[0])
        text(app, 1, 2, app->editor.note, rgb(theme->failure));
    else if (!app->pads_read)
        text(app, 1, 2, "Looking for controllers...", rgb(theme->soft));
    pad_sync(app);
    snx_pad_screen_lay(&app->pad, cw, 3 * ch, (float)(app->cols - 2) * cw, (float)SDL_max(1, app->rows - 4) * ch, cw,
                       ch, &app->pad_drawing);
    for (i = 0; i < app->pad_drawing.n; ++i) {
        const SnxPadShape* p = &app->pad_drawing.shape[i];
        GuiRgb on = p->style == SNX_PAD_STYLE_FOCUS ? theme->focus : theme->active;
        bool marked = p->style == SNX_PAD_STYLE_FOCUS || p->style == SNX_PAD_STYLE_LIT;
        if (p->kind == SNX_PAD_SHAPE_TEXT)
            pad_text(app, p, marked ? theme->page : p->style == SNX_PAD_STYLE_SOFT ? theme->soft : theme->ink);
        else if (p->kind == SNX_PAD_SHAPE_DISC)
            pad_disc(app, p, marked ? on : theme->soft);
        else if (marked)
            fill(app, p->x, p->y, p->w, p->h, on, 255);
        else
            fill(app, p->x, p->y, p->w, p->h, theme->soft, p->kind == SNX_PAD_SHAPE_BAR ? 255 : 0x40);
    }
}
static void editor_key(App* app, GuiKey key) {
    GuiEditorDo what = gui_editor_key(&app->editor, key);
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_SETTINGS && !app->editor.step) {
        open_group(app, app->editor.row);
        return;
    }
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_GROUP) { /* a page's row opens it, an All steps its rows */
        int row = app->first + app->editor.row;
        if (app->settings.values[row].kind == GUI_SETTING_PAGE && !app->editor.step)
            open_page(app, row);
        else if (gui_settings_step_all(&app->settings, row, app->editor.step)) {
            app->editor.dirty = true;
            keep_settings(app);
        }
        return;
    }
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_APP) {
        app_action(app, app->editor.row, app->editor.step);
        return;
    }
    if (app->form == GUI_FORM_FOLDERS) {
        if (what == GUI_EDITOR_ACTION && !app->editor.step)
            folder_select(app, app->editor.row);
        else if (what == GUI_EDITOR_SAVE) /* Add a folder */
            open_folder_picker(app);
        if (what != GUI_EDITOR_ACTION || app->editor.step) app->folder_armed = 0;
        if (what != GUI_EDITOR_LEAVE) return;
    }
    if (what == GUI_EDITOR_SAVE && app->form == GUI_FORM_SETTINGS) { /* the groups' last row */
        gui_settings_reset(&app->settings, -1);
        app->editor.dirty = true;
    } else if (what == GUI_EDITOR_SAVE && app->form == GUI_FORM_THEME)
        save_editor(app);
    keep_settings(app);
    if (what == GUI_EDITOR_LEAVE) {
        leave_form(app);
        return;
    }
#ifdef SDL_PLATFORM_ANDROID
    if (app->form == GUI_FORM_GROUP) { /* the menu on the whole screen too, as the games, unless fullscreen is 0 */
        const char* fullscreen = gui_settings_value(&app->settings, "fullscreen");
        SDL_SetWindowFullscreen(app->window, !fullscreen || strcmp(fullscreen, "0"));
    }
#endif
    if (app->editor.typing && !SDL_TextInputActive(app->window))
        SDL_StartTextInput(app->window); /* a phone's keyboard comes up */
    else if (!app->editor.typing && SDL_TextInputActive(app->window))
        SDL_StopTextInput(app->window);
}
/* keys and typed text: while a value is typed, Space and the letters are its text */
static void editor_input(App* app, const SDL_Event* e) {
    GuiKey key;
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        gui_editor_type(&app->editor, e->text.text);
        return;
    }
    if (app->form == GUI_FORM_THEME && e->key.key == SDLK_S && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        save_editor(app);
        return;
    }
    /* Del, or Backspace when nothing is typed: a setting back to its default */
    if (app->form == GUI_FORM_GROUP && !app->editor.typing &&
        (e->key.key == SDLK_DELETE || e->key.key == SDLK_BACKSPACE)) {
        gui_settings_reset(&app->settings, app->first + app->editor.row);
        app->editor.dirty = true;
        keep_settings(app);
        return;
    }
    /* the game folders: Del takes one off, Ctrl+V adds the one pasted */
    if (app->form == GUI_FORM_FOLDERS && (e->key.key == SDLK_DELETE || e->key.key == SDLK_BACKSPACE)) {
        if (app->editor.row && app->editor.row <= app->folder_count)
            folder_remove(app, app->editor.row - 1);
        else if (!app->editor.row)
            snprintf(app->editor.note, sizeof app->editor.note, "games/ is always listed");
        return;
    }
    if (app->form == GUI_FORM_FOLDERS && e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        char* path = gui_drop_path(clipboard ? clipboard : "", true);
        if (path)
            folder_add(app, path);
        else
            snprintf(app->editor.note, sizeof app->editor.note, "Paste one local folder's path");
        free(path);
        SDL_free(clipboard);
        return;
    }
    if (e->key.key == SDLK_TAB && !e->key.repeat && !app->editor.typing) {
        screen_tab(app, e->key.mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if (app->form == GUI_FORM_THEME && e->key.key == SDLK_P && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        app->editor.preview = app->editor.preview == GUI_SCREEN_MENU ? GUI_SCREEN_COMPILING : GUI_SCREEN_MENU;
        app->preview_since = 0;
        return;
    }
    if (app->editor.typing) {
        if (e->key.key == SDLK_BACKSPACE) gui_editor_erase(&app->editor);
        if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL)) {
            char* clipboard = SDL_GetClipboardText();
            gui_editor_type(&app->editor, clipboard ? clipboard : "");
            SDL_free(clipboard);
        }
        if (e->key.key == SDLK_SPACE) return;
    }
    if (menu_key_of(&e->key, &key)) editor_key(app, key);
}
/* a click or a tap on a row focuses it, and on the focused one is Select; on the preview it switches the screen */
static void editor_pointer(App* app, float x, float y, bool click) {
    int col, row, cols, rows, r = app->editor.top + (int)y - 4;
    preview_box(app, &col, &row, &cols, &rows);
    if (!click) return;
    if (app->form == GUI_FORM_THEME && x >= col && x < col + cols && y >= row && y < row + rows + 1) {
        app->editor.preview = app->editor.preview == GUI_SCREEN_MENU ? GUI_SCREEN_COMPILING : GUI_SCREEN_MENU;
        app->preview_since = 0;
        return;
    }
    if (y < 4 || r < 0 || r >= gui_editor_rows(&app->editor) || (int)y - 4 >= editor_rows(app)) return;
    if (r == app->editor.row && !app->editor.typing)
        editor_key(app, GUI_SELECT);
    else if (r != app->editor.row) {
        if (app->editor.typing) editor_key(app, GUI_CANCEL);
        app->editor.row = r;
    }
}
/* ---- the Settings and the theme editor in boxes: the groups on the left, the open or the focused group's rows on the
   right; the theme editor's keys on the left, the preview on the right ---- */
typedef struct {
    GuiPlace place;
    GuiList left, right;
    GuiEditor peek;        /* the focused group's rows, shown while the focus is on the groups */
    const GuiEditor* rows; /* the right box's rows: the editor's, or peek */
    GuiRect info;          /* the right box's foot, for a line about its rows */
} GuiFormBoxes;
static const char* form_title(const App* app, char* out, size_t size) {
    if (app->theme.head == GUI_STYLE_NONE) return "";
    if (app->form == GUI_FORM_THEME)
        snprintf(out, size, "THEME EDITOR  /  %s", app->editor.id);
    else
        snprintf(out, size, "SETTINGS");
    return out;
}
static const char* form_keys(const App* app, char* out, size_t size) {
    return screen_keys(app,
                       app->form == GUI_FORM_THEME      ? SAY_EDITOR
                       : app->form == GUI_FORM_SETTINGS ? SAY_GROUPS
                       : app->form == GUI_FORM_APP      ? SAY_APP
                       : app->form == GUI_FORM_FOLDERS  ? SAY_FOLDERS
                                                        : SAY_SETTINGS,
                       out, size);
}
/* the group the groups' list shows open, -1 for none: the one being edited, App for its folders */
static int form_group(const App* app) {
    if (app->form == GUI_FORM_GROUP) return app->group;
    return app->form == GUI_FORM_APP || app->form == GUI_FORM_FOLDERS ? 0 : -1;
}
/* the columns a form's row label takes, the widest's */
static int form_labels(const GuiEditor* e) {
    char label[64];
    int r, labels = 8;
    for (r = 0; r < gui_editor_rows(e); ++r) {
        gui_editor_label(e, r, label, sizeof label);
        labels = SDL_max(labels, (int)strlen(label));
    }
    return labels;
}
static void form_boxes(App* app, GuiFormBoxes* f) {
    char title[96], keys[256];
    int theme = app->form == GUI_FORM_THEME, width, height, g, longest = (int)strlen("Reset all to the defaults");
    area(app, &width, &height);
    for (g = 0; g < app->groups; ++g) longest = SDL_max(longest, (int)strlen(app->group_keys[g].key));
    place_screen(app, &f->place, form_title(app, title, sizeof title), form_keys(app, keys, sizeof keys),
                 theme ? width / cell_w(app) : longest + 2 + (int)SDL_utf8strlen(app->theme.marker), GUI_SHOWS_TABS);
    f->rows = &app->editor;
    if (app->form == GUI_FORM_SETTINGS) { /* the focused group's rows, to see before opening it */
        int g = app->editor.row;
        memset(&f->peek, 0, sizeof f->peek);
        if (g == 0) {
            app_values(app);
            gui_editor_form(&f->peek, app_keys, GUI_APP_ROWS, &app->app_form, NULL);
        } else if (g < app->groups)
            gui_editor_form(&f->peek, app->settings.keys + app->group_first[g], app->group_rows[g],
                            app->settings.values, NULL);
        f->peek.flat = true;
        f->peek.row = -1;
        f->rows = &f->peek;
    }
    if (theme)
        list_place(app, &f->left, f->place.list, gui_editor_rows(&app->editor), app->editor.row);
    else
        list_place(app, &f->left, f->place.list, app->groups + 1,
                   app->form == GUI_FORM_SETTINGS ? app->editor.row : -1);
    f->info = box_rect(f->place.side.x, f->place.side.y + f->place.side.h - cell_h(app) - 4 * app->scale,
                       f->place.side.w, cell_h(app) + 2 * app->scale);
    list_place(app, &f->right,
               box_rect(f->place.side.x + cell_w(app), f->place.side.y + head_h(app) + 4 * app->scale,
                        f->place.side.w - 2 * cell_w(app), f->info.y - f->place.side.y - head_h(app) - 6 * app->scale),
               theme ? 0 : gui_editor_rows(f->rows), app->form == GUI_FORM_SETTINGS ? -1 : f->rows->row);
}
/* a row of a form as its box shows it: its label, the value past the widest label; one being typed ends where the
   typing is, its start giving way */
static void form_row(App* app, const GuiList* l, const GuiEditor* e, int r, int labels, int focused, int marked) {
    char label[64], value[96], line[200];
    const GuiThemeKey* k = gui_editor_key_of(e, r);
    int typing = focused && e->typing, room = (list_box(l, r).w - cell_w(app) * 3 / 2) / cell_w(app); /* list_item's */
    size_t n;
    gui_editor_label(e, r, label, sizeof label);
    if (typing)
        snprintf(value, sizeof value, "%s_", e->text);
    else
        gui_editor_value(e, r, value, sizeof value);
    if (app->form == GUI_FORM_GROUP && e == &app->editor && k && !value[0]) snprintf(value, sizeof value, ".ini");
    snprintf(line, sizeof line, "%s%-*s %s", app->theme.marker, labels, label, value);
    n = SDL_utf8strlen(line);
    if (typing && room > 4 && n > (size_t)room) { /* the typed end in view */
        const char* tail = value;
        size_t skip = n - (size_t)room + 3;
        while (skip-- && *tail) SDL_StepUTF8(&tail, NULL);
        snprintf(line, sizeof line, "%s%-*s ...%s", app->theme.marker, labels, label, tail);
    }
    list_item(app, l, r, line, focused, marked);
    if (k && k->type == GUI_KEY_COLOUR) { /* its colour, framed in soft so a page-coloured one shows */
        GuiRect b = list_box(l, r);
        const GuiRgb* c = (const GuiRgb*)((const char*)(e->object ? e->object : (const char*)&e->theme) + k->at);
        int x = b.x + cell_w(app) / 2 + (int)SDL_utf8strlen(line) * cell_w(app) + cell_w(app), s = app->scale;
        if (x + 2 * cell_w(app) <= b.x + b.w) {
            fill_rect(app, box_rect(x, b.y + 3 * s, 2 * cell_w(app), b.h - 6 * s), app->theme.soft);
            fill_rect(app, box_rect(x + s, b.y + 4 * s, 2 * cell_w(app) - 2 * s, b.h - 8 * s), *c);
        }
    }
}
/* what the bar says of the focused row: the note of the last key, else the row's help */
static const char* form_about(const App* app, SDL_Color* color) {
    const GuiEditor* e = &app->editor;
    const GuiThemeKey* k = gui_editor_key_of(e, e->row);
    *color = rgb(e->note[0] ? (e->leaving ? app->theme.failure : app->theme.focus) : app->theme.ink);
    if (e->note[0]) return e->note;
    if (k) return k->about;
    if (app->form == GUI_FORM_SETTINGS) return "Every setting back to its default";
    if (app->form == GUI_FORM_FOLDERS) return "Pick, drop or paste a folder of games";
    return e->object ? "" : e->row == GUI_EDITOR_FOLDER ? "The folder of themes/ it is saved in" : "Ctrl+S saves too";
}
static void draw_form_boxed(App* app) {
    GuiFormBoxes f;
    char title[96], keys[256], line[1100];
    const char* about;
    SDL_Color color;
    int r, labels, theme = app->form == GUI_FORM_THEME;
    SDL_SetRenderDrawColor(app->renderer, app->theme.page.r, app->theme.page.g, app->theme.page.b, 255);
    SDL_RenderClear(app->renderer);
    form_boxes(app, &f);
    chrome(app, &f.place);
    title_box(app, f.place.title, form_title(app, title, sizeof title));
    if (theme) {
        labels = form_labels(&app->editor);
        for (r = f.left.first; r < f.left.first + f.left.shown; ++r)
            form_row(app, &f.left, &app->editor, r, labels, r == app->editor.row, false);
    } else
        for (r = f.left.first; r < f.left.first + f.left.shown; ++r) {
            snprintf(line, sizeof line, "%s%s", app->theme.marker,
                     r < app->groups ? app->group_keys[r].key : "Reset all to the defaults");
            list_item(app, &f.left, r, line, app->form == GUI_FORM_SETTINGS && r == app->editor.row,
                      r == form_group(app));
        }
    list_marks(app, &f.left);
    if (theme) { /* the preview, 16 by 10, and what switches it */
        GuiRect p = inner(app, f.place.side);
        int w = SDL_min(p.w, (f.info.y - f.place.side.y - 2 * cell_h(app)) * 16 / 10), y;
        y = pane(app, f.place.side, "Preview");
        if (w > 0 && preview_texture(app)) {
            SDL_FRect box = {(float)(p.x + (p.w - w) / 2), (float)y, (float)w, (float)(w * 10 / 16)};
            SDL_RenderTexture(app->renderer, app->preview, NULL, &box);
            text_in(app, p, p.x, y + w * 10 / 16 + 2 * app->scale,
                    app->editor.preview != GUI_SCREEN_MENU ? "A run (Ctrl+P: the menu)" : "The menu (Ctrl+P: a run)",
                    rgb(app->theme.soft));
        }
        if (w > 0) y += w * 10 / 16 + 2 * app->scale + cell_h(app);
        snprintf(line, sizeof line, "Saves in %s", app->themes_dirs[0] ? app->themes_dirs[0] : "(no themes folder)");
        wrap_in(app, box_rect(p.x, y, p.w, f.place.side.y + f.place.side.h - y), p.x, y, line, 1, rgb(app->theme.soft));
    } else {
        int g = app->form == GUI_FORM_SETTINGS ? app->editor.row : app->group;
        pane(app, f.place.side,
             app->form == GUI_FORM_FOLDERS ? "Game folders"
             : g < app->groups             ? app->group_keys[g].key
                                           : "Reset all to the defaults");
        labels = form_labels(f.rows);
        for (r = f.right.first; r < f.right.first + f.right.shown; ++r)
            form_row(app, &f.right, f.rows, r, labels, f.rows == &app->editor && r == app->editor.row,
                     app->form == GUI_FORM_FOLDERS && r == app->folder_armed && r);
        list_marks(app, &f.right);
        if (app->form == GUI_FORM_SETTINGS || app->form == GUI_FORM_GROUP)
            text_in(app, inner(app, f.info), f.info.x + cell_w(app), f.info.y,
                    app->settings.count > GUI_SETTINGS_OWN ? "For the next runs. .ini: as the game's .ini says"
                                                           : "The runtime listed no settings",
                    rgb(app->theme.soft));
    }
    about = form_about(app, &color);
    bar(app, f.place.bar, about, color, form_keys(app, keys, sizeof keys));
}
/* a row of a form under the pointer: focused, or on the focused one Select, as a key would */
static void form_row_pointer(App* app, int r) {
    if (r == app->editor.row && !app->editor.typing)
        editor_key(app, GUI_SELECT);
    else if (r != app->editor.row) {
        if (app->editor.typing) editor_key(app, GUI_CANCEL);
        app->editor.row = r;
    }
}
/* a click on a form in boxes: a tab; a row of the list or of the right box; a group of the list opens it from a
   group's rows; the theme editor's preview switches */
static void form_boxed_pointer(App* app, float x, float y, bool touch) {
    GuiFormBoxes f;
    int i;
    if ((i = tab_hit(app, x, y)) >= 0) {
        go_tab(app, i);
        return;
    }
    form_boxes(app, &f);
    if (app->form == GUI_FORM_THEME && x >= (float)f.place.side.x && x < (float)(f.place.side.x + f.place.side.w) &&
        y >= (float)f.place.side.y && y < (float)(f.place.side.y + f.place.side.h)) {
        app->editor.preview = app->editor.preview == GUI_SCREEN_MENU ? GUI_SCREEN_COMPILING : GUI_SCREEN_MENU;
        app->preview_since = 0;
        return;
    }
    if ((i = list_hit(&f.left, x, y, touch)) >= 0) {
        if (app->form == GUI_FORM_THEME || app->form == GUI_FORM_SETTINGS) {
            form_row_pointer(app, i);
            return;
        }
        if (app->editor.typing) editor_key(app, GUI_CANCEL);
        keep_settings(app);
        leave_editor(app);
        settings_top(app, i); /* another group: from its row, as Enter there */
        if (i < app->groups) open_group(app, i);
        return;
    }
    if ((i = list_hit(&f.right, x, y, touch)) < 0) return;
    if (app->form == GUI_FORM_SETTINGS) { /* a row of the focused group: it opens, on that row */
        if (app->editor.row >= app->groups) return;
        open_group(app, app->editor.row);
        app->editor.row = SDL_min(i, gui_editor_rows(&app->editor) - 1);
        return;
    }
    form_row_pointer(app, i);
}
static void render(App* app) {
    layout(app);
    if (splash_shown(app)) return;
    if (app->editing && app->form == GUI_FORM_PAD)
        render_pad(app);
    else if (app->editing && boxed(app))
        draw_form_boxed(app);
    else if (app->editing)
        render_editor(app);
    else
        draw_screen(app);
    SDL_RenderPresent(app->renderer);
}
/* the game's name as its run gave it, from the step that names it, for its recent game */
static void recent_name(App* app, const char* label) {
    const GuiStep* named = gui_steps_last(&app->steps, label);
    if (!app->recent_file || app->recent_named || !app->file || !named || !named->detail[0]) return;
    app->recent_named = 1;
    if (gui_recent_rename(&app->recent, app->file, named->detail)) recent_changed(app);
}
/* back to the menu from a finished run, which fades in */
static void count_games(App* app);
static void list_games(App* app);
static uint64_t games_stamp(const App* app);
static void leave_run(App* app) {
    app->finished = false;
    app->menu.view = GUI_LOAD_NONE;
    count_games(app); /* a run may have made one, and taken disk */
    space_start(app);
    if (app->menu.active == GUI_GAMES && app->menu.games) app->menu.selected = GUI_GAMES;
    app->message[0] = 0;
    app->leaving_ms = 0;
    app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
    layout(app);
}
#ifdef SDL_PLATFORM_ANDROID
/* the working folder moves to /sdcard/ScarletNX once the app may write there, which the user grants after it starts */
static bool android_folder(App* app) {
    char* cwd = gui_android_folder() ? SDL_GetCurrentDirectory() : NULL;
    if (!cwd || (app->cwd && !strcmp(cwd, app->cwd))) {
        SDL_free(cwd);
        return false;
    }
    SDL_free(app->cwd);
    app->cwd = cwd;
    free(app->data_dir);
    app->data_dir = snx_strdup(cwd);
    free(app->games_dir);
    app->games_dir = snx_path_join(cwd, "games");
    free(app->themes_dirs[0]);
    app->themes_dirs[0] = snx_path_join(cwd, "themes");
    return true;
}
/* A game runs in the app's game activity, a process of its own: no child of this one gets a window */
static int play(App* app, const char* game) {
    GuiSettings* s = settings_of(app);
    char* option = snx_format("--game=%s", game);
    const char** argv = (const char**)snx_alloc((size_t)(s->count + 3) * sizeof(*argv));
    int ok;
    argv[0] = option;
    argv[1 + gui_settings_options(s, 0, 1, argv + 1)] = NULL;
    ok = gui_android_play(argv, app->message, sizeof(app->message));
    free(argv);
    free(option);
    return ok;
}
/* the game a run of the driver made ready: there the driver names it, as it cannot open it; whether it opened */
static int ready(App* app) {
    const GuiStep* made = gui_steps_last(&app->steps, "READY");
    char* game = made ? snx_path_join(app->games_dir, made->detail) : NULL;
    int ok = game && play(app, game);
    free(game);
    return ok;
}
/* the run in its notification: the game, the step on the screen and the percent, as they change, a second apart at
   most (Android drops notifications that come faster) */
static void tell(App* app) {
    char name[96], runs[3][112], line[sizeof app->told];
    size_t lengths[3];
    int percent = gui_steps_percent(&app->steps);
    Uint64 now = SDL_max(SDL_GetTicks(), 1);
    line[0] = 0;
    if (app->steps.count) {
        step_runs(&app->steps.step[app->steps.count - 1], app->theme.progress, now_ms(app), runs, lengths);
        snprintf(line, sizeof line, "%s%s%s", runs[0], runs[1], runs[2]);
    }
    if (app->told_ms && (now - app->told_ms < 1000 || (percent == app->told_percent && !strcmp(line, app->told))))
        return;
    run_name(app, name, sizeof name);
    gui_android_running(name, line, percent);
    snprintf(app->told, sizeof app->told, "%s", line);
    app->told_percent = percent;
    app->told_ms = now;
}
#endif
#ifdef SNX_XDG
/* a line more while an older build's file is at old, which this one keeps at now */
static void left_line(App* app, const char* old, const char* now) {
    char *line, *grown;
    size_t n = app->left ? strlen(app->left) : 0;
    if (!snx_left_behind(old, now)) return;
    line = snx_format("%s" SAY_LEFT_BY "%s\n", old, now);
    grown = (char*)realloc(app->left, n + strlen(line) + 1);
    if (grown) {
        strcpy(grown + n, line);
        app->left = grown;
        app->lefts++;
    }
    free(line);
}
#endif
/* what older builds left where this one no longer looks, for the menu to say while it is there: the menu's own files,
   work/, each game's (common.h). An installed build's alone (XDG): elsewhere the folders are as they were */
static void left_behind(App* app) {
    free(app->left);
    app->left = NULL;
    app->lefts = 0;
    app->left_at = SDL_GetTicks();
#ifdef SNX_XDG
    static const char* const mine[] = {"recent.txt", "last-run.log"};
    char *data = snx_user_data_dir(), *folder = app->recent_file ? snx_dirname(app->recent_file) : NULL;
    char *work = snx_path_join(app->data_dir, "work"), *moved = snx_user_path(work, SNX_USER_CACHE);
    char** names = NULL;
    size_t count = 0, i, j;
    for (i = 0; data && folder && i < sizeof mine / sizeof *mine; ++i) {
        char *old = snx_format("%s/gui/%s", data, mine[i]), *now = snx_path_join(folder, mine[i]);
        left_line(app, old, now);
        free(old);
        free(now);
    }
    left_line(app, work, moved);
    snx_list_dir(app->games_dir, &names, &count);
    for (i = 0; i < SNX_GAME_FILES; ++i) {
        const char* name = snx_game_files[i].name;
        char* to = snx_user_path(app->games_dir, snx_game_files[i].kind);
        for (j = 0; j < (name[0] == '.' ? count : 1); ++j) { /* a game's own, by its name, or the folder's */
            const char* file = name[0] == '.' ? names[j] : name;
            size_t n = strlen(file), k = strlen(name);
            char *old, *now;
            if (name[0] == '.' && (n <= k || strcmp(file + n - k, name))) continue;
            old = snx_path_join(app->games_dir, file);
            now = snx_path_join(to, file);
            left_line(app, old, now);
            free(old);
            free(now);
        }
        free(to);
    }
    snx_free_strings(names, count);
    free(moved);
    free(work);
    free(folder);
    free(data);
#endif
}
static void pump(App* app) {
    if (app->menu.running) {
        char buffer[4096];
        /* Bounded work per frame keeps keyboard/window events responsive even
           when a compiler floods the inherited pipe. There is no log backlog. */
        for (int reads = 0; reads < 16; ++reads) {
            int n = gui_process_read(app->process, buffer, sizeof(buffer));
            if (n <= 0) break;
            if (app->run_out) fwrite(buffer, 1, (size_t)n, app->run_out);
            gui_console_feed(&app->console, buffer, (size_t)n);
            gui_steps_feed(&app->steps, buffer, (size_t)n, now_ms(app));
            app->has_output = true;
        }
        recent_name(app, "LAUNCH");
#ifdef SDL_PLATFORM_ANDROID
        tell(app);
#endif
        int status;
        if (gui_process_finished(app->process, &status)) {
            app->menu.running = false;
            /* Normal exits wait for acknowledgement. ESC already acknowledged
               an abort, so return straight to the menu after nonblocking reaping. */
            app->finished = !app->aborting;
            app->abnormal_exit = app->aborting || status != 0;
            gui_steps_finish(&app->steps, !app->abnormal_exit);
            if (app->run_out) {
                fprintf(app->run_out, "\n[%s, status %d]\n", app->aborting ? "aborted" : "ended", status);
                fclose(app->run_out);
                app->run_out = NULL;
            }
            recent_name(app, "LAUNCH");
            recent_name(app, "RECOMPILE"); /* a run that ended before its game loaded */
            app->menu.view = GUI_LOAD_NONE;
            if (app->aborting)
                snprintf(app->message, sizeof(app->message), "Program ended abnormally (force-terminated by ESC).");
            else if (app->abnormal_exit)
                snprintf(app->message, sizeof(app->message), "Program ended abnormally (status %d).", status);
            else
                snprintf(app->message, sizeof(app->message), "Driver exited with status %d.", status);
            app->aborting = false;
            app->confirm.shown = false; /* a run that ended while asking has nothing left to abort */
            layout(app);
#ifdef SDL_PLATFORM_ANDROID
            /* the game opens in its own activity, over this one: coming back from it is coming back to the menu.
               Android lets no app behind others open one, so then it waits for the menu to show */
            if (!app->abnormal_exit && app->hidden)
                app->play_later = true;
            else if (!app->abnormal_exit && ready(app))
                leave_run(app);
            /* seen here, the end needs no word; behind other apps one stays until the menu shows */
            gui_android_ran(!app->hidden ? NULL : app->abnormal_exit ? "Failed" : "Ready");
#endif
        }
    }
#ifdef SDL_PLATFORM_ANDROID
    if (app->play_later && !app->hidden) {
        app->play_later = false;
        if (ready(app)) leave_run(app);
    }
#endif
    Uint64 now = SDL_GetTicks();
    if (!app->sampled || now - app->sampled >= 500) {
        app->usage = gui_process_usage(app->process);
        app->sampled = now;
    }
    if (app->left_shown && (!app->left_at || now - app->left_at >= 5000)) { /* the player may have moved them */
        left_behind(app);
        app->left_shown = app->lefts > 0;
    }
    if (app->leaving_ms && now_ms(app) - app->leaving_ms >= app->theme.fade_out_ms) leave_run(app);
    /* games put in or taken out of the folders while the menu is open, looked for once a second: Games and its list
       follow, the list on the file it had */
    if (!app->menu.running && !app->finished && now_ms(app) - app->games_checked >= 1000) {
        app->games_checked = now_ms(app);
        if (games_stamp(app) != app->games_stamp) {
            count_games(app);
            if (app->menu.view == GUI_LOAD_GAMES) {
                GuiBrowser was = app->browser;
                list_games(app);
                app->browser.page = was.page;
                app->browser.selected = was.selected;
                gui_browser_resize(&app->browser, app->browser.page_size);
            }
        }
    }
}
static void launch(App* app, char* absolute, bool prepared) {
#ifdef SDL_PLATFORM_ANDROID
    android_folder(app); /* the driver works where the menu stands */
#endif
    if (!absolute || !snx_is_file(absolute)) {
        snprintf(app->message, sizeof(app->message), "That path is not a readable file. Try again or press ESC.");
        free(absolute);
        return;
    }
    /* The 3DS browser lists CIA inputs, not prepared host executables. */
    if (app->menu.selected == GUI_3DS) prepared = false;
    bool native = prepared && app->menu.selected == GUI_SWITCH;
    char* driver = native ? NULL : find_driver(app, app->menu.selected);
    if (!native && !driver) {
        snprintf(app->message, sizeof(app->message), "Cannot find %s beside the GUI or on PATH.",
                 driver_name(app->menu.selected) ? driver_name(app->menu.selected) : "the selected driver");
        free(absolute);
        return;
    }
#ifdef SDL_PLATFORM_ANDROID
    if (native) {
        if (play(app, absolute) && app->recent_file && !gui_recent_add(&app->recent, absolute, GUI_SWITCH, true))
            recent_changed(app);
        free(absolute);
        return;
    }
#endif
    /* the driver, the file, the settings, --repo and its folder, NULL */
    GuiSettings* s = settings_of(app);
    const char** argv = (const char**)snx_alloc((size_t)(s->count + 6) * sizeof(*argv));
    size_t n = 1;
    argv[0] = native ? absolute : driver;
    if (!native) argv[n++] = absolute;
    /* Switch options only, never guest argv for the CPU-only Linux driver or the 3DS one. The driver works in the
       menu's data folder, so the two list the same games */
    if (app->menu.selected == GUI_SWITCH) n += gui_settings_options(s, 1, native, argv + n);
    if (app->menu.selected == GUI_SWITCH && !native) {
        argv[n++] = "--repo";
        argv[n++] = app->data_dir;
    }
    argv[n] = NULL;
    bool started = gui_process_start(app->process, argv, app->message, sizeof(app->message));
    if (started && app->run_log) {
        size_t k;
        if (app->run_out) fclose(app->run_out);
        app->run_out = snx_fopen(app->run_log, "wb");
        for (k = 0; app->run_out && k < n; ++k) fprintf(app->run_out, "%s%s", k ? " " : "$ ", argv[k]);
        if (app->run_out) fputc('\n', app->run_out);
    }
    free(argv);
    gui_steps_reset(&app->steps);
    app->native = native;
    /* the menu fades out, then the run fades in */
    Uint64 now = SDL_max(now_ms(app), 1);
    app->entering_ms = started && app->theme.fade_out_ms ? now : 0;
    app->fade_ms = started ? now + (app->entering_ms ? app->theme.fade_out_ms : 0) : 0;
    app->leaving_ms = 0;
    free(driver);
    if (!started) {
        free(absolute);
        return;
    }
    free(app->file);
    app->file = absolute;
    if (app->recent_file && !gui_recent_add(&app->recent, absolute, app->menu.selected, prepared)) recent_changed(app);
    app->recent_named = 0;
    app->menu.view = GUI_LOAD_NONE;
    app->menu.running = true;
    app->finished = false;
    app->abnormal_exit = false;
    app->message[0] = 0;
    gui_console_init(&app->console, app->cols - 2, app->rows - 8);
    app->has_output = true;
    layout(app);
#ifdef SDL_PLATFORM_ANDROID
    /* now, while the menu shows: Android 12 starts no notification's service for an app behind others */
    app->told_ms = 0;
    tell(app);
#endif
}
/* a recent game, run as the menu would: its driver, the options the menu has now */
static void open_recent(App* app, int index) {
    GuiRecentGame game = app->recent.game[index];
    if (!snx_is_file(game.path)) {
        snprintf(app->message, sizeof(app->message), "%s is gone (%s): it left the recent games.", game.name,
                 game.path);
        gui_recent_remove(&app->recent, index);
        recent_changed(app);
        return;
    }
    app->menu.selected = game.driver;
    launch(app, snx_strdup(game.path), game.prepared);
    if (app->menu.running) app->menu.recent_focus = 1; /* back from the run, on the game that is now first */
}
static void install_keys(App* app, const char* source) {
    const char* name = source;
    if (source)
        for (const char* p = source; *p; ++p)
            if (*p == '/' || *p == '\\') name = p + 1;
    if (!source || strcmp(name, "prod.keys") || !snx_is_file(source)) {
        snprintf(app->message, sizeof(app->message), "Please drop a readable file named prod.keys.");
        return;
    }
    if (!app->keys_path) {
        snprintf(app->message, sizeof(app->message), "Cannot locate your home directory. Keys were not installed.");
        return;
    }
    /* Stage privately on the same filesystem, then replace atomically. A failed
       copy (or dropping the installed file itself) must not destroy existing keys. */
    char* directory = snx_dirname(app->keys_path);
    char* temporary = directory && !snx_mkdirs(directory) ? snx_temp_dir(directory) : NULL;
    char* staged = temporary ? snx_path_join(temporary, "prod.keys") : NULL;
    bool ok = staged && !snx_copy_file(source, staged);
#ifndef _WIN32
    if (ok) ok = chmod(staged, 0600) == 0;
#endif
    if (ok) ok = !snx_replace(staged, app->keys_path);
    if (temporary) snx_remove_tree(temporary);
    free(staged);
    free(temporary);
    free(directory);
    snprintf(app->message, sizeof(app->message), "%s",
             ok ? "Device keys installed successfully."
                : "Cannot install prod.keys. Check file and folder permissions, then try again.");
    if (ok) app->menu.view = GUI_LOAD_NONE;
}
static void drop(App* app, const char* value, bool text_drop) {
    if ((app->menu.view != GUI_LOAD_DROP && app->menu.view != GUI_LOAD_KEYS) || app->menu.running || app->finished)
        return;
    char* path = gui_drop_path(value, text_drop);
    if (!path) {
        snprintf(app->message, sizeof(app->message), "Drop one local file path, not a list or remote URL.");
        return;
    }
    char* absolute = snx_absolute(path);
    free(path);
    if (app->menu.view == GUI_LOAD_KEYS) {
        install_keys(app, absolute);
        free(absolute);
    } else {
        /* Hovering another main item must not change the driver for this drop. */
        app->menu.selected = app->menu.active;
        launch(app, absolute, false);
    }
}
/* the system's file picker answers on its own thread: the choice comes back to event() as an app->picked event */
static void SDLCALL picker_done(void* userdata, const char* const* files, int filter) {
    SDL_Event e;
    (void)filter;
    SDL_zero(e);
    e.type = *(const Uint32*)userdata;
    e.user.data1 = files && files[0] ? SDL_strdup(files[0]) : NULL;
    SDL_PushEvent(&e);
}
static void open_picker(App* app) {
    if (!app->picked) app->picked = SDL_RegisterEvents(1);
    if (app->picked) SDL_ShowOpenFileDialog(picker_done, &app->picked, app->window, NULL, 0, NULL, false);
}
/* the last max bytes of a file, as text with only ASCII (what Java takes as a string from any log), or "" */
static char* log_tail(const char* path, size_t max) {
    size_t size = 0, i;
    unsigned char* data = path ? snx_read_file(path, &size) : NULL;
    char* text = (char*)snx_alloc(SDL_min(size, max) + 1);
    for (i = 0; i < SDL_min(size, max); ++i) {
        unsigned char c = data[size - SDL_min(size, max) + i];
        text[i] = c == '\n' || c == '\t' || (c >= 32 && c < 127) ? (char)c : '?';
    }
    text[i] = 0;
    free(data);
    return text;
}
/* the game log written last, games/<game>.log: beside the games, or an installed build's in its state folder */
static char* newest_log(const App* app) {
    int count = 0, i;
    char* folder = snx_user_path(app->games_dir, SNX_USER_STATE);
    char** names = SDL_GlobDirectory(folder, "*.log", 0, &count);
    char* newest = NULL;
    SDL_Time time = 0;
    for (i = 0; names && i < count; ++i) {
        SDL_PathInfo info;
        char* path = snx_path_join(folder, names[i]);
        if (SDL_GetPathInfo(path, &info) && (!newest || info.modify_time > time)) {
            free(newest);
            newest = path;
            time = info.modify_time;
        } else
            free(path);
    }
    SDL_free(names);
    free(folder);
    return newest;
}
/* the message on the screen, the last run's output and the newest game log, as scarletnx.log: on Android in
   /sdcard/ScarletNX and handed to the share sheet as that file, elsewhere in the working folder with its path copied.
   An installed build saves it in its state folder (XDG), as the working folder is the home folder when the menu is
   started from the desktop */
static void share_log(App* app) {
    char *game, *run, *played, *text, *saved, *state = NULL;
    const char* folder;
#ifdef SDL_PLATFORM_ANDROID
    android_folder(app);
    folder = gui_android_folder();
#else
    folder = app->cwd;
#endif
#ifdef SNX_XDG
    if (installed(app)) state = snx_user_dir(SNX_USER_STATE);
    if (state && snx_mkdirs(state)) {
        free(state);
        state = NULL;
    }
    if (state) folder = state;
#endif
    left_behind(app);
    game = newest_log(app);
    run = log_tail(app->run_log, 1 << 20);
    played = log_tail(game, 4 << 20);
    text = snx_format("%s\nMessage: %s\n%s\n== the last run: %s ==\n%s\n\n== %s ==\n%s\n", snx_build_info(),
                      app->message, app->left ? app->left : "", app->run_log ? app->run_log : "", run,
                      game ? game : "no game log", played);
    saved = folder ? snx_path_join(folder, "scarletnx.log") : NULL;
    if (!saved || snx_write_file(saved, text, strlen(text)))
        snprintf(app->message, sizeof(app->message), SAY_LOG_FAILED);
#ifdef SDL_PLATFORM_ANDROID
    else if (gui_android_share(saved, app->message, sizeof(app->message)))
        snprintf(app->message, sizeof(app->message), "Log saved to %s", saved);
#else
    else if (SDL_SetClipboardText(saved))
        snprintf(app->message, sizeof(app->message), "Log saved to %s (path copied)", saved);
    else
        snprintf(app->message, sizeof(app->message), "Log saved to %s", saved);
#endif
    free(saved);
    free(text);
    free(played);
    free(run);
    free(game);
    free(state);
}
/* games/'s games and the player's folders', every kind; the first folder that cannot be read is named, the others
   still listed */
static void games_scan(App* app, GuiGames* games, char* message, size_t size) {
    int kinds = GUI_GAMES_RUN | GUI_GAMES_CIA, i;
    char error[sizeof app->message];
#ifdef SDL_PLATFORM_ANDROID
    android_folder(app);
#endif
    gui_games_free(games);
    if (!gui_games_add(games, app->games_dir, kinds, error, sizeof error))
        snprintf(message, size, "Cannot open games/. Create it in the displayed working directory.");
    else
        message[0] = 0;
    for (i = 0; i < app->folder_count; ++i)
        if (!gui_games_add(games, app->folders[i], kinds, error, sizeof error) && !message[0])
            snprintf(message, size, "%s It is in Settings > App > Game folders.", error);
}
/* the names games/ and the player's folders hold, hashed: a game put in, taken out or renamed changes it, and so does a
   folder that comes or goes */
static uint64_t games_stamp(const App* app) {
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    int i;
    for (i = -1; i < app->folder_count; ++i) {
        const char* dir = i < 0 ? app->games_dir : app->folders[i];
        char** names = NULL;
        size_t count = 0, k;
        const char* c;
        if (!dir || snx_list_dir(dir, &names, &count)) count = (size_t)-1; /* none: not the same as an empty one */
        for (k = 0; k < count && names; ++k)
            for (c = names[k];; ++c) {
                h = (h ^ (unsigned char)*c) * 1099511628211ull;
                if (!*c) break;
            }
        h = (h ^ (uint64_t)count) * 1099511628211ull;
        if (names) snx_free_strings(names, count);
    }
    return h;
}
/* how many games Games has: it shows when there are any */
static void count_games(App* app) {
    GuiGames games = {0};
    char message[sizeof app->message];
    games_scan(app, &games, message, sizeof message);
    app->games_stamp = games_stamp(app);
    app->games_total = games.count;
    app->menu.games = games.count > 0;
    if (!app->menu.games && app->menu.selected == GUI_GAMES) app->menu.selected = GUI_SWITCH;
    gui_games_free(&games);
}
static void list_games(App* app) {
    games_scan(app, &app->games, app->message, sizeof app->message);
    app->games_total = app->games.count;
    memset(&app->browser, 0, sizeof(app->browser));
    app->browser.count = app->games.count;
    layout(app);
}
/* the screen a tab opens: the menu, Games or the Settings. A theme edited and not saved keeps its screen, the note
   saying why */
static void go_tab(App* app, int tab) {
    if (app->editing) {
        if (app->editor.typing || tab == GUI_TAB_SETTINGS) return;
        if (app->form == GUI_FORM_THEME && app->editor.dirty) {
            snprintf(app->editor.note, sizeof app->editor.note,
                     "Not saved: Ctrl+S saves it, ESC twice leaves without it");
            return;
        }
        keep_settings(app);
        leave_editor(app);
        app->theme_from_app = false;
    }
    if (tab == tab_shown(app)) return;
    app->message[0] = 0;
    app->menu.view = tab == GUI_TAB_GAMES ? GUI_LOAD_GAMES : GUI_LOAD_NONE;
    if (tab == GUI_TAB_GAMES)
        list_games(app);
    else if (tab == GUI_TAB_SETTINGS)
        open_settings(app);
    else if (app->menu.games)
        app->menu.selected = GUI_GAMES; /* back from it, as its Back does */
}
/* the tab step tabs from the one shown, round the three: Tab and Shift+Tab, a pad's shoulders */
static void screen_tab(App* app, int step) {
    if (app->menu.running || app->finished || app->confirm.shown || app->splash) return;
    go_tab(app, ((tab_shown(app) + step) % GUI_TABS + GUI_TABS) % GUI_TABS);
}
static void menu_key(App* app, GuiKey key) {
    GuiLoadView before = app->menu.view; /* the screen this key was pressed on */
    if (app->finished) {
        if (key == GUI_CANCEL && app->theme.fade_out_ms && !app->leaving_ms)
            app->leaving_ms = SDL_max(now_ms(app), 1); /* the run fades out first; pump() comes back */
        else if (key == GUI_CANCEL)
            leave_run(app);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        /* Resizes and key events can arrive in the same SDL poll batch. */
        layout(app);
        GuiBrowserAction action = gui_browser_key(&app->browser, key);
        if (action == GUI_BROWSER_BACK) {
            app->menu.view = GUI_LOAD_NONE;
            app->menu.selected = GUI_GAMES;
            app->message[0] = 0;
        } else if (action == GUI_BROWSER_OPEN) {
            size_t index = app->browser.page * app->browser.page_size + app->browser.selected - 1;
            if (index < app->games.count) {
                /* a CIA is the 3DS driver's, anything else a Switch game's: ready to run, or a dump to prepare */
                app->menu.selected = gui_is_cia(app->games.names[index]) ? GUI_3DS : GUI_SWITCH;
                launch(app, snx_absolute(app->games.paths[index]), !gui_is_dump(app->games.names[index]));
                if (!app->menu.running) app->menu.selected = GUI_GAMES;
            }
        }
        return;
    }
    if (pick_screen(app) && key != GUI_SELECT && key != GUI_CANCEL) return; /* nothing there to move to */
    switch (gui_menu_key(&app->menu, key)) {
        case GUI_MENU_RECENT:
            if (app->menu.recent_focus <= recent_shown(app))
                open_recent(app, app->menu.recent_focus - 1);
            else { /* ...show more: every recent game, the focus on the first that was hidden */
                app->recent_all = true;
                app->menu.recent = app->recent.count;
            }
            return;
        case GUI_MENU_SETTINGS: open_settings(app); break;
        case GUI_MENU_LOG: share_log(app); break;
        default: break;
    }
    if (key == GUI_CANCEL || (key == GUI_SELECT && pick_screen(app))) app->message[0] = 0;
    /* the screen opens first, so what happens shows there; a select on it opens the picker, whose answer may come at
       once */
#ifdef SDL_PLATFORM_ANDROID
    /* a phone has no screen to pick on: a load opens the picker at once; the keys' screen does on a tap */
    if (key == GUI_SELECT && (app->menu.view == GUI_LOAD_DROP || (before == GUI_LOAD_KEYS && app->menu.view == before)))
#else
    if (key == GUI_SELECT && app->menu.view == before && (before == GUI_LOAD_DROP || before == GUI_LOAD_KEYS))
#endif
        open_picker(app);
    if (app->menu.view == GUI_LOAD_GAMES) list_games(app);
    /* Back or ESC with nothing open to close: on Android the app goes behind, as an app's first screen does; on a
       computer the menu asks before it closes, as rEden does */
    if (key == GUI_CANCEL && before == GUI_LOAD_NONE && app->menu.view == GUI_LOAD_NONE && !app->menu.running) {
#ifdef SDL_PLATFORM_ANDROID
        gui_android_background(app->message, sizeof(app->message));
#else
        app->confirm.shown = true;
        app->confirm.exit = true;
        app->confirm.selected = GUI_CONFIRM_KEEP;
#endif
    }
}
/* a label's cells, or its whole row for a finger, which is wider than a mouse pointer */
static bool hit_text(const App* app, float x, float y, int col, int row, const char* label, bool touch) {
    int end = touch ? app->cols : SDL_min(app->cols - 1, col + (int)SDL_utf8strlen(label));
    return x >= (touch ? 0 : col) && x < end && y >= row && y < row + 1;
}
static void abort_run(App* app) {
    if (app->aborting) return;
    /* Do not ask a hung program to shut down or wait on it in the event
       handler. Kill its tree now and let pump reap it without blocking. */
    gui_process_terminate(app->process);
    app->aborting = true;
}
/* the question's second choice taken: the app closes, or the run is aborted */
static void confirmed(App* app) {
    if (app->confirm.exit)
        app->menu.quit = true;
    else
        abort_run(app);
}
/* the pointer on the question, at x, y in cells: on a choice it takes the focus, and a click takes the choice; a click
   off the box keeps going */
static void ask_pointer(App* app, float x, float y, bool click) {
    int col, row, cols;
    ask_box(app, &col, &row, &cols);
    if (x >= (float)col && x < (float)(col + cols) && y >= (float)(row + 3) && y < (float)(row + 5)) {
        app->confirm.selected = (int)y - row - 3;
        if (click && gui_confirm_key(&app->confirm, GUI_SELECT)) confirmed(app);
    } else if (click && (x < (float)col || x >= (float)(col + cols) || y < (float)row || y >= (float)(row + 6)))
        gui_confirm_key(&app->confirm, GUI_CANCEL);
}
/* the pointer on the question in boxes, at pixel x, y: a choice under it takes the focus, a click takes it; a click
   off the box keeps going */
static void ask_boxed_pointer(App* app, float x, float y, bool click) {
    GuiRect r;
    GuiList l;
    int i;
    ask_boxes(app, &r, &l);
    if ((i = list_hit(&l, x, y, true)) >= 0) {
        app->confirm.selected = i;
        if (click && gui_confirm_key(&app->confirm, GUI_SELECT)) confirmed(app);
    } else if (click && (x < (float)r.x || x >= (float)(r.x + r.w) || y < (float)r.y || y >= (float)(r.y + r.h)))
        gui_confirm_key(&app->confirm, GUI_CANCEL);
}
/* the tab at pixel x, y, -1 for none */
static int tab_hit(App* app, float x, float y) {
    GuiPlace p;
    int i;
    if (!app->theme.tabs) return -1;
    place_screen(app, &p, "", "", 0, GUI_SHOWS_TABS);
    for (i = 0; i < GUI_TABS; ++i) {
        GuiRect r = tab_rect(app, p.tabs, i);
        if (x >= (float)r.x && x < (float)(r.x + r.w) && y >= (float)r.y && y < (float)(r.y + r.h)) return i;
    }
    return -1;
}
/* the pointer on a screen in boxes, at pixel x, y: the box under it takes the focus, and a click selects it; on the
   screen a file is picked on, a click on the box the waiting line is in opens the picker */
static void boxed_pointer(App* app, float x, float y, bool click, bool touch) {
    GuiMenuBoxes m;
    int i;
    if ((i = tab_hit(app, x, y)) >= 0) {
        if (click) go_tab(app, i);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        GuiGamesBoxes g;
        games_boxes(app, &g);
        if ((i = list_hit(&g.list, x, y, touch)) < 0) return;
        app->browser.selected = (size_t)i;
        if (click) menu_key(app, GUI_SELECT);
        return;
    }
    if (pick_screen(app)) {
        GuiPickBoxes k;
        pick_boxes(app, &k);
        if (click && list_hit(&k.back, x, y, touch) == 0)
            menu_key(app, GUI_CANCEL);
        else if (click && y >= (float)k.place.list.y && y < (float)k.back.at.y && x >= (float)k.place.list.x &&
                 x < (float)(k.place.list.x + k.place.list.w))
            menu_key(app, GUI_SELECT); /* the box the waiting line is in */
        return;
    }
    menu_boxes(app, &m);
    i = list_hit(&m.list, x, y, touch);
    if (i < 0 || m.entry[i].kind == GUI_ENTRY_LABEL) return;
    if (m.entry[i].kind == GUI_ENTRY_RECENT)
        app->menu.recent_focus = m.entry[i].index + 1;
    else {
        app->menu.selected = m.entry[i].index;
        app->menu.recent_focus = 0;
    }
    if (click) menu_key(app, GUI_SELECT);
}
static void pointer(App* app, float window_x, float window_y, bool click, bool touch) {
    if ((app->menu.running && !app->confirm.shown) || app->finished) return;
    layout(app);
    float x, y;
    if (!SDL_RenderCoordinatesFromWindow(app->renderer, window_x, window_y, &x, &y)) return;
    if (app->editing && app->form == GUI_FORM_PAD) {
        if (click) pad_did(app, snx_pad_screen_tap(&app->pad, snx_pad_screen_hit(&app->pad_drawing, x, y)));
        return;
    }
    if (boxed(app) && app->editing) {
        if (click) form_boxed_pointer(app, x, y, touch);
        return;
    }
    if (boxed(app) && app->confirm.shown) {
        ask_boxed_pointer(app, x, y, click);
        return;
    }
    if (boxed(app)) {
        boxed_pointer(app, x, y, click, touch);
        return;
    }
    x /= 8 * app->scale;
    y /= 16 * app->scale;
    if (app->editing) {
        editor_pointer(app, x, y, click);
        return;
    }
    if (app->confirm.shown) {
        ask_pointer(app, x, y, click);
        return;
    }
    if (pick_screen(app)) {
        if (click && hit_text(app, x, y, 1, 8, "Back [ESC]", touch))
            menu_key(app, GUI_CANCEL);
        else if (click && hit_text(app, x, y, 1, SDL_max(app->rows / 2, 10), SAY_KEYS_WAIT, true))
            menu_key(app, GUI_SELECT); /* its whole row: the theme may center or frame the prompt */
        return;
    }
    /* the line under the pointer takes the focus, and a click selects it */
    if (app->menu.view == GUI_LOAD_GAMES) {
        size_t start = app->browser.page * app->browser.page_size;
        if (hit_text(app, x, y, 1, 3, "Back [ESC]", touch)) {
            app->browser.selected = 0;
            goto hit;
        }
        for (size_t i = 0; i < gui_browser_items(&app->browser); ++i)
            if (hit_text(app, x, y, 1, 5 + (int)i, app->games.names[start + i], touch)) {
                app->browser.selected = i + 1;
                goto hit;
            }
        return;
    }
    for (int game = 0; game < app->menu.recent; ++game) {
        char label[160];
        recent_label(app, game, label, sizeof label);
        if (menu_shows(app, recent_row(app, game)) && hit_text(app, x, y, 1, recent_row(app, game), label, touch)) {
            app->menu.recent_focus = game + 1;
            goto hit;
        }
    }
    for (int i = 0; i < GUI_ITEM_COUNT; ++i) {
        char label[100];
        if (i == GUI_GAMES && !app->menu.games) continue;
        menu_label(app, i, label, sizeof(label));
        if (menu_shows(app, menu_row(app, i)) && hit_text(app, x, y, 1, menu_row(app, i), label, touch)) {
            app->menu.selected = i;
            app->menu.recent_focus = 0;
            goto hit;
        }
    }
    return;
hit:
    if (click) menu_key(app, GUI_SELECT);
}
/* the file the system's picker chose, as a dropped one; none (cancelled) stays on the screen and says so */
static void picked(App* app, const char* uri) {
    char* path = uri ? gui_document_path(uri) : NULL;
#ifdef SDL_PLATFORM_ANDROID
    if (uri && !path) path = gui_android_document_file(uri);
#endif
    if (!uri)
        snprintf(app->message, sizeof(app->message), "No file was picked.");
    else if (!path)
        snprintf(app->message, sizeof(app->message), "Pick the file from the device's storage, not from an app (%s).",
                 uri);
    else
        drop(app, path, false);
    free(path);
}
/* ESC: a run is aborted, or asked about first when ask_abort says so; anything else goes back */
static void cancel(App* app) {
    if (!app->menu.running)
        menu_key(app, GUI_CANCEL);
    else if (app->ask_abort && !app->aborting) {
        app->confirm.shown = true;
        app->confirm.exit = false;
        app->confirm.selected = GUI_CONFIRM_KEEP;
    } else
        abort_run(app);
}
/* the row of a running run's ESC line, -1 when it has none */
static int escape_row(const App* app) {
    if (app->theme.run == GUI_RUN_STEPS) return app->rows - 1;
    return app->theme.head != GUI_STYLE_NONE ? 3 : -1;
}
/* a finger's id, the pen's apart: SDL gives a pen's touch the id 1 on every platform, the id Android and Wayland
   give a first finger and any finger may have, and no real finger's id sets bit 63 */
static Uint64 finger_id(const SDL_TouchFingerEvent* f) {
    return (Uint64)f->fingerID | (f->touchID == SDL_PEN_TOUCHID ? 1ull << 63 : 0);
}
/* a row up or down for a finger's drag and the wheel, which stop at the first and last rows where the keys go round */
static void scroll(App* app, GuiKey key) {
    int row;
    if (app->editing && app->form == GUI_FORM_PAD) {
        pad_did(app, snx_pad_screen_key(&app->pad, key == GUI_UP ? SNX_PAD_KEY_UP : SNX_PAD_KEY_DOWN));
        return;
    }
    if (app->editing) {
        editor_key(app, key); /* stops at its ends */
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        if (key == GUI_UP ? app->browser.selected > 0 : app->browser.selected < gui_browser_items(&app->browser))
            menu_key(app, key);
        return;
    }
    row = menu_focus_row(app);
    menu_key(app, key);
    if (key == GUI_UP ? menu_focus_row(app) > row : menu_focus_row(app) < row)
        menu_key(app, key == GUI_UP ? GUI_DOWN : GUI_UP); /* it went round: back */
}
/* the menu's rows go past the screen: its letters are too big for it */
static bool menu_scrolls(App* app) {
    GuiMenuBoxes m;
    if (!boxed(app)) return menu_row_in_full(app, GUI_EXIT) > menu_last_row(app);
    layout(app);
    menu_boxes(app, &m);
    return m.list.shown < m.list.count;
}
/* a drag up or down from where the finger went down, on a menu whose letters make it scroll */
static bool drags(App* app, float x, float y) {
    float dx = x - app->finger_x, dy = y - app->finger_y;
    return SDL_fabsf(dy) > 0.05f && SDL_fabsf(dy) > 2 * SDL_fabsf(dx) && app->menu.view != GUI_LOAD_GAMES &&
           !app->menu.running && !app->finished && (app->editing ? app->form != GUI_FORM_PAD : menu_scrolls(app));
}
/* the focus follows the finger, up as it goes up, a row each time it passes a row's height, so the highlight is seen
   going */
static void drag(App* app, float y) {
    float row = 1.0f / (float)SDL_max(1, app->rows);
    while (y <= app->finger_mark - row) {
        scroll(app, GUI_UP);
        app->finger_mark -= row;
    }
    while (y >= app->finger_mark + row) {
        scroll(app, GUI_DOWN);
        app->finger_mark += row;
    }
}
/* a finger lifted at x, y (0 to 1 across the window): a swipe across turns the games' pages; a tap clicks there, is ESC
   on a finished run and on a running one's ESC line */
static void touch(App* app, float x, float y) {
    float dx = x - app->finger_x, dy = y - app->finger_y, rx, ry;
    int w, h;
    if (SDL_fabsf(dx) > 0.1f && SDL_fabsf(dx) > 2 * SDL_fabsf(dy)) {
        if (app->menu.view == GUI_LOAD_GAMES && !app->menu.running && !app->finished)
            menu_key(app, dx < 0 ? GUI_PAGE_DOWN : GUI_PAGE_UP);
        return;
    }
    if (drags(app, x, y)) {
        drag(app, y); /* what its last moves did not reach */
        return;
    }
    if (SDL_fabsf(dx) > 0.03f || SDL_fabsf(dy) > 0.03f) return; /* a drag, no tap */
    if (app->finished) {
        menu_key(app, GUI_CANCEL);
        return;
    }
    SDL_GetWindowSize(app->window, &w, &h);
    if (app->confirm.shown) {
        pointer(app, x * (float)w, y * (float)h, true, true);
        return;
    }
    if (app->menu.running) {
        GuiPlace p;
        layout(app);
        if (!SDL_RenderCoordinatesFromWindow(app->renderer, x * (float)w, y * (float)h, &rx, &ry)) return;
        if (boxed(app) && app->theme.run != GUI_RUN_STEPS) { /* the bar the ESC line is in */
            run_place(app, &p);
            if (ry >= (float)p.bar.y && ry < (float)(p.bar.y + p.bar.h)) cancel(app);
        } else if ((int)(ry / (float)(16 * app->scale)) == escape_row(app))
            cancel(app);
        return;
    }
    pointer(app, x * (float)w, y * (float)h, true, true);
}
#ifdef SDL_PLATFORM_ANDROID
/* SDL hands the app's lifecycle to watchers only, inside SDL_PollEvent on this thread. Back from the system's settings,
   the user may have just let the app write to /sdcard */
static bool SDLCALL lifecycle(void* data, SDL_Event* e) {
    App* app = (App*)data;
    if (e->type == SDL_EVENT_DID_ENTER_BACKGROUND) app->hidden = true;
    if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) app->hidden = false;
    if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND && android_folder(app)) {
        if (app->menu.view == GUI_LOAD_GAMES)
            list_games(app);
        else
            count_games(app);
    }
    return true;
}
#endif
/* the menu's key for a key press; false for none and for Enter or Space held down */
static bool menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key) {
    switch (k->key) {
        case SDLK_UP: *key = GUI_UP; return true;
        case SDLK_DOWN: *key = GUI_DOWN; return true;
        case SDLK_LEFT: *key = GUI_LEFT; return true;
        case SDLK_RIGHT: *key = GUI_RIGHT; return true;
        case SDLK_PAGEUP: *key = GUI_PAGE_UP; return true;
        case SDLK_PAGEDOWN: *key = GUI_PAGE_DOWN; return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: *key = GUI_SELECT; return !k->repeat;
        case SDLK_ESCAPE:
        case SDLK_AC_BACK: *key = GUI_CANCEL; return true;
        default: return false;
    }
}
static void event(App* app, const SDL_Event* e) {
    GuiKey key;
    if (e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        app->menu.quit = true;
        return;
    }
    /* the box of an older version's files takes the first key, button or finger, which closes it */
    if (app->left_shown && !app->splash &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_FINGER_DOWN)) {
        app->left_shown = false;
        return;
    }
    /* the splash takes the input it gets: a key, a button or a finger ends it, and does nothing else */
    if (app->splash && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) {
        app->splash_until = 0;
        return;
    }
    if (app->editing && app->form == GUI_FORM_PAD && pad_event(app, e)) return;
    if (app->editing && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        editor_input(app, e);
        return;
    }
    if ((e->type == SDL_EVENT_DROP_FILE || e->type == SDL_EVENT_DROP_TEXT) && app->editing &&
        app->form == GUI_FORM_FOLDERS) {
        char* path = gui_drop_path(e->drop.data, e->type == SDL_EVENT_DROP_TEXT);
        if (path)
            folder_add(app, path);
        else
            snprintf(app->editor.note, sizeof app->editor.note, "Drop one local folder");
        free(path);
        return;
    }
    if (e->type == SDL_EVENT_DROP_FILE || e->type == SDL_EVENT_DROP_TEXT) {
        drop(app, e->drop.data, e->type == SDL_EVENT_DROP_TEXT);
        return; /* SDL3 owns the event's data, unlike SDL2. */
    }
    /* the folder picker's answer: a folder, none, or why it could not open */
    if (app->picked_folder && e->type == app->picked_folder) {
        char* error = (char*)e->user.data2;
        if (e->user.data1)
            folder_add(app, (const char*)e->user.data1);
        else {
            char note[sizeof app->editor.note];
            if (e->user.code)
                snprintf(note, sizeof note, "The folder picker could not open: %s", error ? error : "no reason given");
            else
                snprintf(note, sizeof note, "No folder was picked.");
            if (app->editing && app->form == GUI_FORM_FOLDERS)
                snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
            else
                snprintf(app->message, sizeof app->message, "Game folders: %s", note);
        }
        SDL_free(e->user.data1);
        SDL_free(error);
        return;
    }
    if (app->picked && e->type == app->picked) {
        picked(app, e->user.data1);
        SDL_free(e->user.data1);
        return;
    }
    if (e->type == SDL_EVENT_FINGER_DOWN && !app->finger) {
        app->finger = finger_id(&e->tfinger);
        app->finger_x = e->tfinger.x;
        app->finger_y = app->finger_mark = e->tfinger.y;
        return;
    }
    if (e->type == SDL_EVENT_FINGER_MOTION && finger_id(&e->tfinger) == app->finger) {
        if (drags(app, e->tfinger.x, e->tfinger.y)) drag(app, e->tfinger.y);
        return;
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        finger_id(&e->tfinger) == app->finger) {
        app->finger = 0;
        if (e->tfinger.touchID == SDL_PEN_TOUCHID) {
            app->pen_held = true;
            app->pen_x = e->tfinger.x;
            app->pen_y = e->tfinger.y;
        }
        if (e->type == SDL_EVENT_FINGER_UP) touch(app, e->tfinger.x, e->tfinger.y);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        pointer(app, e->motion.x, e->motion.y, false, false);
        return;
    }
    /* a notch of the wheel is a row, as a finger's drag, the way the system turns it; not under a box */
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        int rows = e->wheel.integer_y;
        if (app->splash || app->menu.running || app->finished || app->confirm.shown || app->left_shown) return;
        for (; rows > 0; --rows) scroll(app, GUI_UP);
        for (; rows < 0; ++rows) scroll(app, GUI_DOWN);
        return;
    }
    /* a hovering pen highlights as the mouse does (it sends no mouse motion). Touching, it is a finger, and once it
       lifts its hover waits until it has moved away as far as a tap may: the focus a drag moved would otherwise jump
       to the row under the hover at the lift, which SDL sends right after it */
    if (e->type == SDL_EVENT_PEN_PROXIMITY_OUT) {
        app->pen_held = false;
        return;
    }
    if (e->type == SDL_EVENT_PEN_MOTION) {
        int w, h;
        if (e->pmotion.pen_state & SDL_PEN_INPUT_DOWN) return;
        SDL_GetWindowSize(app->window, &w, &h);
        if (app->pen_held && w > 0 && h > 0 && SDL_fabsf(e->pmotion.x / (float)w - app->pen_x) <= 0.03f &&
            SDL_fabsf(e->pmotion.y / (float)h - app->pen_y) <= 0.03f)
            return;
        app->pen_held = false;
        pointer(app, e->pmotion.x, e->pmotion.y, false, false);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT) {
        pointer(app, e->button.x, e->button.y, true, false);
        return;
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return;
    /* a running run takes ESC, and the question, while it shows, every key; a held key asks and answers nothing */
    if (app->menu.running) {
        if (e->key.repeat || !menu_key_of(&e->key, &key)) return;
        if (app->confirm.shown) {
            if (gui_confirm_key(&app->confirm, key)) confirmed(app);
        } else if (key == GUI_CANCEL)
            cancel(app);
        return;
    }
    /* the exit question takes every key while it shows */
    if (app->confirm.shown) {
        if (!e->key.repeat && menu_key_of(&e->key, &key) && gui_confirm_key(&app->confirm, key)) confirmed(app);
        return;
    }
    if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) &&
        (app->menu.view == GUI_LOAD_DROP || app->menu.view == GUI_LOAD_KEYS) && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        drop(app, clipboard, true);
        SDL_free(clipboard);
        return;
    }
    if (e->key.key == SDLK_TAB && !e->key.repeat) {
        screen_tab(app, e->key.mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if (menu_key_of(&e->key, &key)) menu_key(app, key);
}
/* the themes that come with the GUI: beside it in a build tree, else where an install puts data, share/scarletnx/themes
   beside its bin/ (/usr/local, the Windows zip) */
static char* shipped_themes(const char* program_dir) {
    char* beside = snx_path_join(program_dir, "themes");
    if (snx_is_dir(beside)) return beside;
    free(beside);
    return snx_path_join(program_dir, "../share/scarletnx/themes");
}
static bool initialize(App* app, const char* self) {
    SDL_WindowFlags flags;
    memset(app, 0, sizeof(*app));
    app->menu.selected = GUI_SWITCH; /* the first item until Games shows */
    if (self) app->self = snx_strdup(self);
#ifdef SDL_PLATFORM_ANDROID
    app->theme = *gui_theme_builtin("yorha");
    app->ask_abort = true;
    gui_android_start();
    /* back is ESC here: without this it would close the app */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* a run goes on behind other apps: its output is read, or it stops once the pipe is full */
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
#else
    app->theme = *gui_theme_builtin("default");
#endif
    /* touch() takes the fingers, and a finger and the mouse each arrive once: SDL would also send each as the other,
       and a pen as both (it stays a finger), so one tap would select twice */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) return false;
#ifdef SDL_PLATFORM_ANDROID
    /* the whole screen, the system's bars out of the way, as the Settings' fullscreen says: a phone held wide is short */
    flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    if (!SDL_CreateWindowAndRenderer(snx_build_info(), 1280, 800, flags, &app->window, &app->renderer)) return false;
    snx_window_icon(app->window);
    SDL_SetRenderVSync(app->renderer, 1);
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, true);
    app->font = create_font(app->renderer);
    app->process = gui_process_create();
    app->cwd = SDL_GetCurrentDirectory();
#ifdef SDL_PLATFORM_ANDROID
    app->drivers = gui_android_libraries() ? snx_strdup(gui_android_libraries()) : NULL;
    app->data_dir = snx_strdup(app->cwd ? app->cwd : ".");
#else
    app->drivers = self ? snx_dirname(self) : SDL_GetBasePath() ? snx_strdup(SDL_GetBasePath()) : NULL;
    app->data_dir = snx_data_dir(GUI_CHECKOUT, GUI_BUILD, app->drivers);
    if (!app->data_dir) app->data_dir = snx_strdup(app->cwd ? app->cwd : ".");
#endif
    app->games_dir = snx_path_join(app->data_dir, "games");
    app->themes_dirs[0] = snx_path_join(app->data_dir, "themes");
    if (app->drivers) app->themes_dirs[1] = shipped_themes(app->drivers);
    app->keys_path = device_keys_path(app);
    gui_console_init(&app->console, 78, 17);
    if (!app->font || !app->process || !app->drivers) {
        SDL_SetError("Cannot initialize GUI resources");
        return false;
    }
#ifdef SDL_PLATFORM_ANDROID
    SDL_AddEventWatch(lifecycle, app);
#endif
    layout(app);
    return true;
}
static void cleanup(App* app) {
#ifdef SDL_PLATFORM_ANDROID
    SDL_RemoveEventWatch(lifecycle, app);
    gui_android_ran(NULL); /* the run goes with the menu */
#endif
    gui_process_destroy(app->process);
    space_stop(app);
    gui_settings_free(&app->settings);
    free(app->settings_file);
    for (int i = 0; i < app->folder_count; ++i) free(app->folders[i]);
    free(app->folders_file);
    free(app->left);
    free(app->self);
    free(app->drivers);
    free(app->file);
    free(app->data_dir);
    free(app->games_dir);
    free(app->themes_dirs[0]);
    free(app->themes_dirs[1]);
    gui_themes_free(&app->themes);
    free(app->keys_path);
    free(app->recent_file);
    if (app->run_out) fclose(app->run_out);
    free(app->run_log);
    gui_games_free(&app->games);
    SDL_free(app->cwd);
    if (app->editing) gui_editor_stop(&app->editor);
    SDL_DestroyTexture(app->preview);
    SDL_DestroyTexture(app->splash);
    SDL_DestroyTexture(app->font);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
}
#ifdef SNX_XDG
/* recent.txt and last-run.log: in the state folder's gui/ (~/.local/state/scarletnx/gui/). An older build's in the data
   folder's stay, the menu saying so (left_behind). Elsewhere SDL_GetPrefPath's folder (under %APPDATA% on Windows) */
static char* state_folder(void) {
    char *state = snx_user_dir(SNX_USER_STATE), *folder = state ? snx_path_join(state, "gui") : NULL;
    if (folder && snx_mkdirs(folder)) {
        free(folder);
        folder = NULL;
    }
    free(state);
    return folder;
}
#endif
/* the file of Settings picks: the global one, every game's (game NULL), in the config folder (XDG) or the menu's own;
   a game's own, games/<game>.ini there, is to come. See settings.h for which a run takes first */
static char* settings_path(const App* app, const char* game) {
#ifdef SNX_XDG
    char* folder = snx_user_dir(SNX_USER_CONFIG);
#else
    char* folder = app->recent_file ? snx_dirname(app->recent_file) : NULL;
#endif
    char* path = !folder ? NULL
                 : game  ? snx_format("%s/games/%s.ini", folder, game)
                         : snx_path_join(folder, "settings.ini");
    (void)app;
    free(folder);
    return path;
}
int snx_gui_main(const char* self) {
    App app;
    char* prefs;
    if (!initialize(&app, self)) {
        fprintf(stderr, "scarletnx: %s\n", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ScarletNX", SDL_GetError(), app.window);
        cleanup(&app);
        return 1;
    }
    /* the menu's own files, opened here, not in initialize(), so the tests keep their own */
#ifdef SNX_XDG
    prefs = state_folder();
#else
    {
        char* sdl = SDL_GetPrefPath("scarletnx", "gui");
        prefs = sdl ? snx_strdup(sdl) : NULL;
        SDL_free(sdl);
    }
#endif
    if (prefs) recent_open(&app, snx_path_join(prefs, "recent.txt"));
    if (prefs) app.run_log = snx_path_join(prefs, "last-run.log");
    app.settings_file = settings_path(&app, NULL);
    if (app.settings_file) {
        char* config = snx_dirname(app.settings_file);
        app.folders_file = config ? snx_path_join(config, "folders.txt") : NULL;
        free(config);
        folders_load(&app);
    }
    count_games(&app);
    if (app.menu.games) app.menu.selected = GUI_GAMES; /* the first item */
    space_start(&app);                                 /* the footer's disk */
    free(prefs);
    left_behind(&app);
    app.left_shown = app.lefts > 0;
    splash_start(&app);
    while (!app.menu.quit) {
        SDL_Event e;
#ifdef SDL_PLATFORM_ANDROID
        /* behind other apps with no run to read, the menu waits to show again */
        if (app.hidden && !app.menu.running && SDL_WaitEvent(&e)) event(&app, &e);
#endif
        while (SDL_PollEvent(&e)) event(&app, &e);
        pump(&app);
#ifdef SDL_PLATFORM_ANDROID
        if (!app.hidden)
#endif
            render(&app);
        SDL_Delay(16);
    }
    cleanup(&app);
    return 0;
}
#ifndef SNX_GUI_NO_MAIN /* the menu alone: Android's activity runs it, and a build without the Switch runtime */
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return snx_gui_main(NULL);
}
#endif
