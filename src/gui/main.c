/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "app.h"
#include "ini_lines.h"
#ifndef SNX_GUI_NO_MAIN
#include <SDL3/SDL_main.h>
#endif
/* the checkout the drivers were built from and its build tree: a build tree's menu lists the games/ its driver makes */
#ifdef SNX_SOURCE_DIR
#define GUI_CHECKOUT SNX_SOURCE_DIR
#define GUI_BUILD    SNX_BUILD_DIR
#else
#define GUI_CHECKOUT NULL
#define GUI_BUILD    NULL
#endif

const char* const gui_items[] = {"Games",
                                 "Load Switch NSP/XCI/NRO",
                                 "Load Linux ARM ELF",
                                 "Load 3DS CIA",
                                 "Setup device keys",
                                 "Settings",
                                 "Share log",
                                 "Exit"};
static void open_folders(App* app, int row);
static uint64_t games_stamp(const App* app);
static void count_games(App* app);
static void list_games(App* app);
static void screen_tab(App* app, int step);
static void event(App* app, const SDL_Event* e);
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
char* gui_app_keys_in_use(const App* app) {
    if (app->keys_path && snx_is_file(app->keys_path)) return snx_strdup(app->keys_path);
#ifdef SNX_XDG
    char* home = installed(app) ? home_keys_path() : NULL;
    if (home && snx_is_file(home)) return home;
    free(home);
#endif
    return NULL;
}
/* a theme's file with deprecated keys says what they were read as: in the note of the screen the theme was picked on,
   else in the menu's message */
static void theme_note(App* app) {
    char note[256];
    if (!gui_theme_old_keys(&app->theme, note, sizeof note)) return;
    if (app->editing)
        snprintf(app->editor.note, sizeof app->editor.note, "%s: %s", app->theme.id, note);
    else
        snprintf(app->message, sizeof app->message, "%s: %s", app->theme.id, note);
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
    theme_note(app);
}
/* Settings > App's picks, kept at once in app_file for the next start. The App's note, or the menu's message, says when
   they could not be */
static void app_save(App* app) {
    char text[256], *dir, *staged;
    int n, failed;
    if (!app->app_file) return;
    n = snprintf(text, sizeof text, "; Settings > App in the launcher, as they were left\n[app]\ntheme = %s\n",
                 app->theme.id);
    if (app->menu.text_size)
        n += snprintf(text + n, sizeof text - (size_t)n, "text_size = %d\n", app->menu.text_size);
    else
        n += snprintf(text + n, sizeof text - (size_t)n, "text_size = auto\n");
    n += snprintf(text + n, sizeof text - (size_t)n, "layout = %s\n", app->menu.panels ? "panels" : "classic");
    /* staged beside it, then put in place: a crash mid-write leaves the old picks */
    dir = snx_dirname(app->app_file);
    staged = snx_format("%s.new", app->app_file);
    failed = !dir || !staged || snx_mkdirs(dir) || snx_write_file(staged, text, (size_t)n) ||
             snx_replace(staged, app->app_file);
    if (failed && staged) remove(staged);
    if (failed && app->editing)
        snprintf(app->editor.note, sizeof app->editor.note, "Not kept for the next start: %s could not be written",
                 app->app_file);
    else if (failed)
        snprintf(app->message, sizeof app->message, "Not kept for the next start: %s could not be written",
                 app->app_file);
    free(dir);
    free(staged);
}
/* what app_save kept, as the menu opens. A value it does not know, or a theme no longer there, leaves what the menu has
   and the message names it */
static void app_load(App* app) {
    size_t size = 0, i;
    char *text = app->app_file ? (char*)snx_read_file(app->app_file, &size) : NULL, *key, *value, bad[384];
    const char* const* dirs = (const char* const*)app->themes_dirs;
    int scale = app->menu.text_size, n = 0;
    SnxIni ini;
    if (!text) return;
    bad[0] = 0;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        char* end = NULL;
        long number = strtol(value, &end, 10);
        if (!strcmp(ini.section, "app") && !strcmp(key, "theme")) {
            gui_themes_scan(&app->themes, dirs, 2);
            for (i = 0; i < app->themes.count && strcmp(app->themes.ids[i], value); ++i) {}
            if (i < app->themes.count) {
                gui_theme_load(&app->theme, dirs, 2, value);
                continue;
            }
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "text_size")) {
            if (!strcmp(value, "auto")) {
                scale = 0;
                continue;
            }
            if (*value && !*end && number >= 1 && number <= GUI_TEXT_MAX) {
                scale = (int)number;
                continue;
            }
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "layout")) {
            if (!strcmp(value, "classic") || !strcmp(value, "panels")) {
                app->menu.panels = !strcmp(value, "panels");
                continue;
            }
        }
        if (n) n += snprintf(bad + n, sizeof bad - (size_t)n, ", ");
        if (n < (int)sizeof bad && strcmp(ini.section, "app")) /* a key outside [app] says where it is */
            n += snprintf(bad + n, sizeof bad - (size_t)n, "[%s] ", ini.section);
        if (n < (int)sizeof bad) n += snprintf(bad + n, sizeof bad - (size_t)n, "%s = %s", key, value);
        if (n >= (int)sizeof bad) n = (int)sizeof bad - 1;
    }
    free(text);
    if (scale != app->menu.text_size) {
        app->menu.text_size = scale;
        gui_app_layout(app);
    }
    theme_note(app);
    if (bad[0]) {
        size_t m = strlen(app->message);
        snprintf(app->message + m, sizeof app->message - m, "%sNot used from %s: %s", m ? " " : "", app->app_file, bad);
    }
}
/* a list that could not be kept: the message says so, after what it says already, until it is */
static void recent_note(App* app) {
    char note[sizeof app->message], *at;
    size_t n = strlen(app->message);
    if (!app->recent_file) return;
    snprintf(note, sizeof note, "Recent games not kept: %s could not be written.", app->recent_file);
    at = strstr(app->message, note);
    if (app->recent_unsaved && !at)
        snprintf(app->message + n, sizeof app->message - n, "%s%s", n ? " " : "", note);
    else if (!app->recent_unsaved && at) { /* with the space that set it apart */
        size_t cut = strlen(note);
        if (at > app->message)
            --at, ++cut;
        else if (at[cut] == ' ')
            ++cut;
        memmove(at, at + cut, strlen(at + cut) + 1);
    }
}
/* the list as the menu and the file have it */
static void recent_changed(App* app) {
    app->menu.recent = recent_shown(app) + recent_more(app);
    if (app->menu.recent_focus > app->menu.recent) app->menu.recent_focus = app->menu.recent;
    if (app->recent_file) app->recent_unsaved = gui_recent_save(&app->recent, app->recent_file) != 0;
    recent_note(app);
}
static void recent_open(App* app, char* file) {
    free(app->recent_file);
    app->recent_file = file;
    gui_recent_load(&app->recent, file);
    app->menu.recent = recent_shown(app) + recent_more(app);
    app->menu.recent_focus = 0;
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
void gui_app_layout(App* app) {
    int run;
    int width, height, whole_width, whole_height;
    gui_app_set_area(app);
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
                                          ? gui_app_games_page(app)
                                          : (size_t)SDL_max(1, app->rows - 10));
    int log_rows = SDL_max(1, app->rows - app->log_top - 4);
    if (boxed(app) && run && app->theme.run != GUI_RUN_STEPS) { /* the run's output box */
        GuiPlace p;
        int cols, rows;
        gui_app_run_place(app, &p);
        gui_app_run_console(app, &p, &cols, &rows);
        gui_console_resize(&app->console, cols, rows);
    } else if (!boxed(app))
        gui_console_resize(&app->console, SDL_max(1, app->cols - 2), log_rows);
}
/* the box with what older builds left: each file where it is (-) and where this one would keep it (->, a proposal, so
   not in a diff's added colour), whole over as many rows as it needs, as many files as fit, and OK. Nothing is moved:
   the player does that */
/* the question over a running run, in the run screen's colours, its choices as the menu's entries */

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
/* the editor on the theme the menu shows: a folder of themes/ keeps its file's extras and its name, a built-in is
   saved as a new theme */
static void open_editor(App* app) {
    GuiThemeFile file;
    size_t size = 0;
    char* text = gui_theme_text((const char* const*)app->themes_dirs, 2, app->theme.id, &size);
    int from_file = text && gui_theme_file_read(&file, text, size);
    free(text);
    gui_editor_start(&app->editor, &app->theme, from_file ? &file : NULL, from_file ? app->theme.id : "mytheme");
    gui_theme_old_keys(&app->editor.theme, app->editor.note, sizeof app->editor.note); /* Save writes the new keys */
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
const GuiThemeKey gui_app_form_keys[] = {
    {"", "Theme", GUI_KEY_ACTION, offsetof(GuiAppForm, theme), sizeof(((GuiAppForm*)0)->theme), NULL, 0,
     "Built-in themes, then each themes/<name>/theme.ini"},
    {"", "Text size", GUI_KEY_ACTION, offsetof(GuiAppForm, text), sizeof(((GuiAppForm*)0)->text), NULL, 0,
     "Bigger letters scroll the menu. Auto fits the screen"},
    {"", "Layout", GUI_KEY_ACTION, offsetof(GuiAppForm, layout), sizeof(((GuiAppForm*)0)->layout), NULL, 0,
     "Classic: lines down the page. Panels: the theme's panels side by side"},
    {"", "Theme editor", GUI_KEY_ACTION, 0, 0, NULL, 0,
     "This theme's keys with a preview, saved as themes/<name>/theme.ini"},
    {"", "Game folders", GUI_KEY_ACTION, offsetof(GuiAppForm, folders), sizeof(((GuiAppForm*)0)->folders), NULL, 0,
     "Folders of games listed with games/"}};
void gui_app_values(App* app) {
    GuiAppForm* f = &app->app_form;
    snprintf(f->theme, sizeof f->theme, "%s", app->theme.name);
    if (app->menu.text_size)
        snprintf(f->text, sizeof f->text, "%dx", app->menu.text_size);
    else
        snprintf(f->text, sizeof f->text, "Auto");
    snprintf(f->layout, sizeof f->layout, "%s", app->menu.panels ? "Panels" : "Classic");
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
void gui_app_settings_top(App* app, int row) {
    settings_groups(app);
    gui_editor_form(&app->editor, app->group_keys, app->groups, &app->app_form, "Reset all to the defaults");
    app->editor.flat = true;
    app->editor.row = SDL_min(row, gui_editor_rows(&app->editor) - 1);
    app->form = GUI_FORM_SETTINGS;
    app->editing = true;
}
static void open_app(App* app, int row) {
    gui_app_values(app);
    gui_editor_form(&app->editor, gui_app_form_keys, GUI_APP_ROWS, &app->app_form, NULL);
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
void gui_app_open_group(App* app, int g) {
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
/* a page's rows (settings.h), from its row in the group; the button map's, the pad drawn */
static void open_page(App* app, int row) {
    const GuiSetting* page = &app->settings.values[row];
    if (gui_app_pad_page(app, row)) {
        gui_app_open_pad(app, row);
        return;
    }
    gui_editor_form(&app->editor, app->settings.keys + page->first, page->count, app->settings.values, NULL);
    app->editor.flat = true;
    app->first = page->first;
    app->page = row;
}
static void open_settings(App* app) {
    app->group = 0;
    gui_app_settings_top(app, 0);
    space_start(app);
}
void gui_app_leave_editor(App* app) {
    gui_editor_stop(&app->editor);
    app->editing = false;
    SDL_StopTextInput(app->window);
}
/* ESC on a screen: a group's and the App's go back to the groups, the theme editor to where it was opened */
void gui_app_leave_form(App* app) {
    int form = app->form;
    gui_app_leave_editor(app);
    if (form == GUI_FORM_PAD) gui_app_pad_close(app);
    if ((form == GUI_FORM_GROUP || form == GUI_FORM_PAD) && app->page >= 0) { /* a page: back to its row */
        int row = app->page;
        gui_app_open_group(app, app->group);
        app->editor.row = row - app->first;
        app->editing = true;
    } else if (form == GUI_FORM_GROUP || form == GUI_FORM_APP)
        gui_app_settings_top(app, app->group);
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
    else if (row == GUI_APP_LAYOUT) /* the two either way round */
        app->menu.panels = !app->menu.panels;
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
    if (row == GUI_APP_THEME || row == GUI_APP_LAYOUT || row == GUI_APP_TEXT) app_save(app);
    gui_app_values(app);
}
/* saved in themes/ of the data folder, and the menu's theme from then on */
static void save_editor(App* app) {
    const char* const* dirs = (const char* const*)app->themes_dirs;
    if (gui_editor_save(&app->editor, app->themes_dirs[0])) return;
    gui_themes_scan(&app->themes, dirs, 2);
    gui_theme_load(&app->theme, dirs, 2, app->editor.id);
    app_save(app);
}
/* a Settings pick, kept at once */
void gui_app_keep_settings(App* app) {
    if ((app->form != GUI_FORM_GROUP && app->form != GUI_FORM_SETTINGS && app->form != GUI_FORM_PAD) ||
        !app->editor.dirty)
        return;
    app->editor.dirty = false;
    gui_settings_sync(&app->settings);
    if (app->settings_file && gui_settings_save(&app->settings, app->settings_file))
        snprintf(app->editor.note, sizeof app->editor.note, "Not saved: %s could not be written", app->settings_file);
}
void gui_app_editor_key(App* app, GuiKey key) {
    GuiEditorDo what = gui_editor_key(&app->editor, key);
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_SETTINGS && !app->editor.step) {
        gui_app_open_group(app, app->editor.row);
        return;
    }
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_GROUP) { /* a page's row opens it, an All steps its rows */
        int row = app->first + app->editor.row;
        if (app->settings.values[row].kind == GUI_SETTING_PAGE && !app->editor.step)
            open_page(app, row);
        else if (gui_settings_step_all(&app->settings, row, app->editor.step)) {
            app->editor.dirty = true;
            gui_app_keep_settings(app);
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
    gui_app_keep_settings(app);
    if (what == GUI_EDITOR_LEAVE) {
        gui_app_leave_form(app);
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
        gui_app_keep_settings(app);
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
    if (gui_app_menu_key_of(&e->key, &key)) gui_app_editor_key(app, key);
}
/* a click or a tap on a row focuses it, and on the focused one is Select; on the preview it switches the screen */
static void editor_pointer(App* app, float x, float y, bool click) {
    int col, row, cols, rows, r = app->editor.top + (int)y - 4;
    gui_app_preview_box(app, &col, &row, &cols, &rows);
    if (!click) return;
    if (app->form == GUI_FORM_THEME && x >= col && x < col + cols && y >= row && y < row + rows + 1) {
        app->editor.preview = app->editor.preview == GUI_SCREEN_MENU ? GUI_SCREEN_COMPILING : GUI_SCREEN_MENU;
        app->preview_since = 0;
        return;
    }
    if (y < 4 || r < 0 || r >= gui_editor_rows(&app->editor) || (int)y - 4 >= editor_rows(app)) return;
    if (r == app->editor.row && !app->editor.typing)
        gui_app_editor_key(app, GUI_SELECT);
    else if (r != app->editor.row) {
        if (app->editor.typing) gui_app_editor_key(app, GUI_CANCEL);
        app->editor.row = r;
    }
}
/* the game's name as its run gave it, from the step that names it, for its recent game */
static void recent_name(App* app, const char* label) {
    const GuiStep* named = gui_steps_last(&app->steps, label);
    if (!app->recent_file || app->recent_named || !app->file || !named || !named->detail[0]) return;
    app->recent_named = 1;
    if (gui_recent_rename(&app->recent, app->file, named->detail)) recent_changed(app);
}
/* back to the menu from a finished run, which fades in */
static void leave_run(App* app) {
    app->finished = false;
    app->menu.view = GUI_LOAD_NONE;
    count_games(app); /* a run may have made one, and taken disk */
    space_start(app);
    if (app->menu.active == GUI_GAMES && app->menu.games) app->menu.selected = GUI_GAMES;
    app->message[0] = 0;
    recent_note(app);
    app->leaving_ms = 0;
    app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
    gui_app_layout(app);
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
        gui_app_step_runs(&app->steps.step[app->steps.count - 1], app->theme.progress, now_ms(app), runs, lengths);
        snprintf(line, sizeof line, "%s%s%s", runs[0], runs[1], runs[2]);
    }
    if (app->told_ms && (now - app->told_ms < 1000 || (percent == app->told_percent && !strcmp(line, app->told))))
        return;
    gui_app_run_name(app, name, sizeof name);
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
            recent_note(app);
            app->aborting = false;
            app->confirm.shown = false; /* a run that ended while asking has nothing left to abort */
            gui_app_layout(app);
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
    gui_app_layout(app);
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
    gui_app_layout(app);
}
/* the screen a tab opens: the menu, Games or the Settings. A theme edited and not saved keeps its screen, the note
   saying why */
void gui_app_go_tab(App* app, int tab) {
    if (app->editing) {
        if (app->editor.typing || tab == GUI_TAB_SETTINGS) return;
        if (app->form == GUI_FORM_THEME && app->editor.dirty) {
            snprintf(app->editor.note, sizeof app->editor.note,
                     "Not saved: Ctrl+S saves it, ESC twice leaves without it");
            return;
        }
        gui_app_keep_settings(app);
        gui_app_leave_editor(app);
        app->theme_from_app = false;
    }
    if (tab == gui_app_tab_shown(app)) return;
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
    gui_app_go_tab(app, ((gui_app_tab_shown(app) + step) % GUI_TABS + GUI_TABS) % GUI_TABS);
}
void gui_app_menu_key(App* app, GuiKey key) {
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
        gui_app_layout(app);
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
void gui_app_confirmed(App* app) {
    if (app->confirm.exit)
        app->menu.quit = true;
    else
        abort_run(app);
}
/* the pointer on the question, at x, y in cells: on a choice it takes the focus, and a click takes the choice; a click
   off the box keeps going */
static void ask_pointer(App* app, float x, float y, bool click) {
    int col, row, cols;
    gui_app_ask_box(app, &col, &row, &cols);
    if (x >= (float)col && x < (float)(col + cols) && y >= (float)(row + 3) && y < (float)(row + 5)) {
        app->confirm.selected = (int)y - row - 3;
        if (click && gui_confirm_key(&app->confirm, GUI_SELECT)) gui_app_confirmed(app);
    } else if (click && (x < (float)col || x >= (float)(col + cols) || y < (float)row || y >= (float)(row + 6)))
        gui_confirm_key(&app->confirm, GUI_CANCEL);
}
static void pointer(App* app, float window_x, float window_y, bool click, bool touch) {
    if ((app->menu.running && !app->confirm.shown) || app->finished) return;
    gui_app_layout(app);
    float x, y;
    if (!SDL_RenderCoordinatesFromWindow(app->renderer, window_x, window_y, &x, &y)) return;
    if (app->editing && app->form == GUI_FORM_PAD) {
        if (click) gui_app_pad_did(app, snx_pad_screen_tap(&app->pad, snx_pad_screen_hit(&app->pad_drawing, x, y)));
        return;
    }
    if (app->editing && app->editor.picking) {
        gui_app_picker_pointer(app, x, y, click);
        return;
    }
    if (boxed(app) && app->editing) {
        if (click) gui_app_form_boxed_pointer(app, x, y, touch);
        return;
    }
    if (boxed(app) && app->confirm.shown) {
        gui_app_ask_boxed_pointer(app, x, y, click);
        return;
    }
    if (boxed(app)) {
        gui_app_boxed_pointer(app, x, y, click, touch);
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
            gui_app_menu_key(app, GUI_CANCEL);
        else if (click && hit_text(app, x, y, 1, SDL_max(app->rows / 2, 10), SAY_KEYS_WAIT, true))
            gui_app_menu_key(app, GUI_SELECT); /* its whole row: the theme may center or frame the prompt */
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
    if (click) gui_app_menu_key(app, GUI_SELECT);
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
        gui_app_menu_key(app, GUI_CANCEL);
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
        gui_app_pad_did(app, snx_pad_screen_key(&app->pad, key == GUI_UP ? SNX_PAD_KEY_UP : SNX_PAD_KEY_DOWN));
        return;
    }
    if (app->editing) {
        gui_app_editor_key(app, key); /* stops at its ends */
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        if (key == GUI_UP ? app->browser.selected > 0 : app->browser.selected < gui_browser_items(&app->browser))
            gui_app_menu_key(app, key);
        return;
    }
    row = menu_focus_row(app);
    gui_app_menu_key(app, key);
    if (key == GUI_UP ? menu_focus_row(app) > row : menu_focus_row(app) < row)
        gui_app_menu_key(app, key == GUI_UP ? GUI_DOWN : GUI_UP); /* it went round: back */
}
/* a drag up or down from where the finger went down, on a menu whose letters make it scroll */
static bool drags(App* app, float x, float y) {
    float dx = x - app->finger_x, dy = y - app->finger_y;
    return SDL_fabsf(dy) > 0.05f && SDL_fabsf(dy) > 2 * SDL_fabsf(dx) && app->menu.view != GUI_LOAD_GAMES &&
           !app->menu.running && !app->finished &&
           (app->editing ? app->form != GUI_FORM_PAD : gui_app_menu_scrolls(app));
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
            gui_app_menu_key(app, dx < 0 ? GUI_PAGE_DOWN : GUI_PAGE_UP);
        return;
    }
    if (drags(app, x, y)) {
        drag(app, y); /* what its last moves did not reach */
        return;
    }
    if (SDL_fabsf(dx) > 0.03f || SDL_fabsf(dy) > 0.03f) return; /* a drag, no tap */
    if (app->finished) {
        gui_app_menu_key(app, GUI_CANCEL);
        return;
    }
    SDL_GetWindowSize(app->window, &w, &h);
    if (app->confirm.shown) {
        pointer(app, x * (float)w, y * (float)h, true, true);
        return;
    }
    if (app->menu.running) {
        GuiPlace p;
        gui_app_layout(app);
        if (!SDL_RenderCoordinatesFromWindow(app->renderer, x * (float)w, y * (float)h, &rx, &ry)) return;
        if (boxed(app) && app->theme.run != GUI_RUN_STEPS) { /* the bar the ESC line is in */
            gui_app_run_place(app, &p);
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
bool gui_app_menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key) {
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
    if (app->editing && app->form == GUI_FORM_PAD && gui_app_pad_event(app, e)) return;
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
        int w, h;
        app->finger = finger_id(&e->tfinger);
        app->finger_x = e->tfinger.x;
        app->finger_y = app->finger_mark = e->tfinger.y;
        SDL_GetWindowSize(app->window, &w, &h);
        app->pick_press = 0;                     /* the finger's own, not a mouse's whose lift was missed */
        if (app->editing && app->editor.picking) /* the picker's bars take a finger as it goes down, to drag */
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, true, true);
        return;
    }
    if (e->type == SDL_EVENT_FINGER_MOTION && finger_id(&e->tfinger) == app->finger) {
        int w, h;
        SDL_GetWindowSize(app->window, &w, &h);
        if (app->pick_press)
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, false, true);
        else if (drags(app, e->tfinger.x, e->tfinger.y))
            drag(app, e->tfinger.y);
        return;
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        finger_id(&e->tfinger) == app->finger && app->pick_press) { /* the picker had it: no tap */
        app->finger = 0;
        app->pick_press = 0;
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
        if (app->splash || app->menu.running || app->finished || app->confirm.shown || app->left_shown ||
            (app->editing && app->editor.picking))
            return;
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
    if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && e->button.button == SDL_BUTTON_LEFT) {
        app->pick_press = 0; /* a drag on the picker's bar ends */
        return;
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return;
    /* a running run takes ESC, and the question, while it shows, every key; a held key asks and answers nothing */
    if (app->menu.running) {
        if (e->key.repeat || !gui_app_menu_key_of(&e->key, &key)) return;
        if (app->confirm.shown) {
            if (gui_confirm_key(&app->confirm, key)) gui_app_confirmed(app);
        } else if (key == GUI_CANCEL)
            cancel(app);
        return;
    }
    /* the exit question takes every key while it shows */
    if (app->confirm.shown) {
        if (!e->key.repeat && gui_app_menu_key_of(&e->key, &key) && gui_confirm_key(&app->confirm, key))
            gui_app_confirmed(app);
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
    if (gui_app_menu_key_of(&e->key, &key)) gui_app_menu_key(app, key);
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
    app->font = gui_app_create_font(app->renderer);
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
    gui_app_layout(app);
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
    free(app->app_file);
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
        app.app_file = config ? snx_path_join(config, "app.ini") : NULL;
        free(config);
        folders_load(&app);
        app_load(&app);
    }
    count_games(&app);
    if (app.menu.games) app.menu.selected = GUI_GAMES; /* the first item */
    space_start(&app);                                 /* the footer's disk */
    free(prefs);
    left_behind(&app);
    app.left_shown = app.lefts > 0;
    gui_app_splash_start(&app);
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
            gui_app_render(&app);
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
