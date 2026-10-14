/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the actual renderer and SDL event handlers without production test flags. */
#define SNX_GUI_NO_MAIN
#include "../src/gui/main.c"
#include <assert.h>

static void key(App* app, SDL_Keycode code) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    assert(SDL_PushEvent(&e));
    while (SDL_PollEvent(&e)) event(app, &e);
}
static void red_text(App* app, int row, const char* prompt) {
    /* Verify the exact text and its bright red glyphs in the real renderer. */
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    assert(image);
    for (int i = 0; prompt[i]; ++i)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                Uint8 r, g, b, a;
                assert(SDL_ReadSurfacePixel(image, ((i + 1) * 8 + x) * app->scale, (row * 16 + y) * app->scale, &r, &g,
                                            &b, &a));
                bool ink = (gui_vga_font[(unsigned char)prompt[i] * 16 + y] & (0x80 >> x)) != 0;
                assert(r == (ink ? 255 : 0) && g == 0 && b == 0);
            }
    SDL_DestroySurface(image);
}
static void finish(App* app, int status) {
    assert(app->menu.running && !app->finished && !app->aborting && !app->abnormal_exit);
    int selected = app->menu.selected;
    Uint64 until = SDL_GetTicks() + 10000;
    while (app->menu.running && SDL_GetTicks() < until) {
        pump(app);
        render(app);
        SDL_Delay(5);
    }
    assert(!app->menu.running && app->finished && app->menu.view == GUI_LOAD_NONE);
    char expected[80];
    snprintf(expected, sizeof(expected),
             status ? "Program ended abnormally (status %d)." : "Driver exited with status %d.", status);
    assert(!strcmp(app->message, expected) && app->abnormal_exit == (status != 0));
    assert(app->has_output && app->log_top == 4);
    red_text(app, 3, "Press [ESC] to return to menu");
    if (status) red_text(app, 1, expected);
    for (int i = 0; i < 3; ++i) pump(app);
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    key(app, SDLK_SPACE);
    assert(!app->menu.running && app->finished && app->menu.selected == selected);
    key(app, SDLK_ESCAPE);
    assert(!app->menu.running && !app->finished && app->menu.view == GUI_LOAD_NONE);
    assert(!app->menu.quit && !app->message[0] && app->has_output);
    assert(app->menu.selected == selected && app->menu.fullscreen);
    render(app);
    assert(app->log_top == 16);
}
static int SDLCALL hang(void* unused) {
    (void)unused;
    for (;;) SDL_Delay(100);
}
int main(int argc, char** argv) {
    if (argc > 1 && strstr(argv[1], "hang in join")) {
        SDL_Thread* thread = SDL_CreateThread(hang, "hung worker", NULL);
        assert(thread && !snx_write_file(argv[1], "ready", 5));
        SDL_WaitThread(thread, NULL); /* Never returns. ESC must kill, not request a graceful exit. */
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--fullscreen")) {
        puts("prepared game stdout");
        fputs("prepared game stderr\n", stderr);
        return 0;
    }
    if (argc > 1) {
        assert(argc == 2 || argc == 3);
        assert(snx_is_file(argv[1]));
        if (strstr(argv[1], "Game00")) {
            assert(argc == 2 && strstr(argv[0], "gnuabi"));
            puts("Linux file selected from games/");
            return 0;
        }
        assert(strstr(argv[1], "game with spaces"));
        bool nx = strstr(argv[0], "mariko") != NULL;
        if (nx)
            assert(argc == 3 && !strcmp(argv[2], "--fullscreen"));
        else
            assert(argc == 2);
        size_t size;
        unsigned char* data = snx_read_file(argv[1], &size);
        assert(data && size == 1);
        ++data[0];
        assert(!snx_write_file(argv[1], data, 1));
        free(data);
        puts("driver stdout");
        fputs("driver stderr\n", stderr);
        return nx ? 0 : 7; /* Both successful and failed drivers need ESC acknowledgement. */
    }
    App app;
    assert(initialize(&app));
    char* temporary = snx_temp_dir(NULL);
    char* self = snx_executable(argv[0]);
    char* input = snx_path_join(temporary, "game with spaces;literal.nro");
    assert(!snx_write_file(input, "0", 1));
    const char* names[] = {"scarletnx-mariko", "scarletnx-gnuabi"};
    for (int i = 0; i < 2; ++i) {
#ifdef _WIN32
        char* filename = snx_format("%s.exe", names[i]);
#else
        char* filename = snx_strdup(names[i]);
#endif
        char* path = snx_path_join(temporary, filename);
        assert(!snx_copy_file(self, path) && !snx_make_executable(path));
        free(filename);
        free(path);
    }
    app.drivers = temporary;
    free(app.games_dir);
    app.games_dir = snx_path_join(temporary, "games");
    render(&app);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_FULLSCREEN);
    key(&app, SDLK_SPACE);
    assert(app.menu.fullscreen);
    key(&app, SDLK_UP);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_RETURN);
    assert(app.menu.selected == GUI_SWITCH && app.menu.view == GUI_LOAD_OPTIONS);
    pump(&app);
    render(&app);
    const char* screenshot = getenv("SNX_GUI_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_RETURN); /* activate Please drag and drop */
    SDL_Event e = {0};
    e.type = SDL_EVENT_DROP_TEXT;
    e.drop.data = "one\ntwo";
    event(&app, &e);
    assert(app.menu.view == GUI_LOAD_DROP && !app.menu.running);
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.data = input;
    event(&app, &e);
    assert(app.menu.running);
    finish(&app, 0);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_SPACE);
    assert(app.menu.selected == GUI_LINUX && app.menu.view == GUI_LOAD_OPTIONS);
    key(&app, SDLK_RETURN);
    char* quoted = snx_format("\"%s\"", input);
    e.type = SDL_EVENT_DROP_TEXT;
    e.drop.data = quoted;
    event(&app, &e);
    free(quoted);
    assert(app.menu.running);
    finish(&app, 7);
    size_t size;
    unsigned char* runs = snx_read_file(input, &size);
    assert(runs && size == 1 && runs[0] == '2');
    free(runs);

    char* hung = snx_path_join(temporary, "hang in join.elf");
    assert(!snx_write_file(hung, "", 0));
    launch(&app, snx_strdup(hung), false);
    Uint64 until = SDL_GetTicks() + 5000;
    bool ready = false;
    while (SDL_GetTicks() < until) {
        unsigned char* data = snx_read_file(hung, &size);
        ready = data && size == 5 && !memcmp(data, "ready", 5);
        free(data);
        pump(&app);
        if (ready) break;
        SDL_Delay(5);
    }
    assert(ready && app.menu.running);
    render(&app);
    red_text(&app, 3, "Press [ESC] to abort and return to menu");
    int selected = app.menu.selected;
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.menu.running && app.menu.selected == selected);
    Uint64 started = SDL_GetTicks();
    key(&app, SDLK_ESCAPE);
    assert(app.aborting && SDL_GetTicks() - started < 1000);
    key(&app, SDLK_ESCAPE); /* Repeated ESC while reaping is harmless. */
    while (app.menu.running && SDL_GetTicks() - started < 2000) {
        pump(&app);
        SDL_Delay(5);
    }
    assert(!app.menu.running && !app.finished && !app.aborting && !app.menu.quit);
    assert(app.menu.view == GUI_LOAD_NONE && app.abnormal_exit);
    assert(!strcmp(app.message, "Program ended abnormally (force-terminated by ESC)."));
    render(&app);
    red_text(&app, app.log_top - 4, app.message);
    free(hung);

    /* Browser: missing/empty directories, filtering, page controls and both
       launch paths. Reopening refreshes the directory rather than caching it. */
    key(&app, SDLK_UP);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 0 && app.message[0]);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_OPTIONS && app.menu.load_selected == GUI_LOAD_DIRECTORY);
    assert(!snx_mkdirs(app.games_dir));
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 0 && !app.message[0]);
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 0 && app.browser.selected == 0);
    key(&app, SDLK_RETURN); /* Back [ESC] is a selectable item too */
    assert(app.menu.view == GUI_LOAD_OPTIONS);
    for (int i = 0; i < 35; ++i) {
        char name[40];
#ifdef _WIN32
        snprintf(name, sizeof(name), "Game%02d.exe", i);
#else
        snprintf(name, sizeof(name), "Game%02d", i);
#endif
        char* path = snx_path_join(app.games_dir, name);
        assert(!(i ? snx_write_file(path, "fixture", 7) : snx_copy_file(self, path)));
        assert(!snx_make_executable(path));
        free(path);
    }
    const char* ignored[] = {"Game_code.so", "Game_code.dll", ".hidden.exe", "notes.ini"};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        char* path = snx_path_join(app.games_dir, ignored[i]);
        assert(!snx_write_file(path, "fixture", 7));
        if (i < 3) assert(!snx_make_executable(path));
        free(path);
    }
    char* directory = snx_path_join(app.games_dir, "directory.exe");
    assert(!snx_mkdirs(directory));
    free(directory);
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 35 && !app.message[0]);
    assert(!strncmp(app.games.names[0], "Game00", 6));
    assert(gui_browser_pages(&app.browser) > 1);
    key(&app, SDLK_RIGHT);
    key(&app, SDLK_DOWN);
    assert(app.browser.page == 1 && app.browser.selected == 1);
    render(&app);
    screenshot = getenv("SNX_GUI_BROWSER_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 2);
    key(&app, SDLK_PAGEUP);
    key(&app, SDLK_LEFT);
    assert(app.browser.page == 0 && app.browser.selected == 1);
    key(&app, SDLK_RETURN); /* native prepared game, not another extraction */
    assert(app.menu.running);
    finish(&app, 0);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN); /* Linux selection still uses the GNUABI driver */
    assert(app.menu.running);
    finish(&app, 0);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    char* removed = snx_path_join(app.games_dir, app.games.names[0]);
    assert(!snx_remove_tree(removed));
    free(removed);
    key(&app, SDLK_RETURN);
    assert(!app.menu.running && app.menu.view == GUI_LOAD_GAMES && app.message[0]);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_RETURN);
    assert(app.games.count == 34);
    e.type = SDL_EVENT_QUIT;
    event(&app, &e);
    assert(app.menu.quit);
    cleanup(&app);
    assert(!snx_remove_tree(temporary));
    free(temporary);
    free(self);
    free(input);
    puts("SDL3 GUI: keyboard, font rendering, drops, dispatch and return-to-menu passed");
    return 0;
}
