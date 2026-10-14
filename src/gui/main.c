/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <SDL3/SDL.h>
#ifndef SNX_GUI_NO_MAIN
#include <SDL3/SDL_main.h>
#endif
#include "common.h"
#include "browser.h"
#include "font.h"
#include "model.h"
#include "gui_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char* drivers;
    char *cwd, *file, *games_dir;
    char message[512];
    int cols, rows, scale, log_top;
    Uint64 sampled;
    bool has_output, finished, aborting, abnormal_exit;
} App;
static const char* const items[] = {"Load Switch NSP/XCI/NRO", "Load Linux ARM ELF", "Fullscreen", "Exit"};
static const SDL_Color normal = {220, 220, 220, 255}, yellow = {255, 255, 0, 255}, muted = {160, 170, 180, 255},
                       red = {255, 0, 0, 255};

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
static void glyph(App* app, int x, int y, unsigned char c) {
    SDL_FRect source = {(float)((c % 16) * 8), (float)((c / 16) * 16), 8, 16};
    SDL_FRect destination = {(float)(x * 8 * app->scale), (float)(y * 16 * app->scale), (float)(8 * app->scale),
                             (float)(16 * app->scale)};
    SDL_RenderTexture(app->renderer, app->font, &source, &destination);
}
static void text(App* app, int x, int y, const char* value, SDL_Color color) {
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    while (*value && x < app->cols - 1) {
        Uint32 codepoint = SDL_StepUTF8(&value, NULL);
        glyph(app, x++, y, gui_glyph(codepoint));
    }
}
static void layout(App* app) {
    int width, height;
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    app->scale = SDL_min(width / 640, height / 400);
    if (app->scale < 1) app->scale = 1;
    app->cols = SDL_min(width / (8 * app->scale), GUI_MAX_COLS);
    app->rows = SDL_min(height / (16 * app->scale), GUI_MAX_ROWS);
    app->log_top = app->menu.running || app->finished ? 4 : app->menu.view != GUI_LOAD_NONE ? 18 : 16;
    gui_browser_resize(&app->browser, (size_t)SDL_max(1, app->rows - 10));
    int log_rows = SDL_max(1, app->rows - app->log_top - 4);
    gui_console_resize(&app->console, SDL_max(1, app->cols - 2), log_rows);
}
static void render(App* app) {
    layout(app);
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255); /* Always OLED black */
    SDL_RenderClear(app->renderer);
    text(app, 1, 0, "ScarletNX", yellow);
    if (app->menu.view == GUI_LOAD_GAMES && !app->menu.running && !app->finished) {
        text(app, 1, 1, "Up/Down: select. Enter/Space: open. Left/Right or PgUp/PgDown: page.", normal);
        text(app, 1, 2, app->games_dir, muted);
        text(app, 1, 3, "Back [ESC]", app->browser.selected == 0 ? yellow : normal);
        size_t start = app->browser.page * app->browser.page_size;
        size_t count = gui_browser_items(&app->browser);
        for (size_t i = 0; i < count; ++i) {
            bool is_hovered = app->browser.selected == i + 1;
            text(app, 1, 5 + (int)i, app->games.names[start + i], is_hovered ? yellow : normal);
        }
        if (!app->games.count) text(app, 1, 5, "No executables found in games/.", muted);
        char page[128];
        snprintf(page, sizeof(page), "Page %zu/%zu   %zu executable(s)", app->browser.page + 1,
                 gui_browser_pages(&app->browser), app->games.count);
        text(app, 1, app->rows - 5, page, muted);
        text(app, 1, app->rows - 4, app->message, yellow);
    } else if (!app->menu.running && !app->finished) {
        text(app, 1, 1, "Arrows: move (Up/Down/Left/Right). Space/Enter: select.", normal);
        text(app, 1, 2,
             app->menu.view == GUI_LOAD_DROP ? "Drop a file now. Ctrl+V: paste a path. ESC: back."
                                             : "Choose a load option. ESC: back.",
             muted);
        for (int i = 0; i < GUI_ITEM_COUNT; ++i) {
            bool is_hovered = app->menu.selected == i;
            int row = 4 + i * 2;
            if (app->menu.view != GUI_LOAD_NONE && i > app->menu.selected) row += 2;
            char label[100];
            if (i == GUI_FULLSCREEN)
                snprintf(label, sizeof(label), "- [%s] %s", app->menu.fullscreen ? "ON" : "OFF", items[i]);
            else
                snprintf(label, sizeof(label), "- %s", items[i]);
            text(app, 1, row, label, is_hovered ? yellow : normal);
            if (is_hovered && app->menu.view != GUI_LOAD_NONE) {
                text(app, 3, row + 1, "-> Select via drag and drop",
                     app->menu.load_selected == GUI_LOAD_DRAG ? yellow : normal);
                text(app, 3, row + 2, "-> Select file from games/ instead",
                     app->menu.load_selected == GUI_LOAD_DIRECTORY ? yellow : normal);
            }
        }
        text(app, 1, app->log_top - 4, app->message, app->abnormal_exit ? red : yellow);
        if (app->has_output) text(app, 1, app->log_top - 2, "Last output (no scrollback):", muted);
    } else {
        text(app, 1, 1, app->finished ? app->message : items[app->menu.selected], app->abnormal_exit ? red : yellow);
        text(app, 1, 2, app->file ? app->file : "", normal);
        if (app->finished)
            text(app, 1, 3, "Press [ESC] to return to menu", red);
        else
            text(app, 1, 3, app->aborting ? "Aborting process tree..." : "Press [ESC] to abort and return to menu",
                 red);
    }
    if (app->has_output && app->menu.view != GUI_LOAD_GAMES) {
        SDL_SetTextureColorMod(app->font, normal.r, normal.g, normal.b);
        for (int y = 0; y < app->console.rows; ++y)
            for (int x = 0; x < app->console.cols; ++x)
                if (app->log_top + y < app->rows - 4 && app->console.cells[y][x] != ' ')
                    glyph(app, x + 1, app->log_top + y, app->console.cells[y][x]);
    }
    char footer[256];
    if (app->usage.available)
        snprintf(footer, sizeof(footer), "GUI + children: RAM %.1f MiB (sum RSS)  CPU %.1f%% (100%% = 1 core)",
                 (double)app->usage.memory_bytes / 1048576, app->usage.cpu_percent);
    else
        snprintf(footer, sizeof(footer), "GUI + children: resource usage unavailable");
    text(app, 1, app->rows - 3, footer, muted);
    text(app, 1, app->rows - 2, "CWD:", muted);
    text(app, 6, app->rows - 2, app->cwd ? app->cwd : "(unavailable)", normal);
    SDL_RenderPresent(app->renderer);
}
static void pump(App* app) {
    if (app->menu.running) {
        char buffer[4096];
        /* Bounded work per frame keeps keyboard/window events responsive even
           when a compiler floods the inherited pipe. There is no log backlog. */
        for (int reads = 0; reads < 16; ++reads) {
            int n = gui_process_read(app->process, buffer, sizeof(buffer));
            if (n <= 0) break;
            gui_console_feed(&app->console, buffer, (size_t)n);
            app->has_output = true;
        }
        int status;
        if (gui_process_finished(app->process, &status)) {
            app->menu.running = false;
            /* Normal exits wait for acknowledgement. ESC already acknowledged
               an abort, so return straight to the menu after nonblocking reaping. */
            app->finished = !app->aborting;
            app->abnormal_exit = app->aborting || status != 0;
            app->menu.view = GUI_LOAD_NONE;
            if (app->aborting)
                snprintf(app->message, sizeof(app->message), "Program ended abnormally (force-terminated by ESC).");
            else if (app->abnormal_exit)
                snprintf(app->message, sizeof(app->message), "Program ended abnormally (status %d).", status);
            else
                snprintf(app->message, sizeof(app->message), "Driver exited with status %d.", status);
            app->aborting = false;
            layout(app);
        }
    }
    Uint64 now = SDL_GetTicks();
    if (!app->sampled || now - app->sampled >= 500) {
        app->usage = gui_process_usage(app->process);
        app->sampled = now;
    }
}
static char* find_driver(App* app) {
    const char* name = app->menu.selected == GUI_SWITCH ? "scarletnx-mariko" : "scarletnx-gnuabi";
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
static void launch(App* app, char* absolute, bool prepared) {
    if (!absolute || !snx_is_file(absolute)) {
        snprintf(app->message, sizeof(app->message), "That path is not a readable file. Try again or press ESC.");
        free(absolute);
        return;
    }
    bool native = prepared && app->menu.selected == GUI_SWITCH;
    char* driver = native ? NULL : find_driver(app);
    if (!native && !driver) {
        snprintf(app->message, sizeof(app->message), "Cannot find %s beside the GUI or on PATH.",
                 app->menu.selected == GUI_SWITCH ? "scarletnx-mariko" : "scarletnx-gnuabi");
        free(absolute);
        return;
    }
    const char* argv[4] = {native ? absolute : driver, native ? NULL : absolute, NULL, NULL};
    /* GNUABI is a CPU-only ELF driver. Fullscreen is a Switch runtime option,
       never a Linux guest argv entry and never a change to the GUI window. */
    if (app->menu.selected == GUI_SWITCH)
        argv[native ? 1 : 2] = app->menu.fullscreen ? "--fullscreen" : "--fullscreen=0";
    bool started = gui_process_start(app->process, argv, app->message, sizeof(app->message));
    free(driver);
    if (!started) {
        free(absolute);
        return;
    }
    free(app->file);
    app->file = absolute;
    app->menu.view = GUI_LOAD_NONE;
    app->menu.running = true;
    app->finished = false;
    app->abnormal_exit = false;
    app->message[0] = 0;
    gui_console_init(&app->console, app->cols - 2, app->rows - 8);
    app->has_output = true;
    layout(app);
}
static void drop(App* app, const char* value, bool text_drop) {
    if (app->menu.view != GUI_LOAD_DROP || app->menu.running || app->finished) return;
    char* path = gui_drop_path(value, text_drop);
    if (!path) {
        snprintf(app->message, sizeof(app->message), "Drop one local file path, not a list or remote URL.");
        return;
    }
    char* absolute = snx_absolute(path);
    free(path);
    launch(app, absolute, false);
}
static void menu_key(App* app, GuiKey key) {
    if (app->finished) {
        if (key == GUI_CANCEL) {
            app->finished = false;
            app->menu.view = GUI_LOAD_NONE;
            app->message[0] = 0;
            layout(app);
        }
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        /* Resizes and key events can arrive in the same SDL poll batch. */
        layout(app);
        GuiBrowserAction action = gui_browser_key(&app->browser, key);
        if (action == GUI_BROWSER_BACK) {
            app->menu.view = GUI_LOAD_OPTIONS;
            app->message[0] = 0;
        } else if (action == GUI_BROWSER_OPEN) {
            size_t index = app->browser.page * app->browser.page_size + app->browser.selected - 1;
            if (index < app->games.count) {
                char* path = snx_path_join(app->games_dir, app->games.names[index]);
                char* absolute = snx_absolute(path);
                free(path);
                launch(app, absolute, true);
            }
        }
        return;
    }
    gui_menu_key(&app->menu, key);
    if (key == GUI_CANCEL) app->message[0] = 0;
    if (app->menu.view == GUI_LOAD_GAMES) {
        gui_games_scan(&app->games, app->games_dir, app->message, sizeof(app->message));
        app->browser = (GuiBrowser){.count = app->games.count};
        layout(app);
    }
}
static void event(App* app, const SDL_Event* e) {
    if (e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        app->menu.quit = true;
        return;
    }
    if (e->type == SDL_EVENT_DROP_FILE || e->type == SDL_EVENT_DROP_TEXT) {
        drop(app, e->drop.data, e->type == SDL_EVENT_DROP_TEXT);
        return; /* SDL3 owns the event's data, unlike SDL2. */
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return;
    if (app->menu.running) {
        if (e->key.key == SDLK_ESCAPE && !app->aborting) {
            /* Do not ask a hung program to shut down or wait on it in the event
               handler. Kill its tree now and let pump reap it without blocking. */
            gui_process_terminate(app->process);
            app->aborting = true;
        }
        return;
    }
    if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) && app->menu.view == GUI_LOAD_DROP && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        drop(app, clipboard, true);
        SDL_free(clipboard);
        return;
    }
    switch (e->key.key) {
        case SDLK_UP: menu_key(app, GUI_UP); break;
        case SDLK_DOWN: menu_key(app, GUI_DOWN); break;
        case SDLK_LEFT: menu_key(app, GUI_LEFT); break;
        case SDLK_RIGHT: menu_key(app, GUI_RIGHT); break;
        case SDLK_PAGEUP: menu_key(app, GUI_PAGE_UP); break;
        case SDLK_PAGEDOWN: menu_key(app, GUI_PAGE_DOWN); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (!e->key.repeat) menu_key(app, GUI_SELECT);
            break;
        case SDLK_ESCAPE: menu_key(app, GUI_CANCEL); break;
        default: break;
    }
}
static bool initialize(App* app) {
    memset(app, 0, sizeof(*app));
    if (!SDL_Init(SDL_INIT_VIDEO)) return false;
    if (!SDL_CreateWindowAndRenderer("ScarletNX", 1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &app->window, &app->renderer))
        return false;
    SDL_SetWindowMinimumSize(app->window, 800, 480);
    SDL_SetRenderVSync(app->renderer, 1);
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, true);
    app->font = create_font(app->renderer);
    app->process = gui_process_create();
    app->cwd = SDL_GetCurrentDirectory();
    app->games_dir = snx_path_join(app->cwd ? app->cwd : ".", "games");
    app->drivers = SDL_GetBasePath();
    gui_console_init(&app->console, 78, 17);
    if (!app->font || !app->process || !app->drivers) {
        SDL_SetError("Cannot initialize GUI resources");
        return false;
    }
    layout(app);
    return true;
}
static void cleanup(App* app) {
    gui_process_destroy(app->process);
    free(app->file);
    free(app->games_dir);
    gui_games_free(&app->games);
    SDL_free(app->cwd);
    SDL_DestroyTexture(app->font);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
}
#ifndef SNX_GUI_NO_MAIN
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    App app;
    if (!initialize(&app)) {
        fprintf(stderr, "scarletnx-gui: %s\n", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ScarletNX", SDL_GetError(), app.window);
        cleanup(&app);
        return 1;
    }
    while (!app.menu.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) event(&app, &e);
        pump(&app);
        render(&app);
        SDL_Delay(16);
    }
    cleanup(&app);
    return 0;
}
#endif
