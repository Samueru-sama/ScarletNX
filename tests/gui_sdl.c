/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the actual renderer and SDL event handlers without production test flags. */
#define SNX_GUI_NO_MAIN
#include "../src/gui/main.c"
#include <assert.h>
/* the default theme's colours, as the checks below name them */
static const SDL_Color normal = {220, 220, 220, 255}, yellow = {255, 255, 0, 255}, red = {255, 0, 0, 255},
                       pink = {255, 85, 255, 255}, black = {0, 0, 0, 255};

static void key(App* app, SDL_Keycode code) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    assert(SDL_PushEvent(&e));
    while (SDL_PollEvent(&e)) event(app, &e);
}
static void mouse(App* app, int col, int row, bool click) {
    layout(app);
    SDL_Event e = {0};
    float x, y;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (col + 0.5f) * 8 * app->scale, (row + 0.5f) * 16 * app->scale,
                                         &x, &y));
    if (click) {
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x;
        e.button.y = y;
    } else {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.x = x;
        e.motion.y = y;
    }
    event(app, &e);
}
/* a finger down on one cell and lifted on another, as SDL gives it: 0 to 1 across the window */
static void swipe(App* app, int col, int row, int to_col, int to_row) {
    SDL_Event e = {0};
    float x, y;
    int w, h;
    layout(app);
    SDL_GetWindowSize(app->window, &w, &h);
    e.tfinger.fingerID = 7;
    e.type = SDL_EVENT_FINGER_DOWN;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (col + 0.5f) * 8 * app->scale, (row + 0.5f) * 16 * app->scale,
                                         &x, &y));
    e.tfinger.x = x / (float)w;
    e.tfinger.y = y / (float)h;
    event(app, &e);
    e.type = SDL_EVENT_FINGER_UP;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (to_col + 0.5f) * 8 * app->scale,
                                         (to_row + 0.5f) * 16 * app->scale, &x, &y));
    e.tfinger.x = x / (float)w;
    e.tfinger.y = y / (float)h;
    event(app, &e);
}
static void tap(App* app, int col, int row) { swipe(app, col, row, col, row); }
/* a key with a modifier held, and text as a keyboard types it */
static void key_with(App* app, SDL_Keycode code, SDL_Keymod mod) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    e.key.mod = mod;
    event(app, &e);
}
static void typed(App* app, const char* text) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = text;
    event(app, &e);
}
/* what the system's picker answers, from its own thread */
static void pick(App* app, const char* uri) {
    SDL_Event e = {0};
    e.type = app->picked;
    e.user.data1 = uri ? SDL_strdup(uri) : NULL;
    assert(SDL_PushEvent(&e));
    while (SDL_PollEvent(&e)) event(app, &e);
}
static void colored_text_on(App* app, int col, int row, const char* prompt, SDL_Color color, SDL_Color back) {
    /* Verify the exact text, its glyph color and what is behind it in the real renderer. */
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    assert(image);
    for (int i = 0; prompt[i]; ++i)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                Uint8 r, g, b, a;
                assert(SDL_ReadSurfacePixel(image, ((i + col) * 8 + x) * app->scale, (row * 16 + y) * app->scale, &r,
                                            &g, &b, &a));
                bool ink = (gui_vga_font[(unsigned char)prompt[i] * 16 + y] & (0x80 >> x)) != 0;
                SDL_Color want = ink ? color : back;
                assert(r == want.r && g == want.g && b == want.b);
            }
    SDL_DestroySurface(image);
}
static SDL_Color pixel(App* app, float x, float y) {
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    Uint8 r = 0, g = 0, b = 0, a = 0; /* mingw's assert returns, as far as the compiler knows */
    SDL_Color out;
    assert(image && SDL_ReadSurfacePixel(image, (int)x, (int)y, &r, &g, &b, &a));
    SDL_DestroySurface(image);
    out.r = r;
    out.g = g;
    out.b = b;
    out.a = 255;
    return out;
}
static int same_colour(SDL_Color a, SDL_Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
/* the checks are the lines layout's ([body] boxes none), but for those of the boxes, which say so */
static void lines(App* app, const char* id) {
    if (id) app->theme = *gui_theme_builtin(id);
    app->theme.boxes = GUI_BOXES_NONE;
}
/* a list's margin: an arrow up on row first or none, and one down on row last or none */
static void marks(App* app, int first, int last, bool up, bool down) {
    SDL_Color soft = rgb(app->theme.soft), page = rgb(app->theme.page);
    colored_text_on(app, 0, first, up ? "\x1e" : " ", soft, page);
    colored_text_on(app, 0, last, down ? "\x1f" : " ", soft, page);
}
#ifdef SNX_XDG /* the old-files box's check: only an XDG system has older files to move */
/* text drawn from pixel x, y in the font's own bytes: its glyphs in color, around them back but on the font's last row,
   empty, which a box's frame may cover */
static void text_px_is(App* app, int x0, int y0, const char* text, SDL_Color color, SDL_Color back) {
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    assert(image);
    for (int i = 0; text[i]; ++i)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                Uint8 r, g, b, a;
                assert(SDL_ReadSurfacePixel(image, x0 + (i * 8 + x) * app->scale, y0 + y * app->scale, &r, &g, &b, &a));
                bool ink = (gui_vga_font[(unsigned char)text[i] * 16 + y] & (0x80 >> x)) != 0;
                SDL_Color want = ink ? color : back;
                assert((y == 15 && !ink) || (r == want.r && g == want.g && b == want.b));
            }
    SDL_DestroySurface(image);
}
/* the mouse at pixel x, y of the render, moved there or clicking */
static void mouse_px(App* app, int x, int y, bool click) {
    SDL_Event e = {0};
    float wx, wy;
    layout(app);
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.type = click ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_MOTION;
    e.button.button = SDL_BUTTON_LEFT;
    if (click) {
        e.button.x = wx;
        e.button.y = wy;
    } else {
        e.motion.x = wx;
        e.motion.y = wy;
    }
    event(app, &e);
}
/* a finger tapping pixel x, y of the render */
static void tap_px(App* app, int x, int y) {
    SDL_Event e = {0};
    float wx, wy;
    int w, h;
    layout(app);
    SDL_GetWindowSize(app->window, &w, &h);
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.tfinger.fingerID = 9;
    e.tfinger.x = wx / (float)w;
    e.tfinger.y = wy / (float)h;
    e.type = SDL_EVENT_FINGER_DOWN;
    event(app, &e);
    e.type = SDL_EVENT_FINGER_UP;
    event(app, &e);
}
/* the boxed menu's entry for an item, -1 when it has none */
static int item_entry(const GuiMenuBoxes* m, int item) {
    for (int i = 0; i < m->list.count; ++i)
        if (m->entry[i].kind == GUI_ENTRY_ITEM && m->entry[i].index == item) return i;
    return -1;
}
/* the rows a path takes wrapped at width */
static int path_rows(const char* path, int width) {
    int n = 0;
    for (; *path; path += gui_wrap(path, width)) ++n;
    return n;
}
#endif
static void colored_text_at(App* app, int col, int row, const char* prompt, SDL_Color color) {
    colored_text_on(app, col, row, prompt, color, black);
}
static void colored_text(App* app, int row, const char* prompt, SDL_Color color) {
    colored_text_at(app, 1, row, prompt, color);
}
static void red_text(App* app, int row, const char* prompt) { colored_text(app, row, prompt, red); }
/* the Settings' pick for key is value; NULL: the .ini's */
static bool setting_is(App* app, const char* key, const char* value) {
    const char* v = gui_settings_value(&app->settings, key);
    return value ? v && !strcmp(v, value) : !v;
}
/* Select on a row of Settings > App, then back to the menu unless it opened a screen of its own */
static void app_select(App* app, int row) {
    open_app(app, row);
    key(app, SDLK_RETURN);
    if (app->editing && app->form == GUI_FORM_APP) leave_editor(app);
}
/* the launcher's own two Settings on or off */
static void launcher(App* app, bool on) {
    assert(gui_settings_set(&app->settings, "libgccjit", on && SNX_GUI_HAS_GCCJIT ? "on" : "off"));
    assert(gui_settings_set(&app->settings, "gdbstub", on ? "on" : "off"));
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
    /* a theme can make that line a prompt: framed and placed, the done and the failed one apart */
    GuiPromptLine line = status ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE;
    app->theme.prompt.lines[line] = true;
    app->theme.prompt.blink = false;
    render(app);
    colored_text_at(app, (app->cols - 33) / 2, 3, "[ Press [ESC] to return to menu ]", red);
    app->theme.prompt.lines[line] = false;
    app->theme.prompt.lines[status ? GUI_PROMPT_DONE : GUI_PROMPT_FAILED] = true;
    render(app);
    red_text(app, 3, "Press [ESC] to return to menu");
    app->theme.prompt = gui_theme_builtin(app->theme.id)->prompt;
    for (int i = 0; i < 3; ++i) pump(app);
    mouse(app, 2, 4, true); /* A completed process still requires ESC acknowledgement. */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    key(app, SDLK_SPACE);
    assert(!app->menu.running && app->finished && app->menu.selected == selected);
    key(app, SDLK_ESCAPE);
    assert(!app->menu.running && !app->finished && app->menu.view == GUI_LOAD_NONE);
    assert(!app->menu.quit && !app->message[0] && app->has_output);
    /* the item the run was started from has the focus again: Games for one of its games */
    if (app->menu.active == GUI_GAMES && app->menu.games) selected = GUI_GAMES;
    assert(app->menu.selected == selected && setting_is(app, "fullscreen", "1"));
    render(app);
    assert(app->log_top == menu_row(app, GUI_EXIT) + 6);
}
/* what the button map page last drew: a text holding s, or the shape of id of a kind and style (-1: any) */
static bool drawn_text(const App* app, const char* s) {
    for (int i = 0; i < app->pad_drawing.n; ++i)
        if (app->pad_drawing.shape[i].kind == SNX_PAD_SHAPE_TEXT && strstr(app->pad_drawing.shape[i].text, s))
            return true;
    return false;
}
static const SnxPadShape* drawn(const App* app, int id, int kind, int style) {
    for (int i = 0; i < app->pad_drawing.n; ++i) {
        const SnxPadShape* p = &app->pad_drawing.shape[i];
        if (p->id == id && p->kind == kind && (style < 0 || p->style == style)) return p;
    }
    return NULL;
}
/* the page's events as the loop gets them, after the virtual pad's changes */
static void pads(App* app) {
    SDL_Event e;
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&e)) event(app, &e);
}
static void pad_press(App* app, SDL_Joystick* j, SDL_GamepadButton button) {
    SDL_SetJoystickVirtualButton(j, button, true);
    pads(app);
    SDL_SetJoystickVirtualButton(j, button, false);
    pads(app);
}
static bool picks_have(App* app, const char* line) {
    size_t size;
    char* kept = (char*)snx_read_file(app->settings_file, &size);
    bool has = kept && strstr(kept, line);
    free(kept);
    return has;
}
/* Settings > Controllers > Button map: the pad drawn (pad_screen.h) with what each control sends, on SDL's virtual
   Xbox pad. Its presets are gamepad_buttons' words after the game's own, its map gamepad_map; the keys, the pad's
   presses, its triggers and stick, a click and the wheel act on it, each change kept in the picks at once, Del puts
   both back, and ESC goes back to the page's row with the pads closed */
static void button_map(App* app) {
    static const char list[] =
        "Controllers\tgamepad_buttons\t1\tint\t0\t2\tA B X Y\tA B X Y\t0=By position|1=By label|2=Custom\n"
        "Controllers\tgamepad_deadzone\t10\tint\t0\t90\tDead zone\tStick dead zone\t0=0%|10=10%\n"
        "Controllers/Button map\tgamepad_map\t\ttext\t\t\tgamepad_buttons=2's map\tButton map\t\n";
    SDL_VirtualJoystickDesc d;
    SDL_JoystickID id;
    SDL_Joystick* j;
    SDL_Event e = {0};
    const SnxPadShape* box;
    float x = 0, y = 0; /* mingw's assert returns, as far as the compiler knows */
    int page = -1, focus;
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x045e/0x02ea"); /* a real pad plugged in stays out */
    gui_settings_free(&app->settings);
    memset(&app->settings, 0, sizeof app->settings);
    assert(!gui_settings_read(&app->settings, list));
    gui_settings_mark(&app->settings);
    for (int i = 0; i < app->settings.count; ++i)
        if (app->settings.values[i].kind == GUI_SETTING_PAGE) page = i;
    open_settings(app);
    key(app, SDLK_DOWN); /* past Launch, the launcher's own */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_GROUP && !strcmp(app->group_keys[app->group].key, "Controllers"));
    app->editor.row = page - app->first;
    key(app, SDLK_RETURN);
    /* drawn before SDL looks for pads, which takes it most of a second, saying so */
    colored_text(app, 2, "Looking for controllers...", rgb(app->theme.soft));
    /* no pad: the presets, the one in use focused, the game's own as nothing is picked */
    assert(app->form == GUI_FORM_PAD && app->pads_read && !app->gamepad && app->pad.presets == 4);
    assert(app->pad.in_use == 0 && app->pad.focus == 0 && !strcmp(app->pad.name[3], "Custom"));
    render(app);
    colored_text(app, 0, "SETTINGS  /  CONTROLLERS  /  BUTTON MAP", normal);
    assert(drawn_text(app, "No controller") && drawn_text(app, "Each game's") && drawn_text(app, "By label"));
    assert(drawn(app, 0, SNX_PAD_SHAPE_BOX, SNX_PAD_STYLE_FOCUS));
    /* its texts in the font's own glyphs, a byte each: the preset in use marked with a square */
    assert(drawn_text(app, "\xFE Each game's"));
    {
        SnxPadShape shape = {SNX_PAD_SHAPE_TEXT,
                             SNX_PAD_STYLE_PLAIN,
                             -1,
                             16.0f * (float)app->scale,
                             16.0f * 20 * (float)app->scale,
                             0,
                             0,
                             "\xFE \x82"};
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
        SDL_RenderClear(app->renderer);
        pad_text(app, &shape, app->theme.ink);
        colored_text_on(app, 2, 20, "\xFE \x82", rgb(app->theme.ink), black);
    }
    key(app, SDLK_DOWN);
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(app->pad.in_use == 2 && setting_is(app, "gamepad_buttons", "1") && picks_have(app, "gamepad_buttons=1"));
    /* a pad plugged in is opened, named, and what it holds lit */
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = 0x045e;
    d.product_id = 0x02ea;
    d.name = "Man\xC3\xA9 Pad"; /* UTF-8, shown in the font's glyphs */
    id = SDL_AttachVirtualJoystick(&d);
    j = SDL_OpenJoystick(id);
    assert(id && j);
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) /* at rest: a trigger at the bottom of its range */
        SDL_SetJoystickVirtualAxis(j, a, a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? SDL_JOYSTICK_AXIS_MIN : 0);
    pads(app);
    assert(app->gamepad && SDL_GetGamepadID(app->gamepad) == id);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pads(app);
    render(app);
    assert(!strcmp(app->pad.pad, "Man\x82 Pad") && !drawn_text(app, "No controller") && drawn_text(app, "Man\x82 Pad"));
    assert(drawn(app, SNX_PAD_PRESETS + SNX_PAD_SOUTH, SNX_PAD_SHAPE_DISC, SNX_PAD_STYLE_LIT));
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, false);
    pads(app);
    /* its presses as the game's menu takes them, by the button each sends: the D-pad moves, A selects */
    pad_press(app, j, SDL_GAMEPAD_BUTTON_DPAD_UP);
    assert(app->pad.focus == 1 && app->pad.in_use == 2);
    pad_press(app, j, SDL_GAMEPAD_BUTTON_SOUTH);
    assert(app->pad.in_use == 1 && setting_is(app, "gamepad_buttons", "0"));
    /* by position the right one sends A, the bottom one B */
    pad_press(app, j, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    pad_press(app, j, SDL_GAMEPAD_BUTTON_EAST);
    assert(app->pad.in_use == 2 && app->form == GUI_FORM_PAD);
    /* a control: A waits for a press, which takes its button and gives its own; Custom is then the one in use */
    app->pad.focus = SNX_PAD_PRESETS + SNX_PAD_SOUTH;
    pad_press(app, j, SDL_GAMEPAD_BUTTON_SOUTH);
    assert(app->pad.listening);
    pad_press(app, j, SDL_GAMEPAD_BUTTON_EAST);
    assert(!app->pad.listening && app->pad.in_use == 3 && setting_is(app, "gamepad_buttons", "2"));
    /* the whole map is the player's from then on, whatever pad comes next */
    assert(setting_is(app, "gamepad_map",
                      "south:B east:A west:X north:Y lb:L rb:R lt:ZL rt:ZR ls:L3 rs:R3 back:Minus "
                      "start:Plus up:Up down:Down left:Left right:Right"));
    assert(picks_have(app, "gamepad_map=south:B east:A west:X"));
    /* a trigger is pressed once it is past SNX_PAD_TRIGGER */
    app->pad.focus = SNX_PAD_PRESETS + SNX_PAD_LT;
    key(app, SDLK_RETURN);
    assert(app->pad.listening);
    /* a trigger's 0 to 32767 from its axis' whole range */
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (Sint16)(2 * (SNX_PAD_TRIGGER - 100) - 32768));
    pads(app);
    assert(app->pad.listening);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    pads(app);
    assert(!app->pad.listening && strstr(gui_settings_value(&app->settings, "gamepad_map"), " lt:ZR rt:ZL "));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    pads(app);
    /* the left stick past half way is the D-pad, a step each time it crosses */
    focus = app->pad.focus;
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    pads(app);
    assert(app->pad.focus != focus);
    focus = app->pad.focus;
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 32000);
    pads(app);
    assert(app->pad.focus == focus);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 0);
    pads(app);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    pads(app);
    assert(app->pad.focus != focus);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 0);
    pads(app);
    /* the wheel moves as Up and Down; a click on a preset uses it */
    focus = app->pad.focus;
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.integer_y = 1;
    event(app, &e);
    assert(app->pad.focus != focus);
    render(app);
    box = drawn(app, 1, SNX_PAD_SHAPE_BOX, -1);
    assert(box && SDL_RenderCoordinatesToWindow(app->renderer, box->x + box->w / 2, box->y + box->h / 2, &x, &y));
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = x;
    e.button.y = y;
    event(app, &e);
    assert(app->pad.in_use == 1 && setting_is(app, "gamepad_buttons", "0"));
    /* Del: the preset and the map the game's .ini's again */
    key(app, SDLK_DELETE);
    assert(app->pad.in_use == 0 && !gui_settings_value(&app->settings, "gamepad_buttons") &&
           !gui_settings_value(&app->settings, "gamepad_map") && !picks_have(app, "gamepad_"));
    /* the pad unplugged: none shown, nothing waited for */
    app->pad.focus = SNX_PAD_PRESETS + SNX_PAD_NORTH;
    key(app, SDLK_RETURN);
    assert(app->pad.listening);
    SDL_CloseJoystick(j);
    assert(SDL_DetachVirtualJoystick(id));
    pads(app);
    assert(!app->gamepad && !app->pad.listening);
    render(app);
    assert(drawn_text(app, "No controller"));
    /* ESC: to the presets, then back to the page's row, the pads closed */
    key(app, SDLK_ESCAPE);
    assert(app->form == GUI_FORM_PAD && app->pad.focus < SNX_PAD_PRESETS);
    key(app, SDLK_ESCAPE);
    assert(app->form == GUI_FORM_GROUP && app->page < 0 && app->editor.row == page - app->first);
    assert(!app->pads_read && !SDL_WasInit(SDL_INIT_GAMEPAD));
    /* a map it cannot read is named, in the failure colour, and the page shows the labels' */
    assert(gui_settings_set(&app->settings, "gamepad_map", "south:Q"));
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_PAD && strstr(app->editor.note, "\"south:Q\" is not a control"));
    assert(app->pad.own.bit[SNX_PAD_SOUTH] == SNX_PAD_UNSET);
    render(app);
    colored_text(app, 2, app->editor.note, rgb(app->theme.failure));
    key(app, SDLK_ESCAPE); /* on a preset: back at once */
    assert(app->form == GUI_FORM_GROUP);
    leave_editor(app);
}
/* a running step's line ends in its progress as the theme says, a done one in OK */
static void step_line(void) {
    GuiStep s;
    char runs[3][112];
    size_t lengths[3];
    memset(&s, 0, sizeof s);
    snprintf(s.label, sizeof s.label, "UNPACK");
    snprintf(s.detail, sizeof s.detail, "Game.xci");
    s.value = 1024;
    s.total = 4096;
    s.megabytes = 1;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(!strcmp(runs[0], "UNPACK:") && !strcmp(runs[1], " GAME.XCI") && !strcmp(runs[2], " 25%"));
    step_runs(&s, GUI_PROGRESS_AMOUNT, 0, runs, lengths);
    assert(!strcmp(runs[2], " 1.0 / 4.0 GB") && lengths[2] == 13);
    s.state = GUI_STEP_DONE;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(!strcmp(runs[2], "...OK"));
    s.total = 0;
    s.state = GUI_STEP_ACTIVE;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(!runs[2][0] && !lengths[2]);
}
static int SDLCALL hang(void* unused) {
    (void)unused;
    for (;;) SDL_Delay(100);
}
int main(int argc, char** argv) {
    int row;
    if (argc == 2 && !strcmp(argv[1], "--settings")) { /* as the runtime lists its settings */
        fputs("Display\tfullscreen\t0\tint\t0\t1\t1 = start in fullscreen\t\t0=0|1=1\n"
              "Graphics\tresolution\t1\tfloat\t0.25\t4\tInternal 3D resolution\t\t\n"
              "On-screen controls\tcontrols\t0\tint\t0\t1\tThe on-screen controls\tShow controls\t0=Off|1=On\n"
              "On-screen controls/Buttons\tcontrols_a_shown\t\tint\t0\t1\tA control drawn\tA\t0=Hidden|1=Shown\n"
              "On-screen controls/Buttons\tcontrols_b_shown\t\tint\t0\t1\tA control drawn\tB\t0=Hidden|1=Shown\n",
              stdout);
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "hang in join")) {
        SDL_Thread* thread = SDL_CreateThread(hang, "hung worker", NULL);
        assert(thread && !snx_write_file(argv[1], "ready", 5));
        SDL_WaitThread(thread, NULL); /* Never returns. ESC must kill, not request a graceful exit. */
        return 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--fullscreen=1") && !strcmp(argv[2], "--controls=0")) {
        puts("prepared game stdout");
        fputs("prepared game stderr\n", stderr);
        return 0;
    }
    if (argc > 1 && strstr(argv[0], "scarletnx-3ds")) {
        assert(argc == 2 && snx_is_file(argv[1]) && strstr(argv[1], "3ds input.CIA"));
        fputs("CIA execution is not implemented (stage 2)\n", stderr);
        return 3;
    }
    if (argc > 1 && strstr(argv[1], "option flags")) {
        int i = 2;
        assert(argc == 8 + SNX_GUI_HAS_GCCJIT && !strcmp(argv[i++], "--fullscreen=1") &&
               !strcmp(argv[i++], "--controls=0"));
        if (SNX_GUI_HAS_GCCJIT) assert(!strcmp(argv[i++], "--backend=gcc"));
        assert(!strcmp(argv[i++], "-S") && !strcmp(argv[i++], "-s"));
        assert(!strcmp(argv[i++], "--repo") && argv[i++][0] && i == argc);
        puts("Recompiling option flags\nScarletNX - option flags"); /* the runner's banner: now loading */
        return 0;
    }
    if (argc > 1) {
        assert(argc == 2 || argc == 6);
        assert(snx_is_file(argv[1]));
        if (strstr(argv[1], "Game00")) {
            assert(argc == 2 && strstr(argv[0], "gnuabi"));
            puts("Linux file selected from games/");
            return 0;
        }
        assert(strstr(argv[1], "game with spaces"));
        bool nx = strstr(argv[0], "runtime") != NULL; /* the program the menu is part of */
        if (nx)
            assert(argc == 6 && !strcmp(argv[2], "--fullscreen=1") && !strcmp(argv[3], "--controls=0") &&
                   !strcmp(argv[4], "--repo") && argv[5][0]);
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
    /* the menu's data folder from the test's own environment, never the user's */
    SDL_setenv_unsafe("XDG_DATA_HOME", "/nonexistent/snx-gui-test", 1);
    SDL_setenv_unsafe("XDG_CONFIG_HOME", "/nonexistent/snx-gui-test/config", 1);
    SDL_setenv_unsafe("XDG_STATE_HOME", "/nonexistent/snx-gui-test/state", 1);
    SDL_setenv_unsafe("XDG_CACHE_HOME", "/nonexistent/snx-gui-test/cache", 1);
    /* the menu runs Switch inputs with the runtime it is part of, the other drivers from beside it */
    assert(initialize(&app, "/nonexistent/snx-gui-test/bin/scarletnx"));
    lines(&app, NULL);
    assert(!strcmp(app.self, "/nonexistent/snx-gui-test/bin/scarletnx"));
    assert(!strcmp(app.drivers, "/nonexistent/snx-gui-test/bin"));
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(SDL_PLATFORM_ANDROID)
    /* no checkout and no games/ beside the test: games/ and themes/ come from the user's data folder */
    assert(!strcmp(app.data_dir, "/nonexistent/snx-gui-test/scarletnx"));
    assert(!strcmp(app.games_dir, "/nonexistent/snx-gui-test/scarletnx/games"));
    assert(!strcmp(app.themes_dirs[0], "/nonexistent/snx-gui-test/scarletnx/themes"));
    /* and the keys go in it, where the driver reads them first, not in ~/.switch */
    assert(!strcmp(app.keys_path, "/nonexistent/snx-gui-test/scarletnx/prod.keys"));
#endif
    /* the themes that come with the GUI: beside it in a build tree, else in share/scarletnx beside its bin/ */
    {
        char *root = snx_temp_dir(NULL), *bin = snx_path_join(root, "bin"), *beside = snx_path_join(bin, "themes");
        char *shared = snx_path_join(bin, "../share/scarletnx/themes"), *found;
        assert(root && !snx_mkdirs(bin));
        found = shipped_themes(bin);
        assert(!strcmp(found, shared));
        free(found);
        assert(!snx_mkdirs(beside));
        found = shipped_themes(bin);
        assert(!strcmp(found, beside));
        free(found);
        assert(!snx_remove_tree(root));
        free(shared);
        free(beside);
        free(bin);
        free(root);
    }
    /* the window says which build it is: commit, compiler, platform */
    assert(!strcmp(SDL_GetWindowTitle(app.window), snx_build_info()) && !strncmp(snx_build_info(), "ScarletNX ", 10));
    /* the boot's splash: the default theme's black page with the icon in the middle until its time; a key ends it and
       does nothing else. A theme's own shows in its chance of boots, the default's in the others */
    {
        SDL_Rect view;
        SDL_Color black = {0, 0, 0, 255}, icon = {17, 206, 161, 255}; /* wimsy.png's middle */
        splash_start(&app);
        assert(app.splash && !app.splash_page.r && !app.splash_page.g && !app.splash_page.b);
        app.clock = app.splash_until - 1;
        render(&app);
        assert(app.splash);
        SDL_GetRenderViewport(app.renderer, &view);
        assert(same_colour(pixel(&app, 1, 1), black) && same_colour(pixel(&app, view.w / 2.0f, view.h / 2.0f), icon));
        key(&app, SDLK_RETURN);
        assert(!app.splash_until && app.menu.selected == GUI_SWITCH && !app.menu.activated);
        render(&app);
        assert(!app.splash);
        app.clock = 0;
        assert(gui_splash_theme(gui_theme_builtin("yorha"), 24) == gui_theme_builtin("yorha"));
        assert(gui_splash_theme(gui_theme_builtin("yorha"), 25) == gui_theme_builtin("default"));
    }
    char* temporary = snx_temp_dir(NULL);
    char* self = snx_executable(argv[0]);
    char* input = snx_path_join(temporary, "game with spaces;literal.nro");
    assert(!snx_write_file(input, "0", 1));
    const char* names[] = {"runtime", "scarletnx-gnuabi", "scarletnx-3ds"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
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
    free(app.drivers);
    app.drivers = snx_strdup(temporary);
    free(app.self);
#ifdef _WIN32
    app.self = snx_path_join(temporary, "runtime.exe");
#else
    app.self = snx_path_join(temporary, "runtime");
#endif
    free(app.games_dir);
    app.games_dir = snx_path_join(temporary, "games");
    app.settings_file = snx_path_join(temporary, "config/settings.ini"); /* the Settings' picks, kept */
    app.folders_file = snx_path_join(temporary, "config/folders.txt");   /* the player's game folders */
    render(&app);
    colored_text(&app, menu_row(&app, GUI_SWITCH), "- Load Switch NSP/XCI/NRO", yellow);
    assert(!app.menu.activated);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_3DS);
    render(&app);
    colored_text(&app, menu_row(&app, GUI_3DS), "- Load 3DS CIA", yellow);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_KEYS);
    /* Never touch the real user's keys: exercise installation in a private home. */
    free(app.keys_path);
    char* switch_dir = snx_path_join(temporary, ".switch");
    app.keys_path = snx_path_join(switch_dir, "prod.keys");
    char* source_keys = snx_path_join(temporary, "prod.keys");
    assert(!snx_write_file(source_keys, "first keys", 10));
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_KEYS);
    render(&app);
    /* the drop line waits on the user: framed, in the middle of the screen, blinking at blink_ms */
    {
        const char* waiting = "[ Pick or drop prod.keys ]";
        int middle = SDL_max(app.rows / 2, 10), width = (int)strlen(waiting);
        SDL_Color yellow = rgb(app.theme.focus);
        app.clock = 1000 * app.theme.blink_ms * 2;
        render(&app);
        colored_text_at(&app, (app.cols - width) / 2, middle, waiting, yellow);
        app.clock += app.theme.blink_ms;
        render(&app);
        colored_text_at(&app, (app.cols - width) / 2, middle, "                       ", yellow);
        app.theme.prompt.blink = false; /* steady */
        render(&app);
        colored_text_at(&app, (app.cols - width) / 2, middle, waiting, yellow);
        app.theme.prompt.align = GUI_ALIGN_RIGHT;
        render(&app);
        colored_text_at(&app, app.cols - 1 - width, middle, waiting, yellow);
        app.theme.prompt.align = GUI_ALIGN_LEFT;
        render(&app);
        colored_text_at(&app, 1, middle, waiting, yellow);
        app.theme.prompt.lines[GUI_PROMPT_KEYS] = false; /* plain */
        render(&app);
        colored_text_at(&app, 1, middle, "Pick or drop prod.keys    ", yellow);
        app.theme.prompt = gui_theme_builtin(app.theme.id)->prompt;
        app.clock = 0;
    }
    drop(&app, input, false); /* A game is not a key file and must not launch. */
    assert(app.menu.view == GUI_LOAD_KEYS && !app.menu.running && !snx_is_file(app.keys_path));
    drop(&app, "file://remote/prod.keys", true);
    assert(app.menu.view == GUI_LOAD_KEYS && !snx_is_file(app.keys_path));
    SDL_Event keys_event = {0};
    keys_event.type = SDL_EVENT_DROP_FILE;
    keys_event.drop.data = source_keys;
    event(&app, &keys_event);
    assert(app.menu.view == GUI_LOAD_NONE && !app.menu.running);
    size_t keys_size;
    unsigned char* keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 10 && !memcmp(keys_data, "first keys", 10));
    free(keys_data);
#ifndef _WIN32
    struct stat keys_stat;
    assert(!stat(app.keys_path, &keys_stat) && (keys_stat.st_mode & 0777) == 0600);
#endif
    key(&app, SDLK_RETURN);
    render(&app);
    red_text(&app, 4, "Keys already setup. This will overwrite the previous ones");
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    key(&app, SDLK_RETURN);
    assert(!snx_write_file(source_keys, "replacement", 11));
    char* quoted_keys = snx_format("\"%s\"", source_keys);
    keys_event.type = SDL_EVENT_DROP_TEXT;
    keys_event.drop.data = quoted_keys;
    event(&app, &keys_event);
    free(quoted_keys);
    keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 11 && !memcmp(keys_data, "replacement", 11));
    free(keys_data);
    key(&app, SDLK_RETURN);
    drop(&app, app.keys_path, false); /* Dropping the destination itself is safe. */
    keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 11 && !memcmp(keys_data, "replacement", 11));
    free(keys_data);
    key(&app, SDLK_RETURN);
    assert(!snx_remove_tree(source_keys));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_KEYS && snx_file_size(app.keys_path) == 11);
    /* Failed replacement must leave the destination untouched and permit retry. */
    assert(!snx_remove_tree(app.keys_path) && !snx_mkdirs(app.keys_path));
    assert(!snx_write_file(source_keys, "replacement", 11));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_KEYS && snx_is_dir(app.keys_path) && app.message[0]);
    assert(!snx_remove_tree(app.keys_path));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_NONE && snx_is_file(app.keys_path));
    free(switch_dir);
    free(source_keys);
#ifdef SNX_XDG
    /* keys another tool left in ~/.switch, which the driver reads after the data folder's, are the installed build's
       keys in use until some are installed here */
    {
        char *home = snx_path_join(temporary, "home"), *saved_home = getenv("HOME") ? SDL_strdup(getenv("HOME")) : NULL;
        char *dir = snx_path_join(home, ".switch"), *found = snx_path_join(dir, "prod.keys"), *in_use;
        assert(!snx_mkdirs(dir) && !snx_write_file(found, "home keys", 9));
        SDL_setenv_unsafe("HOME", home, 1);
        in_use = keys_in_use(&app);
        assert(in_use && !strcmp(in_use, app.keys_path)); /* those installed above come first */
        free(in_use);
        assert(!snx_remove_tree(app.keys_path));
        in_use = keys_in_use(&app);
        assert(in_use && !strcmp(in_use, found));
        free(in_use);
        key(&app, SDLK_RETURN);
        assert(app.menu.view == GUI_LOAD_KEYS);
        render(&app);
        key(&app, SDLK_ESCAPE);
        if (saved_home)
            SDL_setenv_unsafe("HOME", saved_home, 1);
        else
            SDL_unsetenv_unsafe("HOME");
        SDL_free(saved_home);
        free(found);
        free(dir);
        free(home);
    }
#endif
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_SETTINGS);
    render(&app);
    colored_text(&app, app.rows - 4, "The game's .ini settings, the launcher's for the next runs, and its theme",
                 normal);
    mouse(&app, 0, 0, true); /* Blank space must not activate the current selection. */
    assert(!app.editing);
    /* Settings: the launcher's own, then each setting the runtime lists, a row each with its help above; what the
       menu's items used to set is in them */
    key(&app, SDLK_SPACE);
    assert(app.editing && app.editor.object && app.editor.count == GUI_SETTINGS_OWN + 3);
    /* and the disk: what ScarletNX's folders take, counted on a thread, and what is left */
    {
        char line[256], used[32], left[32];
        uint64_t total = 0;
        Uint64 until = SDL_GetTicks() + 5000;
        char* big = snx_path_join(app.games_dir, "big.bin");
        char* bytes = (char*)calloc(1, 3 << 20);
        /* the test's own folders alone: the user's data folder (%APPDATA%\scarletnx on Windows) may be another
           test's at the same time */
        char *data_dir = app.data_dir, *themes = app.themes_dirs[0];
        app.data_dir = snx_path_join(temporary, "data");
        app.themes_dirs[0] = snx_path_join(app.data_dir, "themes");
        assert(bytes && !snx_mkdirs(app.games_dir) && !snx_write_file(big, bytes, 3 << 20));
        key(&app, SDLK_ESCAPE); /* counted again as the Settings open */
        key(&app, SDLK_SPACE);
        assert(app.space && app.free_bytes);
        render(&app);
        while (!__atomic_load_n(&app.space->done, __ATOMIC_ACQUIRE) && SDL_GetTicks() < until) SDL_Delay(5);
        for (int i = 0; i < GUI_SPACE_DIRS; ++i) total += gui_tree_size(app.space->dirs[i], &app.space->stop);
        assert(app.space->done && app.space->used == total && !strcmp(app.space->dirs[0], app.games_dir));
        gui_size_text(used, sizeof used, total);
        gui_size_text(left, sizeof left, app.free_bytes);
        /* on the footer's line, after the RAM and the CPU */
        {
            char usage[256];
            gui_usage_text(usage, sizeof usage, app.usage.available, app.usage.memory_bytes, app.usage.cpu_percent,
                           SDL_GetNumLogicalCPUCores());
            snprintf(line, sizeof line, "%s  Disk: %s, %s free", usage, used, left);
        }
        key(&app, SDLK_ESCAPE);
        render(&app);
        colored_text(&app, app.rows - 3, line, rgb(app.theme.soft));
        assert(total == 3 << 20 && strstr(line, "  Disk: 3 MB, "));
        key(&app, SDLK_SPACE);
        assert(!snx_remove_tree(app.games_dir)); /* the browser's checks below start with no games/ */
        free(app.themes_dirs[0]);
        free(app.data_dir);
        app.themes_dirs[0] = themes;
        app.data_dir = data_dir;
        free(bytes);
        free(big);
    }
    /* the groups first: App, then a group per section the runtime lists, and the reset of them all */
    render(&app);
    assert(app.form == GUI_FORM_SETTINGS && app.groups == 5 && gui_editor_rows(&app.editor) == 6);
    colored_text(&app, 0, "SETTINGS", normal);
    colored_text(&app, 1, "Up/Down: group. Enter: open. ESC: back.", normal);
    colored_text(&app, 2, "The theme, the menu's letters, the theme editor and the game folders", rgb(app.theme.soft));
    colored_text(&app, 4, "- App", yellow);
    colored_text(&app, 5, "- Launch", normal);
    colored_text(&app, 6, "- Display", normal);
    colored_text(&app, 7, "- Graphics", normal);
    colored_text(&app, 8, "- On-screen controls", normal);
    colored_text(&app, 9, "- Reset all to the defaults", normal);
    marks(&app, 4, 9, false, false); /* every row shows: no arrows */
    key(&app, SDLK_DOWN);
    render(&app);
    colored_text(&app, 2, "libgccjit, gdbstub", rgb(app.theme.soft)); /* a group's help names its settings */
    key(&app, SDLK_RIGHT);                                            /* a group opens with Enter only */
    assert(app.form == GUI_FORM_SETTINGS);
    key(&app, SDLK_RETURN);
    /* a group's settings on a screen of their own, a row each with its help above */
    assert(app.form == GUI_FORM_GROUP && app.group == 1 && app.editor.count == GUI_SETTINGS_OWN);
    render(&app);
    colored_text(&app, 0, "SETTINGS  /  LAUNCH", normal);
    colored_text(&app, 1, "Up/Down: setting. Left/Right: change. Enter: type. Del: default. ESC: back.", normal);
    colored_text(&app, 2,
                 SNX_GUI_HAS_GCCJIT ? "libgccjit will be used instead of llvm"
                                    : "Program was not compiled with libgccjit",
                 rgb(app.theme.soft));
    colored_text(&app, 4, "- libgccjit                off", yellow);
    key(&app, SDLK_RIGHT);
    assert(setting_is(&app, "libgccjit", SNX_GUI_HAS_GCCJIT ? "on" : NULL));
    key(&app, SDLK_LEFT);
    assert(!gui_settings_value(&app.settings, "libgccjit"));
    mouse(&app, 2, 5, true); /* a click focuses a row, one on the focused row changes it */
    assert(app.editor.row == 1 && !gui_settings_value(&app.settings, "gdbstub"));
    render(&app);
    colored_text(&app, 2, "Enables gdbstub on port 1234", rgb(app.theme.soft));
    mouse(&app, 2, 5, true);
    assert(setting_is(&app, "gdbstub", "on"));
    key(&app, SDLK_RETURN);
    assert(!gui_settings_value(&app.settings, "gdbstub"));
    key(&app, SDLK_RETURN);
    assert(setting_is(&app, "gdbstub", "on"));
    /* ESC goes back to the groups, on the one it came from */
    key(&app, SDLK_ESCAPE);
    assert(app.editing && app.form == GUI_FORM_SETTINGS && app.editor.row == 1);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_GROUP && app.group == 2);
    key(&app, SDLK_RIGHT);
    assert(setting_is(&app, "fullscreen", "1"));
    render(&app);
    colored_text(&app, 0, "SETTINGS  /  DISPLAY", normal);
    colored_text(&app, 4, "- fullscreen               1", yellow); /* the launcher's choice was 0 */
    /* Del puts a setting back as it started */
    key(&app, SDLK_DELETE);
    assert(setting_is(&app, "fullscreen", "0"));
    key(&app, SDLK_RIGHT);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    /* a number typed, and nothing typed for the .ini's again */
    render(&app);
    colored_text(&app, 4, "- resolution               1 [default]", yellow); /* not picked: the default */
    key(&app, SDLK_RETURN);
    typed(&app, "0.5");
    key(&app, SDLK_RETURN);
    assert(setting_is(&app, "resolution", "0.5"));
    key(&app, SDLK_RETURN);
    for (int i = 0; i < 3; ++i) key(&app, SDLK_BACKSPACE);
    key(&app, SDLK_RETURN);
    assert(!gui_settings_value(&app.settings, "resolution"));
    key(&app, SDLK_ESCAPE);
    /* the game's menu's settings, named and worded as it shows them; a page's row opens the page, All first */
    {
        char line[64];
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        render(&app);
        colored_text(&app, 0, "SETTINGS  /  ON-SCREEN CONTROLS", normal);
        snprintf(line, sizeof line, "- %-24s %s", "Show controls", "Off"); /* the launcher's choice, 0 */
        colored_text(&app, 4, line, yellow);
        snprintf(line, sizeof line, "- %-24s %s", "Buttons", ">");
        colored_text(&app, 5, line, normal);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RIGHT); /* a page opens with Enter only */
        assert(app.page < 0);
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_GROUP && app.page >= 0 && app.editor.count == 3);
        render(&app);
        colored_text(&app, 0, "SETTINGS  /  ON-SCREEN CONTROLS  /  BUTTONS", normal);
        snprintf(line, sizeof line, "- %-24s %s", "All", "[default]");
        colored_text(&app, 4, line, yellow);
        snprintf(line, sizeof line, "- %-24s %s", "B", "[default]"); /* an empty default */
        colored_text(&app, 6, line, normal);
        key(&app, SDLK_RIGHT);
        assert(setting_is(&app, "controls_a_shown", "0") && setting_is(&app, "controls_b_shown", "0"));
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RIGHT);
        assert(setting_is(&app, "controls_a_shown", "1") && setting_is(&app, "controls_b_shown", "0"));
        render(&app);
        snprintf(line, sizeof line, "- %-24s %s", "All", "Mixed");
        colored_text(&app, 4, line, normal);
        snprintf(line, sizeof line, "- %-24s %s", "A", "Shown");
        colored_text(&app, 5, line, yellow);
        /* ESC comes back to the page's row, Del on it puts its rows back */
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_GROUP && app.page < 0 && app.editor.row == 1);
        key(&app, SDLK_DELETE);
        assert(!gui_settings_value(&app.settings, "controls_a_shown") &&
               !gui_settings_value(&app.settings, "controls_b_shown"));
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 4);
    }
    /* the groups' last row puts every setting back as it started */
    app.editor.row = gui_editor_rows(&app.editor) - 1;
    key(&app, SDLK_RETURN);
    assert(!gui_settings_value(&app.settings, "gdbstub") && setting_is(&app, "fullscreen", "0"));
    app.editor.row = 2; /* fullscreen 1 again, for the runs below */
    key(&app, SDLK_RETURN);
    key(&app, SDLK_RIGHT);
    assert(setting_is(&app, "fullscreen", "1"));
    key(&app, SDLK_ESCAPE);
    /* App: the theme and the letters, either way round, and the theme editor, which comes back here */
    app.editor.row = 0;
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_THEME);
    {
        char first[64], line[96];
        snprintf(first, sizeof first, "%s", app.theme.name);
        render(&app);
        colored_text(&app, 0, "SETTINGS  /  APP", normal);
        colored_text(&app, 1, "Up/Down: row. Left/Right: change. Enter: change or open. ESC: back.", normal);
        snprintf(line, sizeof line, "- %-24s %s", "Theme", first);
        colored_text(&app, 4, line, yellow);
        colored_text(&app, 5, "- Text size                Auto", normal);
        colored_text(&app, 6, "- Theme editor", normal);
        key(&app, SDLK_RIGHT);
        assert(strcmp(app.theme.name, first) && !strcmp(app.app_form.theme, app.theme.name));
        key(&app, SDLK_LEFT);
        assert(!strcmp(app.theme.name, first));
        lines(&app, NULL); /* the file's own, read again */
    }
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.menu.text_size == 1 && !strcmp(app.app_form.text, "1x"));
    key(&app, SDLK_LEFT);
    assert(!app.menu.text_size && !strcmp(app.app_form.text, "Auto"));
    key(&app, SDLK_LEFT);
    assert(app.menu.text_size == GUI_TEXT_MAX);
    key(&app, SDLK_RIGHT);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RIGHT); /* the editor opens with Enter only */
    assert(app.form == GUI_FORM_APP);
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_THEME && app.editing);
    key(&app, SDLK_ESCAPE);
    assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_EDITOR);
    /* the game folders: games/ first, always; one dropped is added and kept, once; the picker answers none here
       (SDL_FILE_DIALOG_DRIVER=none); Del or Enter twice takes one off, the folder staying */
    key(&app, SDLK_DOWN);
    render(&app);
    colored_text(&app, 7, "- Game folders             games/", yellow);
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_FOLDERS && gui_editor_rows(&app.editor) == 2 && app.editor.row == 0);
    {
        char* more = snx_path_join(temporary, "more games");
        char* file = snx_path_join(more, "Zed");
        char* kept_as;
        char *kept, line[160];
        size_t size;
        SDL_Event e = {0};
        assert(!snx_mkdirs(more) && !snx_write_file(file, "fixture", 7));
        kept_as = snx_absolute(more); /* as the system writes the folder once it is there: backslashes on Windows */
        render(&app);
        colored_text(&app, 0, "SETTINGS  /  APP  /  GAME FOLDERS", normal);
        colored_text(&app, 1, "Add a folder: pick, drop or paste one. Del: take one off. ESC: back.", normal);
        colored_text(&app, 2, "The data folder's games/, always listed", rgb(app.theme.soft));
        snprintf(line, sizeof line, "- %s", app.games_dir);
        colored_text(&app, 4, line, yellow);
        colored_text(&app, 5, "- Add a folder", normal);
        assert(!strcmp(app.app_form.folder[0], "not found")); /* games/ was taken away above */
        key(&app, SDLK_DELETE);
        assert(!strcmp(app.editor.note, "games/ is always listed"));
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = file;
        event(&app, &e);
        assert(app.folder_count == 0 && !strncmp(app.editor.note, "Not a folder: ", 14));
        e.drop.data = more;
        event(&app, &e);
        if (app.folder_count != 1 || strcmp(app.folders[0], kept_as) || strcmp(app.editor.note, "Added"))
            fprintf(stderr, "folders: %d, first \"%s\", expected \"%s\", note \"%s\"\n", app.folder_count,
                    app.folder_count ? app.folders[0] : "", kept_as, app.editor.note);
        assert(app.folder_count == 1 && !strcmp(app.folders[0], kept_as) && !strcmp(app.editor.note, "Added"));
        assert(app.form == GUI_FORM_FOLDERS && app.editor.row == 1 && gui_editor_rows(&app.editor) == 3);
        kept = (char*)snx_read_file(app.folders_file, &size);
        snprintf(line, sizeof line, "%s\n", kept_as);
        assert(kept && !strcmp(kept, line));
        free(kept);
        event(&app, &e);
        assert(app.folder_count == 1 && !strncmp(app.editor.note, "Already listed: ", 16));
        render(&app);
        snprintf(line, sizeof line, "- %s", kept_as);
        colored_text(&app, 5, line, yellow);
        app.editor.row = 2;
        key(&app, SDLK_RETURN);
        {
            SDL_Event answer;
            Uint64 until = SDL_GetTicks() + 2000;
            while (!app.editor.note[0] && SDL_GetTicks() < until) /* its answer comes on the picker's thread */
                while (SDL_PollEvent(&answer)) event(&app, &answer);
        }
        assert(app.folder_count == 1 && (!strcmp(app.editor.note, "No folder was picked.") ||
                                         !strncmp(app.editor.note, "The folder picker could not open: ", 34)));
        app.editor.row = 1;
        key(&app, SDLK_RETURN);
        assert(app.folder_count == 1 && !strcmp(app.editor.note, "Enter again takes it off the list"));
        key(&app, SDLK_RETURN);
        assert(!app.folder_count && !strcmp(app.editor.note, "Taken off the list; the folder stays"));
        assert(snx_is_dir(more) && (kept = (char*)snx_read_file(app.folders_file, &size)) && !size);
        free(kept);
        event(&app, &e); /* back for the games below */
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_FOLDERS);
        assert(!strcmp(app.app_form.folders, "games/ and 1 more"));
        free(file);
        free(kept_as);
        free(more);
    }
    key(&app, SDLK_ESCAPE);
    assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 0);
    key(&app, SDLK_ESCAPE);
    assert(!app.editing && app.menu.selected == GUI_SETTINGS);
    /* kept in a file, the changed ones alone, they come back as the menu opens again; the runs below get them */
    {
        size_t size;
        char* kept = (char*)snx_read_file(app.settings_file, &size);
        assert(kept && strstr(kept, "[Display]\nfullscreen=1\n") && !strstr(kept, "controls=") &&
               !strstr(kept, "gdbstub"));
        free(kept);
        gui_settings_free(&app.settings); /* as a new start has none read */
        assert(setting_is(&app, "fullscreen", NULL) && settings_of(&app) && setting_is(&app, "fullscreen", "1"));
    }
    key(&app, SDLK_UP);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    /* a load opens the screen its file is picked on at once: games/ is Games' now, not an option under it */
    assert(app.menu.selected == GUI_SWITCH && !app.menu.games);
    pump(&app);
    render(&app);
    colored_text(&app, menu_row(&app, GUI_SWITCH), "- Load Switch NSP/XCI/NRO", yellow);
    const char* screenshot = getenv("SNX_GUI_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_SWITCH);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_SWITCH);
    /* the activated item stays pink while the mouse hovers another in yellow; a click selects */
    mouse(&app, 2, menu_row(&app, GUI_LINUX), false);
    assert(app.menu.selected == GUI_LINUX && app.menu.active == GUI_SWITCH && app.menu.view == GUI_LOAD_NONE);
    render(&app);
    colored_text(&app, menu_row(&app, GUI_SWITCH), "- Load Switch NSP/XCI/NRO", pink);
    colored_text(&app, menu_row(&app, GUI_LINUX), "- Load Linux ARM ELF", yellow);
    mouse(&app, 2, menu_row(&app, GUI_SWITCH), true);
    assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_SWITCH);
    {
        const char* waiting = "[ Pick or drop a file ]";
        int middle = SDL_max(app.rows / 2, 10);
        app.clock = 1000 * app.theme.blink_ms * 2; /* the prompt shown, not blinked away */
        render(&app);
        colored_text(&app, 1, "Load Switch NSP/XCI/NRO: click below (or Enter) to open the file picker", normal);
        colored_text_at(&app, (app.cols - (int)strlen(waiting)) / 2, middle, waiting, yellow);
        app.clock = 0;
        /* the arrows have nothing to move to there; a click on the prompt, or Enter, opens the system's picker, which
           answers no file here (SDL_FILE_DIALOG_DRIVER=none) */
        key(&app, SDLK_DOWN);
        assert(app.menu.view == GUI_LOAD_DROP && !app.picked);
        mouse(&app, 10, middle, true);
        assert(app.picked && app.menu.view == GUI_LOAD_DROP);
        key(&app, SDLK_RETURN);
        {
            SDL_Event answer; /* its answer comes in the next poll, as a frame's */
            while (SDL_PollEvent(&answer)) event(&app, &answer);
        }
        assert(app.menu.view == GUI_LOAD_DROP && !strcmp(app.message, "No file was picked."));
    }
    app.menu.selected = GUI_LINUX; /* a drop runs the item that opened the screen */
    SDL_Event e = {0};
    e.type = SDL_EVENT_DROP_TEXT;
    e.drop.data = "one\ntwo";
    event(&app, &e);
    assert(app.menu.view == GUI_LOAD_DROP && !app.menu.running);
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.data = input;
    app.run_log = snx_path_join(temporary, "last-run.log"); /* the run's output, kept as it comes */
    event(&app, &e);
    assert(app.menu.running && app.menu.selected == GUI_SWITCH);
    finish(&app, 0);
    assert(!app.native && !gui_steps_last(&app.steps, "LAUNCH")); /* a driver that started no game stayed compiling */
    /* a menu that is no runtime's (a build without one) looks for scarletnx beside it, then on PATH */
    {
        char* own = app.self;
        char* path = SDL_strdup(SDL_getenv("PATH"));
        app.self = NULL;
        SDL_setenv_unsafe("PATH", "/nonexistent", 1);
        launch(&app, snx_strdup(input), false);
        assert(!app.menu.running && !strcmp(app.message, "Cannot find scarletnx beside the GUI or on PATH."));
        SDL_setenv_unsafe("PATH", path, 1);
        SDL_free(path);
        app.self = own;
        app.message[0] = 0;
    }
    {
        size_t size;
        char* kept = (char*)snx_read_file(app.run_log, &size);
        char* line = snx_format("game with spaces;literal.nro --fullscreen=1 --controls=0 --repo %s\n", app.data_dir);
        assert(kept && line && !app.run_out && !strncmp(kept, "$ ", 2) && strstr(kept, line));
        free(line);
        assert(strstr(kept, "driver stdout") && strstr(kept, "driver stderr") && strstr(kept, "\n[ended, status 0]\n"));
        free(kept);
    }
    key(&app, SDLK_DOWN);
    key(&app, SDLK_SPACE);
    assert(app.menu.selected == GUI_LINUX && app.menu.view == GUI_LOAD_DROP);
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
    char* flags_input = snx_path_join(temporary, "option flags.nro");
    assert(!snx_write_file(flags_input, "fixture", 7));
    app.menu.selected = GUI_SWITCH;
    launcher(&app, true);
    launch(&app, snx_strdup(flags_input), false);
    finish(&app, 0);
    assert(!app.native && gui_steps_last(&app.steps, "LAUNCH"));
    launcher(&app, false);
    app.menu.selected = GUI_LINUX;
    free(flags_input);

    /* recent games: what the GUI ran, newest first above the items and kept in a file, named as the run named it;
       one runs again with the options the menu has now, which the fake driver checks */
    {
        GuiRecent kept;
        char* recent_file = snx_path_join(temporary, "prefs/recent.txt");
        char* again = snx_path_join(temporary, "option flags again.nro");
        SDL_Color soft = rgb(app.theme.soft);
        assert(!snx_write_file(again, "fixture", 7));
        recent_open(&app, snx_strdup(recent_file));
        assert(!app.recent.count && !app.menu.recent);
        app.menu.selected = GUI_SWITCH;
        launcher(&app, true);
        launch(&app, snx_strdup(again), false);
        finish(&app, 0);
        assert(app.recent.count == 1 && app.menu.recent == 1 && !strcmp(app.recent.game[0].name, "option flags"));
        assert(app.recent.game[0].driver == GUI_SWITCH && !app.recent.game[0].prepared);
        gui_recent_load(&kept, recent_file);
        assert(kept.count == 1 && !strcmp(kept.game[0].name, "option flags"));
        assert(!strcmp(kept.game[0].path, app.recent.game[0].path));
        /* a title, the game and a blank row, then the items; up from the first item reaches the game */
        render(&app);
        colored_text(&app, 4, "Recent games", soft);
        colored_text(&app, 5, "- option flags", normal);
        assert(menu_row(&app, GUI_SWITCH) == 7);
        colored_text(&app, 7, "- Load Switch NSP/XCI/NRO", yellow);
        key(&app, SDLK_UP);
        render(&app);
        assert(app.menu.recent_focus == 1);
        colored_text(&app, 5, "- option flags", yellow);
        colored_text(&app, 7, "- Load Switch NSP/XCI/NRO", /* the item gave the focus up */
                     app.menu.activated && app.menu.active == GUI_SWITCH ? pink : normal);
        colored_text(&app, app.rows - 4, app.recent.game[0].path, normal); /* the description: where it is */
        /* run again from there, whatever item the menu last had: its own driver, the options of now */
        app.menu.selected = GUI_LINUX;
        key(&app, SDLK_RETURN);
        assert(app.menu.running && app.menu.selected == GUI_SWITCH);
        finish(&app, 0);
        assert(app.recent.count == 1 && app.menu.recent_focus == 1);
        /* the mouse: over the game it takes the focus, over an item it gives it back */
        mouse(&app, 3, menu_row(&app, GUI_KEYS), false);
        assert(!app.menu.recent_focus && app.menu.selected == GUI_KEYS);
        mouse(&app, 3, 5, false);
        assert(app.menu.recent_focus == 1);
        /* YoRHa: the focused game on the band, the title in its soft colour */
        lines(&app, "yorha");
        app.theme.texture = 0;
        render(&app);
        colored_text_on(&app, 1, 4, "Recent games", rgb(app.theme.soft), rgb(app.theme.page));
        colored_text_on(&app, 3, 5, "option flags", rgb(app.theme.page), rgb(app.theme.focus));
        lines(&app, "default");
        /* a game whose file went away leaves the list when chosen, and the file says so */
        assert(!remove(again));
        key(&app, SDLK_RETURN);
        assert(!app.menu.running && !app.recent.count && !app.menu.recent && !app.menu.recent_focus);
        assert(strstr(app.message, "is gone"));
        gui_recent_load(&kept, recent_file);
        assert(!kept.count);
        render(&app);
        assert(menu_row(&app, GUI_SWITCH) == 4);
        /* five: three of them, and a row that shows the rest, focused from the first item going up */
        {
            const char* names[] = {"e", "d", "c", "b", "a"}; /* added oldest first: a is the newest */
            for (int i = 0; i < 5; ++i) {
                char* path = snx_format("%s/%s.nro", temporary, names[i]);
                assert(!gui_recent_add(&app.recent, path, GUI_SWITCH, 0));
                free(path);
            }
            recent_changed(&app);
            assert(app.recent.count == 5 && app.menu.recent == 4 && !app.recent_all);
            app.menu.selected = GUI_SWITCH;
            render(&app);
            colored_text(&app, 4, "Recent games", soft);
            colored_text(&app, 5, "- a", normal);
            colored_text(&app, 7, "- c", normal);
            colored_text(&app, 8, "- ...show more", normal);
            assert(menu_row(&app, GUI_SWITCH) == 10);
            key(&app, SDLK_UP);
            assert(app.menu.recent_focus == 4);
            render(&app);
            colored_text(&app, 8, "- ...show more", yellow);
            colored_text(&app, app.rows - 4, "The other 2 recent games", normal);
            key(&app, SDLK_RETURN); /* all five, the focus on the first that was hidden */
            assert(!app.menu.running && app.recent_all && app.menu.recent == 5 && app.menu.recent_focus == 4);
            render(&app);
            colored_text(&app, 8, "- d", yellow);
            colored_text(&app, 9, "- e", normal);
            assert(menu_row(&app, GUI_SWITCH) == 11);
            app.recent = (GuiRecent){0};
            app.recent_all = false;
            recent_changed(&app);
            app.menu.recent_focus = 0;
        }
        /* the rest runs with no list */
        free(app.recent_file);
        app.recent_file = NULL;
        app.message[0] = 0;
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        free(again);
        free(recent_file);
    }

    /* 3DS: its own driver for drops, CIA browser and recents, with no Switch
       flags or direct execution, even when another item is hovered. */
    {
        char* original_games = app.games_dir;
        char* recent_file = snx_path_join(temporary, "prefs/3ds-recent.txt");
        app.games_dir = snx_path_join(temporary, "cia-games");
        char* cia = snx_path_join(app.games_dir, "3ds input.CIA");
        assert(!snx_mkdirs(app.games_dir) && !snx_write_file(cia, "fixture", 7));
        recent_open(&app, snx_strdup(recent_file));
        launcher(&app, true);
        mouse(&app, 2, menu_row(&app, GUI_3DS), true);
        assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_3DS);
        mouse(&app, 2, menu_row(&app, GUI_LINUX), false);
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = cia;
        event(&app, &e);
        assert(app.menu.running && app.menu.selected == GUI_3DS && !app.native);
        finish(&app, 3);
        assert(app.recent.count == 1 && app.recent.game[0].driver == GUI_3DS && !app.recent.game[0].prepared);
        GuiRecent kept;
        gui_recent_load(&kept, recent_file);
        assert(kept.count == 1 && kept.game[0].driver == GUI_3DS && !kept.game[0].prepared);
        open_recent(&app, 0);
        finish(&app, 3);
        /* a CIA in games/ is one of Games', which runs it with the 3DS driver */
        app.menu.recent_focus = 0;
        count_games(&app);
        assert(app.menu.games && app.games_total == 1);
        app.menu.selected = GUI_GAMES;
        key(&app, SDLK_RETURN);
        assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 1);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(app.menu.running && !app.native && app.menu.selected == GUI_3DS);
        finish(&app, 3);
        app.recent = (GuiRecent){0};
        app.menu.recent = app.menu.recent_focus = 0;
        free(app.recent_file);
        app.recent_file = NULL;
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        free(app.games_dir);
        app.games_dir = original_games;
        count_games(&app);
        free(cia);
        free(recent_file);
    }

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
    render(&app);
    red_text(&app, 3, "Aborting process tree...");
    app.theme.prompt.lines[GUI_PROMPT_ABORTING] = true;
    app.theme.prompt.blink = false;
    render(&app);
    colored_text_at(&app, (app.cols - 28) / 2, 3, "[ Aborting process tree... ]", red);
    app.theme.prompt = gui_theme_builtin(app.theme.id)->prompt;
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
    /* a finger on a running run's ESC line aborts it, anywhere else it does nothing */
    assert(!snx_write_file(hung, "", 0));
    launch(&app, snx_strdup(hung), false);
    for (until = SDL_GetTicks() + 5000, ready = false; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
        unsigned char* data = snx_read_file(hung, &size);
        ready = data && size == 5 && !memcmp(data, "ready", 5);
        free(data);
        pump(&app);
    }
    assert(ready && app.menu.running);
    assert(escape_row(&app) == 3); /* the console's ESC line, under the head */
    tap(&app, 20, 5);
    assert(app.menu.running && !app.aborting);
    lines(&app, "yorha"); /* a loading screen's is the last row */
    tap(&app, 20, 3);
    assert(!app.aborting);
    tap(&app, 20, app.rows - 1);
    assert(app.aborting);
    lines(&app, "default");
    while (app.menu.running && SDL_GetTicks() < until) {
        pump(&app);
        SDL_Delay(5);
    }
    assert(!app.menu.running && app.abnormal_exit);
    /* where ESC is a back gesture made by mistake (ask_abort, Android's) it asks first: ESC again, a click off the box
       or Keep going leave the run going; Abort aborts it. The ESC line's tap asks too, and a held key answers nothing */
    {
        int col, row, cols, pass;
        SDL_Event held;
        const GuiPalette* loading = &gui_theme_builtin("yorha")->run_colors;
        app.ask_abort = true;
        for (pass = 0; pass < 2; ++pass) {
            assert(!snx_write_file(hung, "", 0));
            launch(&app, snx_strdup(hung), false);
            for (until = SDL_GetTicks() + 5000, ready = false; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
                unsigned char* data = snx_read_file(hung, &size);
                ready = data && size == 5 && !memcmp(data, "ready", 5);
                free(data);
                pump(&app);
            }
            assert(ready && app.menu.running);
            key(&app, SDLK_ESCAPE);
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_KEEP && !app.aborting && app.menu.running);
            if (pass) break;
            render(&app);
            ask_box(&app, &col, &row, &cols);
            assert(cols == 42 && col == (app.cols - 42) / 2 && row == (app.rows - 6) / 2);
            colored_text_on(&app, col + 2, row + 1, "Abort the recompile? Its work is lost.", rgb(app.theme.warning),
                            black);
            colored_text_on(&app, col + 2, row + 3, "- Keep going", yellow, black);
            colored_text_on(&app, col + 2, row + 4, "- Abort", normal, black);
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && app.menu.running && !app.aborting);
            key(&app, SDLK_AC_BACK);
            key(&app, SDLK_DOWN);
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
            render(&app);
            colored_text_on(&app, col + 2, row + 3, "- Keep going", normal, black);
            colored_text_on(&app, col + 2, row + 4, "- Abort", yellow, black);
            key(&app, SDLK_ESCAPE); /* on Abort too */
            assert(!app.confirm.shown && !app.aborting);
            tap(&app, 20, escape_row(&app));
            assert(app.confirm.shown && !app.aborting);
            tap(&app, col + 2, row + 1); /* the question itself: nothing */
            assert(app.confirm.shown);
            tap(&app, col - 1, row + 4); /* beside Abort, off the box */
            assert(!app.confirm.shown && !app.aborting && app.menu.running);
            key(&app, SDLK_ESCAPE);
            mouse(&app, col + 3, row + 4, false); /* the pointer moves the focus */
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
            mouse(&app, col + 3, row + 3, true);
            assert(!app.confirm.shown && !app.aborting && app.menu.running);
            key(&app, SDLK_ESCAPE);
            key(&app, SDLK_RETURN);
            assert(!app.confirm.shown && !app.aborting);
            SDL_zero(held);
            held.type = SDL_EVENT_KEY_DOWN;
            held.key.key = SDLK_ESCAPE;
            held.key.repeat = true;
            event(&app, &held);
            assert(!app.confirm.shown);
            /* YoRHa's loading screen: its colours, the focused choice on a band of its ink */
            lines(&app, "yorha");
            app.fade_ms = 0;
            key(&app, SDLK_ESCAPE);
            render(&app);
            colored_text_on(&app, col + 2, row + 1, "Abort the recompile?", rgb(loading->warning), rgb(loading->page));
            colored_text_on(&app, col + 4, row + 3, "Keep going", rgb(loading->page), rgb(loading->ink));
            colored_text_on(&app, col + 4, row + 4, "Abort", rgb(loading->ink), rgb(loading->page));
            lines(&app, "default");
            tap(&app, col + 3, row + 4);
            assert(app.aborting && !app.confirm.shown);
            while (app.menu.running && SDL_GetTicks() < until) {
                pump(&app);
                SDL_Delay(5);
            }
            assert(!app.menu.running && app.abnormal_exit && !app.finished);
            assert(!strcmp(app.message, "Program ended abnormally (force-terminated by ESC)."));
        }
        /* a run that ends on its own while asking takes the question with it */
        gui_process_terminate(app.process);
        while (app.menu.running && SDL_GetTicks() < until) {
            pump(&app);
            SDL_Delay(5);
        }
        assert(!app.menu.running && app.finished && app.abnormal_exit && !app.confirm.shown);
        key(&app, SDLK_ESCAPE);
        assert(!app.finished && !app.confirm.shown);
        app.ask_abort = false;
    }
    free(hung);

    /* Games: hidden while games/ is missing or empty, shown with how many once games are there; its browser filters,
       pages and launches both ways. Reopening refreshes the folders rather than caching them */
    count_games(&app);
    assert(!app.menu.games && !app.games_total);
    assert(!snx_mkdirs(app.games_dir));
    count_games(&app);
    assert(!app.menu.games);
    render(&app);
    assert(menu_row(&app, GUI_SWITCH) == 4); /* the loads lead */
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
    /* the menu finds them itself, looking once a second while it is open */
    app.clock = 50000;
    app.games_checked = app.clock;
    pump(&app);
    assert(!app.menu.games);
    app.clock += 1000;
    pump(&app);
    assert(app.menu.games && app.games_total == 35);
    app.clock = 0;
    app.games_checked = 0;
    app.menu.selected = GUI_GAMES;
    app.menu.recent_focus = 0;
    render(&app);
    /* Games first, then the loads, the keys, Settings, Share log and Exit, a row each with none between */
    colored_text(&app, 4, "- Games (35 games)", pink); /* chosen last, for the CIA above */
    assert(menu_row(&app, GUI_SWITCH) == 5 && menu_row(&app, GUI_3DS) == 7 && menu_row(&app, GUI_KEYS) == 8);
    assert(menu_row(&app, GUI_SETTINGS) == 9 && menu_row(&app, GUI_LOG) == 10 && menu_row(&app, GUI_EXIT) == 11);
    colored_text(&app, app.rows - 4, "The games of games/ and of the folders added in Settings > App", normal);
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 35 && !app.message[0]);
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 1 && app.browser.selected == 0);
    mouse(&app, 2, 3, true); /* Back [ESC] is a selectable item too */
    assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_GAMES);
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 35 && !app.message[0]);
    assert(!strncmp(app.games.names[0], "Game00", 6));
    /* the player's folders' games after games/'s, each run from where it is; a folder gone is named */
    {
#ifdef _WIN32
        char* zed = snx_path_join(app.folders[0], "Zed.exe");
#else
        char* zed = snx_path_join(app.folders[0], "Zed");
#endif
        assert(app.folder_count == 1 && !snx_write_file(zed, "fixture", 7) && !snx_make_executable(zed));
        list_games(&app);
        assert(app.games.count == 36 && !strcmp(app.games.paths[35], zed) && !app.message[0]);
        app.folders[1] = snx_strdup("/nonexistent/snx-games");
        app.folder_count = 2;
        list_games(&app);
        assert(app.games.count == 36 && strstr(app.message, "/nonexistent/snx-games") &&
               strstr(app.message, "Settings > App > Game folders"));
        free(app.folders[1]);
        app.folder_count = 1;
        assert(!snx_remove_tree(zed));
        list_games(&app);
        assert(app.games.count == 35 && !app.message[0]);
        free(zed);
    }
    assert(gui_browser_pages(&app.browser) > 1);
    key(&app, SDLK_RIGHT);
    key(&app, SDLK_DOWN);
    assert(app.browser.page == 1 && app.browser.selected == 1);
    /* a game put in while the list shows joins it, the list staying on its page and file; taken out, it leaves */
    {
#ifdef _WIN32
        char* late = snx_path_join(app.games_dir, "Game35.exe");
#else
        char* late = snx_path_join(app.games_dir, "Game35");
#endif
        assert(!snx_write_file(late, "fixture", 7) && !snx_make_executable(late));
        app.clock = 60000;
        app.games_checked = 59000;
        pump(&app);
        assert(app.games.count == 36 && app.games_total == 36 && app.browser.page == 1 && app.browser.selected == 1);
        assert(!snx_remove_tree(late));
        app.clock += 1000;
        pump(&app);
        assert(app.games.count == 35 && app.browser.page == 1 && app.browser.selected == 1);
        app.clock = 0;
        app.games_checked = 0;
        free(late);
    }
    render(&app);
    marks(&app, 5, 5 + (int)app.browser.page_size - 1, true, true); /* games on the pages before and after */
    screenshot = getenv("SNX_GUI_BROWSER_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 2 && gui_browser_items(&app.browser) == 5);
    render(&app);
    marks(&app, 5, 9, true, false);
    key(&app, SDLK_PAGEUP);
    key(&app, SDLK_LEFT);
    assert(app.browser.page == 0 && app.browser.selected == 1);
    render(&app);
    marks(&app, 5, 5 + (int)app.browser.page_size - 1, false, true);
    mouse(&app, 2, 6, false);
    assert(app.browser.selected == 2 && !app.menu.running && !app.browser.activated);
    render(&app);
    colored_text(&app, 6, app.games.names[1], yellow);
    mouse(&app, 2, 5, true); /* native prepared game, not another extraction */
    assert(app.menu.running && app.native && screen(&app) == GUI_SCREEN_LOADING);
    finish(&app, 0);
    assert(app.menu.selected == GUI_GAMES); /* back from the run on Games */
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    char* removed = snx_path_join(app.games_dir, app.games.names[0]);
    assert(!snx_remove_tree(removed));
    free(removed);
    key(&app, SDLK_RETURN);
    assert(!app.menu.running && app.menu.view == GUI_LOAD_GAMES && app.message[0]);
    mouse(&app, 2, 6, false);
    render(&app);
    colored_text(&app, 5, app.games.names[0], pink);
    colored_text(&app, 6, app.games.names[1], yellow);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_RETURN);
    assert(app.games.count == 34 && app.games_total == 34);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE && !app.confirm.shown);
    /* Compact and spacious layouts share the renderer's mouse hit coordinates. Text size Auto keeps the letters of
       the window it opened in (1280x800: 2x), however the window is resized, and the window keeps 16 rows of them */
    const int sizes[][4] = {{2560, 1440, 45, 160}, {1280, 960, 30, 80}, {800, 480, 16, 50}, {1280, 800, 25, 80}};
    assert(app.auto_scale == 2);
    /* Auto is twice the display's scale, as far as 16 rows fit on a computer: 100%, 125%, 150% and 200%; a phone keeps
       20, so the Poco F8 (408 dpi, 1156 rows of pixels) and the Pixel 9 (420 dpi, 1080) get 3x, with 24 and 22 rows */
    assert(pick_auto_scale(1.0f, 800, 16) == 2 && pick_auto_scale(1.25f, 1000, 16) == 2);
    assert(pick_auto_scale(1.5f, 1200, 16) == 3 && pick_auto_scale(2.0f, 1600, 16) == 4);
    assert(pick_auto_scale(408.0f / 160, 1156, 20) == 3 && pick_auto_scale(420.0f / 160, 1080, 20) == 3);
    assert(pick_auto_scale(408.0f / 160, 1156, 16) == 4);
    assert(pick_auto_scale(1.0f, 480, 16) == 1 && pick_auto_scale(0.5f, 800, 16) == 1 &&
           pick_auto_scale(3.0f, 2400, 16) == 6);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        assert(SDL_SetWindowSize(app.window, sizes[i][0], sizes[i][1]));
        SDL_SyncWindow(app.window);
        render(&app);
        assert(app.scale == 2 && app.rows == sizes[i][2] && app.cols == sizes[i][3]);
        assert(menu_row(&app, GUI_LINUX) == menu_row(&app, GUI_SWITCH) + 1); /* no blank rows inside a group */
        app.menu.selected = app.rows == 16 ? GUI_LOG : GUI_SWITCH;           /* 16 rows: the menu slides to its focus */
        render(&app);
        mouse(&app, 2, menu_row(&app, GUI_LOG), false);
        assert(app.menu.selected == GUI_LOG && !app.menu.text_size);
        app.clock = 1; /* a description too long for 50 columns rests before it slides */
        render(&app);
        colored_text(&app, menu_row(&app, GUI_LOG), "- Share log", yellow);
        colored_text(&app, app.rows - 4, "Saves the last run's output and game log", normal);
        app.clock = 0;
        assert(app.rows == 16 ? menu_offset(&app) > 0 : menu_row(&app, GUI_EXIT) < app.rows - 4);
    }
    /* a text size too big for the window: the menu slides up as the focus goes down, then back, and a size that
       would leave fewer than 16 rows falls back */
    {
        assert(SDL_SetWindowSize(app.window, 1280, 1024));
        SDL_SyncWindow(app.window);
        app.menu.text_size = GUI_TEXT_MAX;
        app.menu.recent_focus = 0;
        app.menu.selected = GUI_SWITCH;
        render(&app);
        assert(app.scale == GUI_TEXT_MAX && app.rows == 16);
        assert(menu_row_in_full(&app, GUI_EXIT) > menu_last_row(&app) && !menu_offset(&app));
        assert(menu_shows(&app, menu_row(&app, GUI_SWITCH)) && !menu_shows(&app, menu_row(&app, GUI_EXIT)));
        marks(&app, 4, menu_last_row(&app), false, true); /* items below the last row */
        app.menu.selected = GUI_LOG;
        render(&app);
        assert(menu_offset(&app) > 0 && menu_row(&app, GUI_EXIT) > menu_last_row(&app));
        marks(&app, 4, menu_last_row(&app), true, true);
        app.menu.selected = GUI_EXIT;
        render(&app);
        assert(menu_row(&app, GUI_EXIT) == menu_last_row(&app) && !menu_shows(&app, menu_row(&app, GUI_SWITCH)));
        marks(&app, 4, menu_last_row(&app), true, false);
        app.menu.selected = GUI_SWITCH;
        render(&app);
        assert(!menu_offset(&app));
        /* a drag down moves the focus down row by row while the finger goes, not all at once when it lifts */
        {
            SDL_Event e = {0};
            float row = 1.0f / (float)app.rows;
            e.tfinger.fingerID = 7;
            e.tfinger.x = 0.5f;
            e.tfinger.y = 0.2f;
            e.type = SDL_EVENT_FINGER_DOWN;
            event(&app, &e);
            e.type = SDL_EVENT_FINGER_MOTION;
            e.tfinger.y = 0.2f + 1.5f * row;
            event(&app, &e);
            assert(app.menu.selected == GUI_LINUX);
            e.tfinger.y = 0.2f + 3.2f * row;
            event(&app, &e);
            assert(app.menu.selected == GUI_KEYS);
            e.type = SDL_EVENT_FINGER_UP;
            event(&app, &e);
            assert(app.menu.selected == GUI_KEYS && !app.finger);
            app.menu.selected = GUI_SWITCH;
            render(&app);
        }
        /* the wheel too: a notch is a row, down three and back up */
        {
            SDL_Event e = {0};
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.integer_y = -3;
            event(&app, &e);
            assert(app.menu.selected == GUI_KEYS);
            e.wheel.integer_y = 3;
            event(&app, &e);
            assert(app.menu.selected == GUI_SWITCH);
            /* both stop at the menu's ends, where the keys go round */
            app.menu.selected = GUI_EXIT;
            e.wheel.integer_y = -2;
            event(&app, &e);
            assert(app.menu.selected == GUI_EXIT && !app.menu.recent_focus);
            app.menu.recent_focus = app.menu.recent ? 1 : 0;
            app.menu.selected = 0;
            e.wheel.integer_y = 2;
            event(&app, &e);
            assert(app.menu.selected == 0 && app.menu.recent_focus == (app.menu.recent ? 1 : 0));
            app.menu.recent_focus = 0;
            app.menu.selected = GUI_EXIT;
            e.type = SDL_EVENT_FINGER_DOWN;
            e.tfinger.fingerID = 7;
            e.tfinger.x = 0.5f;
            e.tfinger.y = 0.2f;
            event(&app, &e);
            e.type = SDL_EVENT_FINGER_MOTION;
            e.tfinger.y = 0.2f + 2.5f / (float)app.rows;
            event(&app, &e);
            assert(app.menu.selected == GUI_EXIT && !app.menu.recent_focus);
            e.type = SDL_EVENT_FINGER_UP;
            event(&app, &e);
            app.menu.selected = GUI_SWITCH;
            render(&app);
        }
        assert(SDL_SetWindowSize(app.window, 800, 480)); /* 800x512 at least: 4x would leave 8 rows */
        SDL_SyncWindow(app.window);
        render(&app);
        assert(app.scale == 2 && app.rows == 16);
        app.menu.text_size = 0;
        assert(SDL_SetWindowSize(app.window, 1280, 800));
        SDL_SyncWindow(app.window);
        render(&app);
    }
    {
        SDL_Color half;
        char row_text[160];
        int last_row;
        float x, y;
        SDL_Color page, ink, lp, li, ls, dimmed, lf;
        const GuiPalette* run;
        char* steps_input;
        Uint64 until, shown, faded, later, entered, launch_shown, first_shown, second_shown;
        float cell;
        GuiSteps kept;
        const char* detail;
        const char* live;
        const char* wide;
        const char* wide_name;
        int i;
        /* Themes: the menu item goes through the built-ins, then each themes/<id>/theme.ini */
        char* themes = snx_path_join(temporary, "themes");
        char* custom = snx_path_join(themes, "custom");
        char* custom_ini = snx_path_join(custom, "theme.ini");
        const char* ini = "[theme]\nbase = yorha\nname = Custom test\n[colors]\npage = #102030\nink = not a colour\n"
                          "[head]\nmenu = TEST  /  MENU\n[body]\nmarker = \"> \"\n";
        assert(!snx_mkdirs(custom) && !snx_write_file(custom_ini, ini, strlen(ini)));
        free(app.themes_dirs[0]);
        app.themes_dirs[0] = snx_strdup(themes);
        assert(!strcmp(app.theme.id, "default"));
        app_select(&app, GUI_APP_THEME);
        assert(!strcmp(app.theme.id, "yorha") && !app.editing && app.theme.boxes == GUI_BOXES_FILL);
        lines(&app, NULL);
        app.menu.selected = GUI_SETTINGS;
        app.menu.recent_focus = 0;
        render(&app);
        page = rgb(app.theme.page);
        ink = rgb(app.theme.ink);
        colored_text_on(&app, 1, 0, "SCARLETNX  /  MENU", ink, page); /* its head plain, as NieR's titles */
        colored_text_on(&app, 3, menu_row(&app, GUI_SETTINGS), "Settings", page, ink); /* the focused line's band */
        colored_text_on(&app, 3, menu_row(&app, GUI_EXIT), "Exit", ink, page);
        colored_text_on(&app, 1, app.rows - 2, "CWD:", page, ink);
        /* the footer's bar goes on below its last whole row, to the window's edge */
        assert(SDL_SetWindowSize(app.window, 1280, 810));
        SDL_SyncWindow(app.window);
        render(&app);
        assert(800 % (16 * app.scale) == 0 && same_colour(pixel(&app, 4, 809), ink));
        assert(same_colour(pixel(&app, 4, 16.0f * (float)(app.scale * (app.rows - 3)) - 1), page));
        assert(SDL_SetWindowSize(app.window, 1280, 800));
        SDL_SyncWindow(app.window);
        app_select(&app, GUI_APP_THEME);
        assert(!strcmp(app.theme.id, "custom") && !strcmp(app.theme.name, "Custom test"));
        assert(app.theme.boxes == GUI_BOXES_FILL); /* a file that says nothing of boxes: its base's */
        lines(&app, NULL);
        render(&app);
        page.r = 0x10; /* its own page; its ink, unreadable, is YoRHa's */
        page.g = 0x20;
        page.b = 0x30;
        colored_text_on(&app, 1, 0, "TEST  /  MENU", ink, page); /* YoRHa's plain head */
        colored_text_on(&app, 1, menu_row(&app, GUI_SETTINGS), "> Settings", page, ink);
        colored_text_on(&app, 1, menu_row(&app, GUI_EXIT), "> Exit", ink, page);
        app_select(&app, GUI_APP_THEME);
        assert(!strcmp(app.theme.id, "default") && !strcmp(app.app_form.theme, "Default"));
        lines(&app, NULL);
        free(custom_ini);
        free(custom);
        free(themes);

        /* The theme editor: its screen in the menu's theme, a preview the menu's own drawing makes in the theme being
           edited, a typed value, the preview's switch, and Save making it the menu's theme */
        {
            int col, prow, cols, rows, r;
            SDL_Color night = {0x10, 0x20, 0x30, 255};
            char* saved = snx_format("%s/themes/mytheme/theme.ini", temporary);
            app_select(&app, GUI_APP_EDITOR);
            assert(app.editing && app.form == GUI_FORM_THEME && !strcmp(app.editor.id, "mytheme") &&
                   !strcmp(app.editor.theme.id, "default"));
            render(&app);
            colored_text_at(&app, 1, 0, "THEME EDITOR  /  mytheme", normal);
            /* more rows than the screen has: an arrow down at the top, both in the middle, up at the end */
            assert(app.editor.row == 0 && gui_editor_rows(&app.editor) > 2 * editor_rows(&app));
            marks(&app, 4, 3 + editor_rows(&app), false, true);
            app.editor.row = gui_editor_rows(&app.editor) / 2;
            render(&app);
            assert(app.editor.top > 0);
            marks(&app, 4, 3 + editor_rows(&app), true, true);
            app.editor.row = gui_editor_rows(&app.editor) - 1;
            render(&app);
            marks(&app, 4, 3 + editor_rows(&app), true, false);
            app.editor.row = 0;
            render(&app);
            preview_box(&app, &col, &prow, &cols, &rows);
            /* page = #102030, typed: the preview's page takes it, the editor's own stays the menu's */
            for (r = 0; !gui_editor_key_of(&app.editor, r) || strcmp(gui_editor_key_of(&app.editor, r)->key, "page");
                 ++r) {}
            app.editor.row = r;
            key(&app, SDLK_RETURN);
            assert(app.editor.typing && SDL_TextInputActive(app.window));
            for (int i = 0; i < 7; ++i) key(&app, SDLK_BACKSPACE);
            typed(&app, "#102030");
            key(&app, SDLK_RETURN);
            assert(!app.editor.typing && !SDL_TextInputActive(app.window) && app.editor.dirty);
            assert(!strcmp(app.editor.text, "#102030"));
            render(&app);
            assert(same_colour(
                pixel(&app, (col + cols * 76.0f / 80) * 8 * app.scale, (prow + rows * 12.5f / 25) * 16 * app.scale),
                night));
            assert(same_colour(pixel(&app, 4, (app.rows - 3) * 16.0f * app.scale + 8), black)); /* under its rows */
            /* Ctrl+P, or a tap on the preview: a run in the theme, then the menu again; Tab, which goes to the next
               screen, keeps a theme not saved, saying so */
            key(&app, SDLK_TAB);
            assert(app.editing && app.form == GUI_FORM_THEME && !strncmp(app.editor.note, "Not saved: ", 11));
            key_with(&app, SDLK_P, SDL_KMOD_LCTRL);
            assert(app.editor.preview == GUI_SCREEN_COMPILING);
            render(&app);
            mouse(&app, col + 2, prow + 2, true);
            assert(app.editor.preview == GUI_SCREEN_MENU);
            /* a space typed arrives as text: the key itself, Select elsewhere, does not take a text value */
            app.editor.row = 1; /* [theme] name */
            key(&app, SDLK_RETURN);
            typed(&app, "x");
            key(&app, SDLK_SPACE);
            assert(app.editor.typing);
            key(&app, SDLK_ESCAPE);
            assert(!app.editor.typing && strcmp(app.editor.theme.name, "Defaultx"));
            /* a click focuses a row, a click on the focused one changes it */
            mouse(&app, 2, 4 + 2 - app.editor.top, true);
            assert(app.editor.row == 2 && !app.editor.typing);
            /* ESC over changes asks first; Ctrl+S saves in themes/mytheme, and the menu's theme is it */
            key(&app, SDLK_ESCAPE);
            assert(app.editing && app.editor.leaving);
            key_with(&app, SDLK_S, SDL_KMOD_LCTRL);
            assert(snx_is_file(saved) && !app.editor.dirty && !strcmp(app.theme.id, "mytheme"));
            assert(same_colour(rgb(app.theme.page), night));
            key(&app, SDLK_ESCAPE); /* nothing left unsaved: it leaves at once, back to App */
            assert(app.editing && app.form == GUI_FORM_APP && app.editor.row == GUI_APP_EDITOR);
            leave_editor(&app);
            assert(app.theme.boxes == GUI_BOXES_NONE); /* the editor writes every key: none, not its base's line */
            render(&app);
            colored_text_on(&app, 1, menu_row(&app, GUI_LOG), "- Share log", normal, night);
            /* opened on a theme of themes/, it saves there under its own name */
            app_select(&app, GUI_APP_EDITOR);
            assert(app.editing && !strcmp(app.editor.id, "mytheme") && !strcmp(app.editor.file.base, "default"));
            key(&app, SDLK_ESCAPE);
            assert(app.form == GUI_FORM_APP);
            leave_editor(&app);
            free(saved);
        }

        /* YoRHa loading: a run as steps, typed in, with the ring, the dots and the bar; the run fades in, and out */
        steps_input = snx_path_join(temporary, "option flags steps.nro");
        assert(!snx_write_file(steps_input, "fixture", 7));
        gui_theme_load(&app.theme, (const char* const*)app.themes_dirs, 2, "yorha");
        lines(&app, NULL);
        app.menu.selected = GUI_SWITCH;
        launcher(&app, true);
        launch(&app, snx_strdup(steps_input), false);
        assert(app.menu.running && app.fade_ms);
        for (until = SDL_GetTicks() + 10000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) {
            pump(&app);
            render(&app);
        }
        assert(app.finished && !app.abnormal_exit && app.steps.count == 2 && screen(&app) == GUI_SCREEN_LOADING);
        run = &app.theme.run_colors;
        lp = rgb(run->page);
        li = rgb(run->ink);
        ls = rgb(run->soft);
        shown = app.steps.step[1].shown_ms;
        faded = app.fade_ms;
        later = (shown + 60000) / 1600 * 1600; /* where turning dots would show none: the "..." is the end's */
        /* entering: the menu stays and fades out over fade_out_ms, then the run fades in */
        entered = app.entering_ms;
        assert(entered && faded == entered + app.theme.fade_out_ms);
        app.clock = entered;
        render(&app);
        colored_text_on(&app, 1, 0, "SCARLETNX  /  MENU", rgb(app.theme.ink), rgb(app.theme.page));
        /* the run's output waits for the run: its rows, which end above the message (a small window has few) */
        for (row = app.log_top - 2; row <= app.log_top && row < app.rows - 4; ++row)
            colored_text_on(&app, 1, row, "                            ", rgb(app.theme.ink), rgb(app.theme.page));
        app.clock = entered + app.theme.fade_out_ms / 2;
        render(&app);
        dimmed = pixel(&app, 4, 4); /* the page, half way to black */
        assert(!same_colour(dimmed, rgb(app.theme.ink)) && !same_colour(dimmed, black));
        app.clock = entered + app.theme.fade_out_ms;
        render(&app);
        assert(same_colour(pixel(&app, 4, 4), black));
        app.theme.texture = false; /* exact backgrounds first */
        app.clock = later;
        render(&app);
        colored_text_on(&app, 1, 1, "BOOTING", li, lp);
        colored_text_on(&app, 9, 1, "OPTION FLAGS...", ls, lp);
        colored_text_on(&app, 1, 3, "RECOMPILE: OPTION FLAGS", ls, lp);
        colored_text_on(&app, 24, 3, "...OK", li, lp);
        colored_text_on(&app, 1, 4, "LAUNCH: OPTION FLAGS...OK", li, lp);
        colored_text_on(&app, 1, app.rows - 1, "Press [ESC] to return to menu", ls, lp);
        /* the run's last line as a prompt, when the theme says so: done, failed and aborting each on its own switch */
        app.theme.prompt.blink = false;
        app.theme.prompt.lines[GUI_PROMPT_DONE] = true;
        render(&app);
        colored_text_on(&app, (app.cols - 33) / 2, app.rows - 1, "[ Press [ESC] to return to menu ]", ls, lp);
        app.abnormal_exit = true;
        render(&app);
        colored_text_on(&app, 1, app.rows - 1, "Press [ESC] to return to menu    ", ls, lp);
        app.theme.prompt.lines[GUI_PROMPT_FAILED] = true;
        app.theme.prompt.align = GUI_ALIGN_RIGHT;
        render(&app);
        colored_text_on(&app, app.cols - 34, app.rows - 1, "[ Press [ESC] to return to menu ]", ls, lp);
        app.abnormal_exit = app.finished = false;
        app.menu.running = app.aborting = true;
        app.theme.prompt.lines[GUI_PROMPT_ABORTING] = true;
        render(&app);
        colored_text_on(&app, app.cols - 29, app.rows - 1, "[ Aborting process tree... ]", rgb(run->warning), lp);
        app.menu.running = app.aborting = false;
        app.finished = true;
        app.theme.prompt = gui_theme_builtin("yorha")->prompt;
        cell = 16.0f * (float)app.scale;
        /* the menu's footer over the ESC line, in the run's colours: YoRHa's bar style, a band of its ink */
        colored_text_on(&app, 1, app.rows - 3, "GUI + children: ", lp, li);
        colored_text_on(&app, 1, app.rows - 2, "CWD: ", lp, li);
        app.theme.footer = GUI_STYLE_PLAIN;
        render(&app);
        colored_text_on(&app, 1, app.rows - 2, "CWD:", ls, lp);
        app.theme.footer = GUI_STYLE_NONE;
        render(&app);
        colored_text_on(&app, 1, app.rows - 2, "    ", ls, lp);
        app.theme.footer = GUI_STYLE_BAR;
        colored_text_on(&app, app.cols - 6, app.rows - 5, "    ", li, lp); /* YoRHa has no bar unless asked */
        colored_text_on(&app, 1, app.rows - 4, "            ", li, lp);
        assert(same_colour(pixel(&app, 8.0f * (float)app.scale + 2, cell * (float)(app.rows - 5) + cell / 2), lp));
        app.theme.bar = true;
        render(&app);
        colored_text_on(&app, app.cols - 6, app.rows - 5, "100%", li, lp); /* above the footer */
        colored_text_on(&app, 1, app.rows - 4, "OPTION FLAGS", li, lp);
        assert(same_colour(pixel(&app, 8.0f * (float)app.scale + 2, cell * (float)(app.rows - 5) + cell / 2), li));
        colored_text_on(&app, 1, app.rows - 2, "CWD: ", lp, li);
        /* typing: five characters in, the cursor after them; async, each line from when it came. The run is over, so
           a row types whole in typing_ms: 25 characters in 10 ms */
        app.theme.typing_sync = false;
        app.theme.typing_ms = 10;
        app.theme.fade_in_ms = app.theme.fade_out_ms = 0;
        app.theme.blink_ms = 0;
        launch_shown = app.steps.step[1].shown_ms;
        app.steps.step[1].shown_ms = entered + 1; /* it came while the menu still showed: it types once the run is in */
        app.clock = faded + 2;
        render(&app);
        colored_text_on(&app, 1, 4, "LAUNC_", li, lp);
        app.steps.step[1].shown_ms = launch_shown;
        app.fade_ms = 0; /* in view from the start, for the lines below */
        app.clock = shown + 2;
        render(&app);
        colored_text_on(&app, 1, 4, "LAUNC_", li, lp);
        /* sync: one line at a time; the second waits for the first */
        app.theme.typing_sync = true;
        first_shown = app.steps.step[0].shown_ms;
        second_shown = app.steps.step[1].shown_ms;
        app.steps.step[1].shown_ms = first_shown; /* both lines came at once */
        app.clock = first_shown + 2;
        render(&app);
        colored_text_on(&app, 1, 3, "RECOM", ls, lp); /* not the last line: soft, with the cursor typing it */
        colored_text_on(&app, 6, 3, "_", li, lp);
        colored_text_on(&app, 1, 4, "    ", li, lp);
        app.theme.typing_sync = false;
        app.steps.step[1].shown_ms = second_shown;
        /* the newest row of a step still running types a character every typing_ms; a done step's, or one a newer
           row of its step passed, whole in typing_ms; one above the screen at once, the visible ones from when they
           came: a fast log keeps the cursor on its newest lines */
        {
            static size_t total[200], typed[200];
            static Uint64 came[200];
            static int slow[200];
            GuiTheme t = *gui_theme_builtin("yorha");
            int k;
            t.typing_ms = 10;
            for (k = 0; k < 200; ++k) {
                total[k] = 50;
                came[k] = 1000;
                slow[k] = 0;
            }
            slow[199] = 1; /* 200 rows at once, the last its running step's newest; rows 180 to 199 on the screen */
            assert(t.typing_sync && t.line_in == GUI_LINE_TYPING);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 5, typed) == 180);
            assert(typed[0] == 50 && typed[179] == 50 && typed[180] == 25 && typed[181] == 0);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 19 * 10 - 1, typed) == 198);
            assert(typed[180] == 50 && typed[198] == 45 && typed[199] == 0);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 19 * 10 + 30, typed) == 199 && typed[199] == 3);
            slow[199] = 0; /* the step is done: its 20 rows on the screen take 200 ms */
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 20 * 10 - 1, typed) == 199 && typed[199] == 45);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 20 * 10, typed) == -1 && typed[199] == 50);
            slow[199] = 1;
            t.typing_sync = 0; /* async: each row from when it came, at its own speed */
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 30, typed) == 199);
            assert(typed[180] == 50 && typed[198] == 50 && typed[199] == 3);
            t.line_in = GUI_LINE_NONE;
            assert(type_rows(&t, 200, 180, total, came, slow, 1000, typed) == -1 && typed[180] == 50 &&
                   typed[199] == 50);
        }
        /* detailed: a step's lines under it, indented; collapse folds a done step's lines, never a failed one's */
        kept = app.steps;
        lf = rgb(run->failure);
        gui_steps_reset(&app.steps);
        detail = "Recompiling Game (1 modules)\n  compiled block 1/2: code_000 (1.0 s)\n"
                 "  compiled block 2/2: code_001 (1.0 s)\nBuilt games/Game\n"
                 "ScarletNX - Game\nfs: 3 files mapped\n";
        gui_steps_feed(&app.steps, detail, strlen(detail), shown);
        gui_steps_finish(&app.steps, false);
        app.clock = later;
        render(&app);
        colored_text_on(&app, 1, 3, "RECOMPILE: GAME", ls, lp);
        colored_text_on(&app, 16, 3, "...OK", li, lp);
        colored_text_on(&app, 3, 4, "compiled block 1/2: code_000 (1.0 s)", ls, lp);
        colored_text_on(&app, 3, 5, "compiled block 2/2: code_001 (1.0 s)", ls, lp);
        colored_text_on(&app, 1, 6, "BUILT: GAMES/GAME", ls, lp);
        colored_text_on(&app, 1, 7, "LAUNCH: GAME...FAILED", lf, lp);
        colored_text_on(&app, 3, 8, "fs: 3 files mapped", ls, lp);
        app.theme.collapse = true;
        render(&app);
        colored_text_on(&app, 1, 4, "BUILT: GAMES/GAME", ls, lp);
        colored_text_on(&app, 1, 5, "LAUNCH: GAME...FAILED", lf, lp);
        colored_text_on(&app, 3, 6, "fs: 3 files mapped", ls, lp);
        app.theme.detailed = false;
        render(&app);
        colored_text_on(&app, 1, 5, "LAUNCH: GAME...FAILED", lf, lp);
        colored_text_on(&app, 3, 6, "                  ", ls, lp);
        /* a step still running keeps its lines, folded or not; the dots and the cursor stay on the step's own row */
        app.theme.detailed = true;
        gui_steps_reset(&app.steps);
        live = "Recompiling Live (1 modules)\n";
        gui_steps_feed(&app.steps, live, strlen(live), shown);
        gui_steps_feed(&app.steps, "Functions: 2\n", 13, shown + 1000);
        app.clock = shown + 1000 + 3 * app.theme.typing_ms + 1; /* a line types from when it came */
        render(&app);
        colored_text_on(&app, 3, 4, "Fun", ls, lp);
        colored_text_on(&app, 6, 4, "_         ", li, lp);
        app.fade_ms = shown + 2000; /* or once the run is in, when it came before */
        app.clock = shown + 2000 + 3 * app.theme.typing_ms + 1;
        render(&app);
        colored_text_on(&app, 3, 4, "Fun", ls, lp);
        colored_text_on(&app, 6, 4, "_         ", li, lp);
        app.fade_ms = 0;
        app.clock = later + 1200;
        render(&app);
        colored_text_on(&app, 16, 3, "..._", li, lp);
        colored_text_on(&app, 3, 4, "Functions: 2    ", ls, lp);
        /* two lines at once: the one the newer passed types whole, the newer a character every typing_ms */
        gui_steps_reset(&app.steps);
        gui_steps_feed(&app.steps, live, strlen(live), shown);
        gui_steps_feed(&app.steps, "Functions: 2\nBlocks: 3\n", 23, shown + 1000);
        app.clock = shown + 1000 + 3 * app.theme.typing_ms + 1;
        render(&app);
        colored_text_on(&app, 3, 4, "Functions: 2", ls, lp);
        colored_text_on(&app, 3, 5, "Blo", ls, lp);
        /* every line a step kept, down the screen; a row too wide for it ends at the edge */
        app.theme.collapse = app.theme.bar = false;
        gui_steps_reset(&app.steps);
        wide_name = "Recompiling A Game Whose Name Runs Far Past The Right Edge Of This Small Window (1 modules)\n";
        gui_steps_feed(&app.steps, wide_name, strlen(wide_name), shown);
        for (i = 0; i < 9; ++i) {
            char line[32];
            snprintf(line, sizeof line, "line %d\n", i);
            gui_steps_feed(&app.steps, line, strlen(line), shown);
        }
        wide = "fs: a line long enough to pass the right edge of a window eighty columns wide, and on, and on\n";
        gui_steps_feed(&app.steps, wide, strlen(wide), shown);
        gui_steps_finish(&app.steps, true);
        app.clock = later;
        render(&app);
        /* of the row's cols - 6, "RECOMPILE: ", the detail's space and "...OK" take 16 */
        snprintf(row_text, sizeof row_text, "RECOMPILE: %.*s", app.cols - 6 - 16,
                 "A GAME WHOSE NAME RUNS FAR PAST THE RIGHT EDGE OF THIS SMALL WINDOW");
        colored_text_on(&app, 1, 3, row_text, li, lp); /* the last step, in ink */
        colored_text_on(&app, app.cols - 10, 3, "...OK ", li, lp);
        colored_text_on(&app, 3, 4, "line 0", ls, lp);
        colored_text_on(&app, 3, 12, "line 8", ls, lp);
        snprintf(row_text, sizeof row_text, "%.*s...", app.cols - 8, wide);
        colored_text_on(&app, 3, 13, row_text, ls, lp);
        colored_text_on(&app, app.cols - 2, 13, " ", ls, lp);
        for (i = 9; i < 20; ++i) { /* past the bottom, the latest rows stay */
            char line[32];
            snprintf(line, sizeof line, "line %d\n", i);
            gui_steps_feed(&app.steps, line, strlen(line), shown);
        }
        render(&app);
        last_row = app.rows - 5; /* a blank row above the footer */
        colored_text_on(&app, 3, last_row, "line 19", ls, lp);
        colored_text_on(&app, 3, last_row - 1, "line 18", ls, lp);
        app.steps = kept;
        /* the page's faint lines, one pixel row in four */
        lines(&app, "yorha");
        app.fade_ms = faded;
        app.clock = later;
        render(&app);
        x = 8.0f * (float)app.scale * (float)(app.cols - 10);
        y = cell * 10;
        assert(!same_colour(pixel(&app, x, y + 3), lp) && same_colour(pixel(&app, x, y + 2), lp));
        /* the fade in from black, over fade_in_ms */
        app.clock = faded;
        render(&app);
        assert(same_colour(pixel(&app, x, y + 2), black));
        app.clock = faded + app.theme.fade_in_ms / 2;
        render(&app);
        half = pixel(&app, x, y + 2);
        assert(!same_colour(half, black) && !same_colour(half, lp));
        /* ESC: the run fades out to black over fade_out_ms, then the menu fades in */
        app.clock = later;
        key(&app, SDLK_ESCAPE);
        assert(app.finished && app.leaving_ms == app.clock);
        app.clock += app.theme.fade_out_ms / 2;
        pump(&app);
        render(&app);
        assert(app.finished && !same_colour(pixel(&app, x, y + 2), lp));
        app.clock += app.theme.fade_out_ms / 2;
        pump(&app);
        assert(!app.finished && app.menu.view == GUI_LOAD_NONE && app.fade_ms == app.clock);
        render(&app);
        assert(same_colour(pixel(&app, x, y + 2), black));
        app.clock += app.theme.fade_in_ms;
        render(&app);
        colored_text_on(&app, 1, 0, "SCARLETNX  /  MENU", rgb(app.theme.ink), rgb(app.theme.page)); /* the menus' own */
        app.clock = 0;
        lines(&app, "default");
        launcher(&app, false);
        free(steps_input);
    }
    /* The menu in boxes ([body] boxes, the built-ins'): each entry a box of its own, the focused one framed in the focus
       colour (line) or inverted with the cursor in the margin (fill); beside the list what the focus is about, the
       status under it, the bar at the foot with the same, or the screen's message, over the keys. A box under the
       pointer takes the focus and a click selects it; many entries scroll, and a narrow window puts the side box under
       the list */
    {
        GuiMenuBoxes m;
        GuiRect b, r;
        int s, cw, focus, exit_entry;
        SDL_Color dim, page, ink;
        app.theme = *gui_theme_builtin("default");
        assert(app.theme.boxes == GUI_BOXES_LINE && !app.theme.cursor[0] && app.theme.edges == GUI_EDGES_NONE);
        app.menu.selected = GUI_KEYS;
        app.menu.recent_focus = 0;
        app.message[0] = 0;
        app.abnormal_exit = false;
        app.fade_ms = 0; /* the last run's fade in, on the clock it had */
        render(&app);
        menu_boxes(&app, &m);
        s = app.scale;
        cw = 8 * s;
        dim = rgb(app.theme.dim);
        focus = item_entry(&m, GUI_KEYS);
        exit_entry = item_entry(&m, GUI_EXIT);
        assert(focus >= 0 && m.list.focus == focus && exit_entry > focus && !m.place.stacked);
        b = list_box(&m.list, focus);
        assert(same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), yellow)); /* framed in the focus colour */
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Setup device keys", yellow, black);
        b = list_box(&m.list, exit_entry);
        assert(same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), dim)); /* the others in the dim one */
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Exit", normal, black);
        /* the side box: the focus's name over a rule, what it is about and the keys in use; the status box under it */
        r = m.place.side;
        assert(r.x >= m.place.list.x + m.place.list.w && r.h > 0 && m.place.status.y >= r.y + r.h);
        text_px_is(&app, r.x + cw, r.y + 2 * s, "Setup device keys", normal, black);
        text_px_is(&app, r.x + cw, r.y + head_h(&app) + 2 * s, "Installs prod.keys", normal, black);
        text_px_is(&app, m.place.status.x + cw, m.place.status.y + 2 * s, "Status", normal, black);
        /* the bar: what the focus is about, then the keys against its right edge, each key in the focus colour */
        r = m.place.bar;
        text_px_is(&app, r.x + 3 * cw, r.y + 2 * s, "Installs prod.keys", normal, black);
        text_px_is(&app, r.x + r.w - cw - 20 * cw, r.y + 2 * s + 16 * s, "ESC", yellow, black);
        text_px_is(&app, r.x + r.w - cw - 16 * cw, r.y + 2 * s + 16 * s, "exit", normal, black);
        text_px_is(&app, r.x + r.w - cw - 10 * cw, r.y + 2 * s + 16 * s, "Tab", yellow, black);
        /* the screens as tabs over it all, the one shown framed in the focus colour */
        r = m.place.tabs;
        assert(r.h && r.y + r.h <= m.place.list.y);
        text_px_is(&app, tab_rect(&app, r, GUI_TAB_MENU).x + cw, r.y + 2 * s, "Menu", yellow, black);
        text_px_is(&app, tab_rect(&app, r, GUI_TAB_GAMES).x + cw, r.y + 2 * s, "Games", normal, black);
        assert(same_colour(pixel(&app, (float)tab_rect(&app, r, GUI_TAB_MENU).x, (float)(r.y + r.h / 2)), yellow));
        /* Tab goes round them, Shift+Tab back; a click on one opens it, as a pad's shoulders do (screen_tab) */
        key(&app, SDLK_TAB);
        assert(app.menu.view == GUI_LOAD_GAMES && !app.editing);
        key(&app, SDLK_TAB);
        assert(app.editing && app.form == GUI_FORM_SETTINGS);
        key(&app, SDLK_TAB);
        assert(!app.editing && app.menu.view == GUI_LOAD_NONE);
        key_with(&app, SDLK_TAB, SDL_KMOD_LSHIFT);
        assert(app.editing && app.form == GUI_FORM_SETTINGS);
        screen_tab(&app, -1);
        assert(!app.editing && app.menu.view == GUI_LOAD_GAMES);
        render(&app);
        r = tab_rect(&app, m.place.tabs, GUI_TAB_MENU);
        mouse_px(&app, r.x + 4, r.y + 4, true);
        assert(!app.editing && app.menu.view == GUI_LOAD_NONE);
        app.menu.selected = GUI_KEYS;
        app.menu.recent_focus = 0;
        render(&app);
        r = m.place.bar;
        /* the screen's message takes the bar's first row, a failure's in its colour */
        snprintf(app.message, sizeof app.message, "Program ended abnormally (status 7).");
        app.abnormal_exit = true;
        render(&app);
        text_px_is(&app, r.x + 3 * cw, r.y + 2 * s, "Program ended abnormally", red, black);
        app.message[0] = 0;
        app.abnormal_exit = false;
        /* the pointer: over Exit's box it takes the focus, a click on Setup device keys opens its screen */
        b = list_box(&m.list, exit_entry);
        mouse_px(&app, b.x + 4, b.y + 4, false);
        assert(app.menu.selected == GUI_EXIT && !app.menu.recent_focus);
        b = list_box(&m.list, focus);
        mouse_px(&app, b.x + 4, b.y + 4, true);
        assert(app.menu.view == GUI_LOAD_KEYS);
        key(&app, SDLK_ESCAPE);
        assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_KEYS);
        /* Games: Back, then as many of a page's games as the list's box holds, the focused one's path beside them
           and in the bar; Right turns the page, a click on Back goes back */
        {
            GuiGamesBoxes g;
            size_t page;
            app.menu.selected = GUI_GAMES;
            assert(app.menu.games && app.games_total > 12);
            key(&app, SDLK_RETURN);
            assert(app.menu.view == GUI_LOAD_GAMES && !app.browser.selected);
            render(&app);
            games_boxes(&app, &g);
            text_px_is(&app, list_box(&g.list, 0).x + cw / 2, item_text_y(&app, list_box(&g.list, 0)), "Back [ESC]",
                       yellow, black); /* the focus, first */
            key(&app, SDLK_DOWN);
            render(&app);
            games_boxes(&app, &g);
            page = games_page(&app);
            assert(app.browser.page_size == page && g.list.count == (int)page + 1 && g.list.shown == g.list.count);
            text_px_is(&app, list_box(&g.list, 0).x + cw / 2, item_text_y(&app, list_box(&g.list, 0)), "Back [ESC]",
                       normal, black);
            b = list_box(&g.list, 1);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- ", yellow, black);
            text_px_is(&app, b.x + cw / 2 + 2 * cw, item_text_y(&app, b), app.games.names[0], yellow, black);
            text_px_is(&app, g.place.side.x + cw, g.place.side.y + 2 * s, app.games.names[0], normal, black);
            text_px_is(&app, g.place.bar.x + 3 * cw, g.place.bar.y + 2 * s, "/", normal, black); /* its path */
            key(&app, SDLK_RIGHT);
            assert(app.browser.page == 1);
            render(&app);
            games_boxes(&app, &g);
            assert(g.start == page);
            b = list_box(&g.list, 0);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_GAMES);
        }
        /* the keys' screen: one box with where they go, the line that waits in its middle and Back at its foot; a click
           in the box opens the picker, whose answer (none here) the bar says; a click on Back goes back */
        {
            GuiPickBoxes k;
            SDL_Event answer;
            Uint64 until;
            const char* waiting = "[ Pick or drop prod.keys ]";
            app.menu.selected = GUI_KEYS;
            key(&app, SDLK_RETURN);
            assert(app.menu.view == GUI_LOAD_KEYS);
            app.clock = 1000 * app.theme.blink_ms * 2; /* the line shown, not blinked away */
            render(&app);
            pick_boxes(&app, &k);
            r = inner(&app, k.place.list);
            text_px_is(&app, r.x, k.place.list.y + 2 * s, "Setup device keys", normal, black);
            text_px_is(&app, r.x + (r.w - (int)strlen(waiting) * cw) / 2, k.prompt, waiting, yellow, black);
            text_px_is(&app, k.place.bar.x + 3 * cw, k.place.bar.y + 2 * s, "Setup device keys: click the box", normal,
                       black);
            app.clock = 0;
            mouse_px(&app, r.x + 10, k.prompt + 4, true);
            for (until = SDL_GetTicks() + 2000; !app.message[0] && SDL_GetTicks() < until;)
                while (SDL_PollEvent(&answer)) event(&app, &answer);
            assert(app.menu.view == GUI_LOAD_KEYS && !strcmp(app.message, "No file was picked."));
            render(&app);
            text_px_is(&app, k.place.bar.x + 3 * cw, k.place.bar.y + 2 * s, "No file was picked.", yellow, black);
            b = list_box(&k.back, 0);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.menu.view == GUI_LOAD_NONE);
            app.message[0] = 0;
        }
        /* the Settings: the groups on the left, beside them the focused group's rows; Enter opens it, its rows taking
           the focus and the group marked chosen; a click on another group opens that one, one on a row focuses it and
           a second changes it; the bar says what the focused row is */
        {
            GuiFormBoxes f;
            int launch, row;
            open_settings(&app);
            assert(app.editing && app.form == GUI_FORM_SETTINGS && !app.editor.row && app.groups > 3);
            render(&app);
            form_boxes(&app, &f);
            assert(f.place.side.x > f.place.list.x + f.place.list.w && f.left.count == app.groups + 1);
            b = list_box(&f.left, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- App", yellow, black);
            b = list_box(&f.left, app.groups); /* the last row, whole */
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Reset all to the defaults", normal, black);
            text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, "App", normal, black);
            b = list_box(&f.right, GUI_APP_THEME);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Theme", normal,
                       black); /* no focus on the right yet */
            for (launch = 0; launch < app.groups && strcmp(app.group_keys[launch].key, "Launch"); ++launch) {}
            assert(launch < app.groups);
            app.editor.row = launch;
            render(&app);
            form_boxes(&app, &f);
            text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, "Launch", normal, black);
            text_px_is(&app, list_box(&f.right, 0).x + cw / 2, item_text_y(&app, list_box(&f.right, 0)), "- libgccjit",
                       normal, black);
            key(&app, SDLK_RETURN);
            assert(app.form == GUI_FORM_GROUP && app.group == launch && !app.editor.row);
            render(&app);
            form_boxes(&app, &f);
            b = list_box(&f.left, launch);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Launch", pink, black); /* the group open */
            b = list_box(&f.right, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- libgccjit", yellow, black);
            text_px_is(&app, f.place.bar.x + 3 * cw, f.place.bar.y + 2 * s, app.settings.keys[0].about, normal, black);
            /* a click on the second row focuses it, a second click changes it */
            launcher(&app, false);
            b = list_box(&f.right, 1);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.form == GUI_FORM_GROUP && app.editor.row == 1 && setting_is(&app, "gdbstub", NULL));
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(setting_is(&app, "gdbstub", "on"));
            key(&app, SDLK_LEFT); /* back as it was */
            assert(setting_is(&app, "gdbstub", NULL));
            /* a click on App in the list opens it, from a group's rows */
            b = list_box(&f.left, 0);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.editing && app.form == GUI_FORM_APP && app.editor.row == GUI_APP_THEME);
            /* the theme editor: its keys on the left, the theme's menu in the preview on the right, which a click, or
               Ctrl+P, turns to a run; a long value being typed shows its end */
            app.editor.row = GUI_APP_EDITOR;
            key(&app, SDLK_RETURN);
            assert(app.form == GUI_FORM_THEME);
            render(&app);
            form_boxes(&app, &f);
            assert(f.left.count == gui_editor_rows(&app.editor) && f.left.shown < f.left.count);
            text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, "Preview", normal, black);
            mouse_px(&app, f.place.side.x + f.place.side.w / 2, f.place.side.y + f.place.side.h / 2, true);
            assert(app.editor.preview == GUI_SCREEN_COMPILING);
            key_with(&app, SDLK_P, SDL_KMOD_LCTRL);
            assert(app.editor.preview == GUI_SCREEN_MENU);
            for (row = 0;
                 !gui_editor_key_of(&app.editor, row) || strcmp(gui_editor_key_of(&app.editor, row)->key, "name");
                 ++row) {}
            app.editor.row = row;
            key(&app, SDLK_RETURN);
            assert(app.editor.typing);
            typed(&app, "a theme name long enough to run past its box's right edge");
            render(&app);
            form_boxes(&app, &f);
            b = list_box(&f.left, row);
            {
                int room = (b.w - cw * 3 / 2) / cw, end = b.x + cw / 2 + (room - 2) * cw;
                text_px_is(&app, end, item_text_y(&app, b), "e_", yellow, black); /* its end, and the cursor */
            }
            key(&app, SDLK_ESCAPE);
            key(&app, SDLK_ESCAPE);
            assert(app.editing && app.form == GUI_FORM_APP);
            leave_editor(&app);
        }
        /* a box over the screen is a box on the screen darkened: the exit question, its choices as boxes, a click on
           one taking it, a click off the box keeping on; the files an older version left, OK focused, a key closing it */
        {
            GuiList choices;
            app.menu.selected = GUI_EXIT;
            app.menu.recent_focus = 0;
            render(&app);
            menu_boxes(&app, &m);
            b = list_box(&m.list, m.list.focus);
            key(&app, SDLK_ESCAPE);
            assert(app.confirm.shown && app.confirm.exit);
            render(&app);
            ask_boxes(&app, &r, &choices);
            text_px_is(&app, r.x + cw, r.y + 4 * s, "Exit ScarletNX?", rgb(app.theme.warning), black);
            text_px_is(&app, list_box(&choices, 0).x + cw / 2, item_text_y(&app, list_box(&choices, 0)), "- Stay",
                       yellow, black);
            assert(b.x + b.w < r.x &&
                   same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), (SDL_Color){0x7F, 0x7F, 0, 255}));
            mouse_px(&app, list_box(&choices, 1).x + 4, list_box(&choices, 1).y + 4, false);
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
            mouse_px(&app, r.x + r.w + 8, r.y + r.h + 8, true);
            assert(!app.confirm.shown && !app.menu.quit);
            key(&app, SDLK_ESCAPE);
            render(&app);
            mouse_px(&app, list_box(&choices, 1).x + 4, list_box(&choices, 1).y + 4, true);
            assert(!app.confirm.shown && app.menu.quit);
            app.menu.quit = false;
            app.left = snx_format("/old/recent.txt" SAY_LEFT_BY "/new/recent.txt\n");
            app.lefts = 1;
            app.left_shown = true;
            app.left_at = SDL_GetTicks(); /* not looked for again, as there is no such file */
            render(&app);
            {
                int width, height;
                area(&app, &width, &height);
                r = over_rect(&app, width / cw - 6, head_h(&app) + 2 * 16 * s + item_h(&app) + 16 * s);
                text_px_is(&app, r.x + cw, r.y + 2 * s, "An older version left these files", normal, black);
                text_px_is(&app, r.x + cw, r.y + head_h(&app) + 4 * s, "- /old/recent.txt", red, black);
                text_px_is(&app, r.x + cw, r.y + head_h(&app) + 4 * s + 16 * s, "-> /new/recent.txt", normal, black);
            }
            key(&app, SDLK_DOWN);
            assert(!app.left_shown && app.menu.selected == GUI_EXIT); /* the key closed it and did nothing else */
            free(app.left);
            app.left = NULL;
            app.lefts = 0;
            app.menu.selected = GUI_KEYS;
        }
        /* a run of the console layout: the driver's output in its box, sized to it, beside it what runs and how it
           ended, the ESC line in the bar, where a finger's tap aborts a run */
        {
            GuiPlace p;
            Uint64 until;
            int cols, rows;
            char* hung = snx_path_join(temporary, "hang in join boxed.elf");
            bool ready = false;
            assert(!snx_write_file(hung, "", 0));
            app.menu.selected = GUI_LINUX;
            launch(&app, snx_strdup(hung), false);
            for (until = SDL_GetTicks() + 5000; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
                size_t got = 0;
                unsigned char* data = snx_read_file(hung, &got);
                ready = data && got == 5 && !memcmp(data, "ready", 5);
                free(data);
                pump(&app);
            }
            assert(ready && app.menu.running);
            render(&app);
            run_place(&app, &p);
            run_console(&app, &p, &cols, &rows);
            assert(app.console.cols == cols && app.console.rows == rows && !p.tabs.h);
            text_px_is(&app, p.list.x + cw, p.list.y + 2 * s, "Output", normal, black);
            text_px_is(&app, p.side.x + cw, p.side.y + 2 * s, "Load Linux ARM ELF", normal, black);
            r = box_rect(p.bar.x + 3 * cw, p.bar.y, p.bar.w - 4 * cw, p.bar.h);
            text_px_is(&app, r.x + r.w - (int)strlen(SAY_ABORT) * cw, p.bar.y + 2 * s + 16 * s, SAY_ABORT, red, black);
            tap_px(&app, p.bar.x + p.bar.w / 2, p.bar.y + p.bar.h / 2);
            assert(app.aborting);
            for (until = SDL_GetTicks() + 3000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) pump(&app);
            assert(!app.menu.running && !app.finished);
            free(hung);
            /* one that ends: its message beside the output, the ESC line to go back */
            app.menu.selected = GUI_SWITCH;
            launch(&app, snx_strdup(input), false);
            for (until = SDL_GetTicks() + 5000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) pump(&app);
            assert(app.finished && !app.abnormal_exit);
            render(&app);
            run_place(&app, &p);
            text_px_is(&app, p.list.x + cw, p.list.y + head_h(&app) + 2 * s, "driver std", normal, black);
            text_px_is(&app, p.side.x + cw,
                       p.side.y + head_h(&app) + 2 * s + wrap_rows(app.file, (p.side.w - 2 * cw) / cw, 1) * 16 * s +
                           8 * s,
                       "Driver exited with status 0.", yellow, black);
            text_px_is(&app, p.bar.x + 3 * cw, p.bar.y + 2 * s + 16 * s, SAY_DONE, red, black);
            key(&app, SDLK_ESCAPE);
            assert(!app.finished && app.menu.view == GUI_LOAD_NONE);
            pump(&app);
            app.menu.selected = GUI_KEYS;
            app.menu.recent_focus = 0;
        }
        /* YoRHa: the boxes on its dim colour, the focused one on its focus colour with the cursor in the margin, the
           side box's name on a band of its ink */
        app.theme = *gui_theme_builtin("yorha");
        assert(app.theme.boxes == GUI_BOXES_FILL && !strcmp(app.theme.cursor, "\xe2\x96\xba "));
        /* the page behind the boxes: its grid and diagonals in the dim colour under them, the corners darkened over
           everything */
        {
            SDL_Color plain = rgb(app.theme.page), c, corner, side;
            int width, height;
            app.theme.corners = false;
            render(&app);
            area(&app, &width, &height);
            c = pixel(&app, 0, (float)(height / 2 + 1)); /* the grid's first column, beside the boxes */
            assert(!same_colour(c, plain) && c.r < plain.r && c.r > app.theme.dim.r);
            corner = pixel(&app, 1, 1);
            side = pixel(&app, 1, (float)(height / 2 + 1));
            c = pixel(&app, (float)(width / 2), (float)(height / 2)); /* the middle, a box */
            app.theme.corners = true;
            render(&app);
            assert(same_colour(pixel(&app, (float)(width / 2), (float)(height / 2)), c)); /* untouched */
            assert(pixel(&app, 1, 1).r + 30 < corner.r);                                  /* a corner, darker */
            assert(pixel(&app, 1, (float)(height / 2 + 1)).r + 30 > side.r);              /* an edge's middle, less */
            app.theme.corners = app.theme.grid = app.theme.diagonals = false;             /* exact colours below */
        }
        page = rgb(app.theme.page);
        ink = rgb(app.theme.ink);
        render(&app);
        menu_boxes(&app, &m);
        b = list_box(&m.list, m.list.focus);
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "\xfe Setup device keys", page, rgb(app.theme.focus));
        text_px_is(&app, m.list.at.x, item_text_y(&app, b), "\x10", rgb(app.theme.focus), page);
        b = list_box(&m.list, item_entry(&m, GUI_EXIT));
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "\xfe Exit", ink, dim = rgb(app.theme.dim));
        r = m.place.side;
        text_px_is(&app, r.x + cw, r.y + 2 * s, "Setup device keys", page, ink);
        /* its edges, top and bottom: a line, a tick between marks, three dots in a triangle 26 pixels (at 1x) apart */
        {
            GuiRect top = m.place.edge_top, bottom = m.place.edge_bottom;
            int step = 26 * s;
            SDL_Color edge_ink = rgb(app.theme.edge_color);
            assert(top.y == 0 && top.h == 12 * s && bottom.h == 12 * s && bottom.y + bottom.h == 800);
            assert(m.place.tabs.y >= top.y + top.h);
            assert(same_colour(pixel(&app, (float)(top.w / 2 + 3), (float)(top.y + 2 * s)), edge_ink)); /* the line */
            assert(same_colour(pixel(&app, (float)(step - 1), (float)(top.y + 3 * s)), edge_ink));      /* a tick */
            assert(same_colour(pixel(&app, (float)(step / 2 - 3 * s), (float)(top.y + 5 * s)), edge_ink));
            assert(same_colour(pixel(&app, (float)(step / 2 + s), (float)(top.y + 5 * s)), edge_ink));
            assert(same_colour(pixel(&app, (float)(step / 2 - s), (float)(top.y + 8 * s)), edge_ink));
            assert(same_colour(pixel(&app, (float)(step / 2 - s), (float)(top.y + 6 * s)), page)); /* between them */
            assert(same_colour(pixel(&app, (float)(step / 2 - 3 * s), (float)(bottom.y + 5 * s)), edge_ink));
        }
        /* its title twice the text's size, over its shadow in the dim colour a big pixel down and right */
        r = m.place.title;
        assert(r.h == 2 * 16 * s + 2 * s && r.y + r.h <= m.place.list.y);
        {
            const unsigned char* g = &gui_vga_font['S' * 16];
            int big = 2 * s, shadowed = 0;
            for (int fy = 0; fy < 15; ++fy)
                for (int fx = 0; fx < 7; ++fx) {
                    int here = g[fy] & (0x80 >> fx), under = g[fy + 1] & (0x80 >> (fx + 1));
                    if (here) assert(same_colour(pixel(&app, (float)(r.x + fx * big), (float)(r.y + fy * big)), ink));
                    if (here && !under) {
                        SDL_Color c = pixel(&app, (float)(r.x + (fx + 1) * big), (float)(r.y + (fy + 1) * big));
                        assert(same_colour(c, dim));
                        ++shadowed;
                    }
                }
            assert(shadowed > 4);
        }
        /* with its edges, tabs and big title, a 1280x800 window still holds the whole menu: three recent games, the row
           for the rest and every item */
        {
            GuiRecent kept = app.recent;
            int n;
            app.recent.count = 4;
            for (n = 0; n < 4; ++n) {
                snprintf(app.recent.game[n].name, sizeof app.recent.game[n].name, "Game %d", n);
                snprintf(app.recent.game[n].path, sizeof app.recent.game[n].path, "/games/Game %d", n);
                app.recent.game[n].driver = GUI_SWITCH;
            }
            app.recent_all = false;
            app.menu.recent = recent_shown(&app) + recent_more(&app);
            app.menu.games = true;
            render(&app);
            menu_boxes(&app, &m);
            assert(m.list.count == 1 + 4 + GUI_ITEM_COUNT && m.list.shown == m.list.count);
            app.recent = kept;
            app.menu.recent = recent_shown(&app) + recent_more(&app);
            count_games(&app);
        }
        /* letters too big for the screen: the list scrolls, the focus in view, an arrow where entries are hidden */
        app.menu.text_size = GUI_TEXT_MAX;
        app.menu.selected = GUI_EXIT;
        render(&app);
        menu_boxes(&app, &m);
        assert(m.list.shown < m.list.count && m.list.first > 0 && m.list.focus < m.list.first + m.list.shown);
        assert(menu_scrolls(&app));
        b = list_box(&m.list, m.list.first);
        text_px_is(&app, m.list.at.x + m.list.at.w - 8 * app.scale, item_text_y(&app, b), "\x1e", rgb(app.theme.soft),
                   page);
        assert(!m.place.stacked);
        /* and on a window too narrow for both, the side box goes under the list */
        assert(SDL_SetWindowSize(app.window, 800, 800));
        SDL_SyncWindow(app.window);
        render(&app);
        menu_boxes(&app, &m);
        assert(m.place.stacked && m.place.side.y > m.place.list.y + m.place.list.h && m.place.side.h > 0);
        assert(SDL_SetWindowSize(app.window, 1280, 800));
        SDL_SyncWindow(app.window);
        app.menu.text_size = 0;
        lines(&app, "default");
        render(&app);
    }
    /* ESC on the menu's first screen asks before the app closes: ESC again stays, Exit closes */
    assert(app.menu.view == GUI_LOAD_NONE && !app.menu.running && !app.finished && !app.confirm.shown);
    key(&app, SDLK_ESCAPE);
    assert(app.confirm.shown && app.confirm.exit && app.confirm.selected == GUI_CONFIRM_KEEP && !app.menu.quit);
    render(&app);
    key(&app, SDLK_ESCAPE);
    assert(!app.confirm.shown && !app.menu.quit);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(!app.confirm.shown && app.menu.quit);
    app.menu.quit = false;
    /* fingers: a tap clicks anywhere on a row, which a mouse does on the label only; a drag is no tap, nor a second
       finger; a swipe across turns the games' pages; back is ESC */
    {
        int row = menu_row(&app, GUI_SETTINGS);
        SDL_Event second = {0};
        assert(app.menu.view == GUI_LOAD_NONE && !app.menu.running && !app.finished && !app.editing);
        mouse(&app, app.cols - 3, row, true);
        assert(!app.editing);
        tap(&app, app.cols - 3, row);
        assert(app.editing && app.menu.selected == GUI_SETTINGS);
        key(&app, SDLK_ESCAPE);
        swipe(&app, 10, row, 10, row + 4);
        assert(!app.editing);
        second.type = SDL_EVENT_FINGER_DOWN;
        second.tfinger.fingerID = 8;
        event(&app, &second); /* the first finger is still down */
        assert(app.finger == 8);
        tap(&app, 2, row); /* finger 7: no finger of the screen's */
        assert(!app.editing);
        second.type = SDL_EVENT_FINGER_UP;
        second.tfinger.x = app.finger_x;
        second.tfinger.y = app.finger_y;
        event(&app, &second); /* finger 8 lifts where it went down, on the title: nothing there */
        assert(!app.finger && !app.editing);
        tap(&app, 2, row);
        assert(app.editing);
        key(&app, SDLK_ESCAPE);
        tap(&app, 2, menu_row(&app, GUI_SWITCH));
        assert(app.menu.view == GUI_LOAD_DROP);
        key(&app, SDLK_AC_BACK);
        assert(app.menu.view == GUI_LOAD_NONE);
        app.menu.view = GUI_LOAD_GAMES;
        app.browser = (GuiBrowser){.count = 1000};
        layout(&app);
        swipe(&app, 60, 10, 10, 11);
        assert(app.browser.page == 1);
        swipe(&app, 10, 10, 60, 10);
        assert(app.browser.page == 0);
        app.menu.view = GUI_LOAD_NONE;
        app.browser = (GuiBrowser){0};
    }
    /* Share log off Android: scarletnx.log in the working folder, with the message and the last run's output */
    {
        char* saved = snx_path_join(temporary, "scarletnx.log");
        size_t size = 0;
        char* text;
        SDL_free(app.cwd);
        app.cwd = SDL_strdup(temporary);
        snprintf(app.message, sizeof(app.message), "before the log");
        app.menu.selected = GUI_LOG;
#ifdef SNX_XDG
        /* an installed build's in its state folder, as the working folder is the home folder from the desktop */
        char *state = snx_path_join(temporary, "state"), *logged = snx_path_join(state, "scarletnx/scarletnx.log");
        char* installed_dir = app.data_dir;
        SDL_setenv_unsafe("XDG_STATE_HOME", state, 1);
        key(&app, SDLK_RETURN);
        assert(strstr(app.message, logged) && !snx_path_exists(saved));
        text = (char*)snx_read_file(logged, &size);
        assert(text && strstr(text, "Message: before the log"));
        free(text);
        free(logged);
        free(state);
        /* any other build's (a checkout, a portable copy) in the working folder */
        app.data_dir = snx_strdup(temporary);
        snprintf(app.message, sizeof(app.message), "before the log");
#endif
        key(&app, SDLK_RETURN);
        assert(!strncmp(app.message, "Log saved to ", 13) && strstr(app.message, "scarletnx.log"));
        text = (char*)snx_read_file(saved, &size);
        assert(text && strstr(text, "Message: before the log") && strstr(text, "== the last run: "));
        free(text);
        free(saved);
#ifdef SNX_XDG
        free(app.data_dir);
        app.data_dir = installed_dir;
#endif
    }
#ifdef SNX_XDG
    /* the menu's recent.txt and last-run.log in the state folder's gui/; an older build's in the data folder's stay */
    {
        char *data = snx_path_join(temporary, "data"), *state = snx_path_join(temporary, "state");
        char *old = snx_path_join(data, "scarletnx/gui"), *recent = snx_path_join(old, "recent.txt");
        char *run = snx_path_join(old, "last-run.log"), *folder, *expected = snx_path_join(state, "scarletnx/gui"),
             *now;
        assert(!snx_mkdirs(old) && !snx_write_file(recent, "recent", 6) && !snx_write_file(run, "run", 3));
        SDL_setenv_unsafe("XDG_DATA_HOME", data, 1);
        folder = state_folder();
        assert(folder && !strcmp(folder, expected) && snx_file_size(recent) == 6 && snx_file_size(run) == 3);
        now = snx_path_join(folder, "recent.txt");
        assert(!snx_path_exists(now) && snx_left_behind(recent, now));
        free(now);
        free(folder);
        /* the menu says, a line at a time, what an older build left where this one no longer looks: its own files,
           work/ and each game's, until they are gone */
#define LONG "Foo, a game whose name is longer than a row of the box has room for"
        {
            char *installed_dir = app.data_dir, *games = app.games_dir, *ini, *log, *threads, *work, *line;
            const char* const lines[] = {"%s/scarletnx/gui/recent.txt",
                                         "%s/scarletnx/gui/recent.txt",
                                         "%s/scarletnx/gui/last-run.log",
                                         "%s/scarletnx/gui/last-run.log",
                                         "%s/scarletnx/work",
                                         "%s/scarletnx/work",
                                         "%s/scarletnx/games/" LONG ".log",
                                         "%s/scarletnx/games/" LONG ".log",
                                         "%s/scarletnx/games/threads.txt",
                                         "%s/scarletnx/games/threads.txt",
                                         "%s/scarletnx/games/" LONG ".ini",
                                         "%s/scarletnx/games/" LONG ".ini"};
            const char* const into[] = {state, state, "cache", "state", "state", "config"};
            char* config = snx_path_join(temporary, "config");
            char* cache = snx_path_join(temporary, "cache");
            const char* at;
            int i;
            SDL_setenv_unsafe("XDG_CONFIG_HOME", config, 1);
            SDL_setenv_unsafe("XDG_CACHE_HOME", cache, 1);
            app.data_dir = snx_path_join(data, "scarletnx");
            app.games_dir = snx_path_join(app.data_dir, "games");
            app.recent_file = snx_path_join(expected, "recent.txt");
            ini = snx_path_join(app.games_dir, LONG ".ini");
            log = snx_path_join(app.games_dir, LONG ".log");
            threads = snx_path_join(app.games_dir, "threads.txt");
            work = snx_path_join(app.data_dir, "work");
            assert(!snx_mkdirs(work) && !snx_mkdirs(app.games_dir) && !snx_write_file(ini, "", 0));
            assert(!snx_write_file(log, "", 0) && !snx_write_file(threads, "", 0));
            left_behind(&app);
            assert(app.lefts == 6);
            for (i = 0, at = app.left; i < 6; ++i, at = strchr(at, '\n') + 1) {
                char *old_path = snx_format(lines[2 * i], data), *now_path;
                const char* root = !strcmp(into[i], "config") ? config : !strcmp(into[i], "cache") ? cache : state;
                now_path = snx_format(lines[2 * i + 1], root);
                line = snx_format("%s was left by an older version: this one keeps it in %s\n", old_path, now_path);
                assert(!strncmp(at, line, strlen(line)));
                free(line);
                free(now_path);
                free(old_path);
            }
            /* in a box over the menu as it opens: each where it is (-) and where it would go (->, in ink: a proposal,
               not a diff's added green), a path too long for a row going on in the next, as many as fit */
            {
                int width = app.cols - 7, rows = 6, row, y, wraps = 0, selected = app.menu.selected;
                SDL_Color page = rgb(app.theme.page), ink = rgb(app.theme.ink), added = rgb(app.theme.added);
                static char paths[2][4200]; /* where each file is, and where it goes */
                int window_w, window_h, text_size = app.menu.text_size;
                app.left_shown = true;
                app.clock = 1;
                /* tall enough for all six, whatever the temporary folder's path, at the letters' size of now */
                assert(SDL_GetWindowSize(app.window, &window_w, &window_h));
                app.menu.text_size = app.scale;
                assert(SDL_SetWindowSize(app.window, window_w, 30 * 16 * app.scale));
                SDL_SyncWindow(app.window);
                render(&app);
                width = app.cols - 7;
                assert(app.rows >= 28 && width >= 57 && !same_colour(added, ink));
                for (at = app.left; *at; at = strchr(at, '\n') + 1) {
                    left_paths(at, paths[0], paths[1], sizeof paths[0]);
                    for (i = 0; i < 2; ++i) { /* each path's rows: the whole of it, in pieces */
                        rows += path_rows(paths[i], width - (i ? 3 : 2));
                        wraps += path_rows(paths[i], width - (i ? 3 : 2)) > 1;
                    }
                }
                assert(wraps >= 4 && rows <= app.rows - 2); /* the long game's paths wrap, and all six files fit */
                row = (app.rows - rows) / 2;
                colored_text_on(&app, 4, row + 1, "An older version left these files: move each from - to ->",
                                rgb(app.theme.warning), page);
                for (y = row + 3, at = app.left; *at; at = strchr(at, '\n') + 1) {
                    left_paths(at, paths[0], paths[1], sizeof paths[0]);
                    for (i = 0; i < 2; ++i)
                        for (const char* rest = paths[i]; *rest; ++y) { /* the whole path, a piece a row */
                            size_t n = gui_wrap(rest, width - (i ? 3 : 2));
                            line = snx_format("%s%.*s",
                                              rest == paths[i] ? (i ? "-> " : "- ")
                                              : i              ? "   "
                                                               : "  ",
                                              (int)n, rest);
                            colored_text_on(&app, 4, y, line, i ? ink : rgb(app.theme.failure), page);
                            free(line);
                            rest += n;
                        }
                }
                assert(y == row + rows - 3);
                colored_text_on(&app, 4, row + rows - 2, "- OK, nothing is moved", rgb(app.theme.focus), page);
                /* a window too short for them all: as many as fit, then how many more */
                {
                    int used = 0, shown = 0;
                    app.rows = 14;
                    for (at = app.left; *at; at = strchr(at, '\n') + 1, ++shown) {
                        int need;
                        left_paths(at, paths[0], paths[1], sizeof paths[0]);
                        need = path_rows(paths[0], width - 2) + path_rows(paths[1], width - 3);
                        if (shown && used + need > app.rows - 2 - 6) break;
                        used += need;
                    }
                    assert(shown > 0 && shown < 6);
                    render_left(&app);
                    row = (app.rows - (used + 6)) / 2;
                    line = snx_format("and %d more, which Share log lists", 6 - shown);
                    colored_text_on(&app, 4, row + 3 + used, line, rgb(app.theme.soft), page);
                    free(line);
                    colored_text_on(&app, 4, row + 4 + used, "- OK, nothing is moved", rgb(app.theme.focus), page);
                    app.menu.text_size = text_size;
                    assert(SDL_SetWindowSize(app.window, window_w, window_h));
                    SDL_SyncWindow(app.window);
                    layout(&app);
                }
                app.clock = 0;
                /* the wheel moves nothing under it, and leaves it open */
                {
                    SDL_Event wheel = {0};
                    wheel.type = SDL_EVENT_MOUSE_WHEEL;
                    wheel.wheel.integer_y = -2;
                    event(&app, &wheel);
                    assert(app.left_shown && app.menu.selected == selected);
                }
                /* a key closes it and does nothing else; nothing was moved */
                key(&app, SDLK_DOWN);
                assert(!app.left_shown && app.menu.selected == selected && snx_is_file(ini));
                /* gone while it shows, it closes */
                app.left_shown = true;
                assert(!snx_remove_tree(app.data_dir)); /* its gui/ too */
                app.left_at = 0;
                pump(&app);
                assert(!app.left_shown && !app.lefts && !app.left);
            }
            SDL_setenv_unsafe("XDG_CONFIG_HOME", "/nonexistent/snx-gui-test/config", 1);
            SDL_setenv_unsafe("XDG_CACHE_HOME", "/nonexistent/snx-gui-test/cache", 1);
            free(app.recent_file);
            app.recent_file = NULL;
            free(app.games_dir);
            app.games_dir = games;
            free(app.data_dir);
            app.data_dir = installed_dir;
            free(work);
            free(threads);
            free(log);
            free(ini);
            free(cache);
            free(config);
        }
#undef LONG
        /* and an empty state folder is made for the first run */
        assert(!snx_remove_tree(state));
        folder = state_folder();
        assert(folder && snx_is_dir(folder));
        free(folder);
        SDL_setenv_unsafe("XDG_DATA_HOME", "/nonexistent/snx-gui-test", 1);
        free(expected);
        free(run);
        free(recent);
        free(old);
        free(state);
        free(data);
    }
#endif
    /* the system's picker, as the Android menu opens it for a file to load or for the keys: a document with no path
       stays on the screen, and so does none, saying so, for the screen's messages to be read; a path loads */
    app.picked = SDL_RegisterEvents(1);
    assert(app.picked);
    app.menu.selected = GUI_KEYS;
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_KEYS);
    pick(&app, NULL);
    assert(app.menu.view == GUI_LOAD_KEYS && !strcmp(app.message, "No file was picked."));
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    app.menu.selected = GUI_SWITCH;
    key(&app, SDLK_RETURN);
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_DROP);
    pick(&app, "content://com.android.providers.downloads.documents/document/msf%3A12");
    assert(app.menu.view == GUI_LOAD_DROP && !app.menu.running && app.message[0]);
    pick(&app, NULL);
    assert(app.menu.view == GUI_LOAD_DROP && !strcmp(app.message, "No file was picked."));
    pick(&app, input);
    assert(app.menu.running && app.menu.selected == GUI_SWITCH);
    finish(&app, 0);
    button_map(&app);
    /* the button map in the theme's boxes: the tabs, the title big, the pad drawn in the box between it and the bar,
       which says the note a map it cannot read leaves */
    {
        GuiPlace p;
        GuiRect r;
        char keys[256];
        const SnxPadShape* first;
        int s = app.scale, cw = 8 * s, page = -1;
        app.theme = *gui_theme_builtin("default");
        for (int i = 0; i < app.settings.count; ++i)
            if (app.settings.values[i].kind == GUI_SETTING_PAGE) page = i;
        open_settings(&app);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        app.editor.row = page - app.first;
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_PAD && app.editor.note[0]); /* the map it cannot read, still set */
        render(&app);
        place_screen(&app, &p, "SETTINGS  /  CONTROLLERS  /  BUTTON MAP", screen_keys(&app, SAY_PAD, keys, sizeof keys),
                     0, GUI_SHOWS_TABS);
        r = inner(&app, p.list);
        first = drawn(&app, 0, SNX_PAD_SHAPE_BOX, -1);
        assert(first && first->x >= (float)r.x && first->y >= (float)r.y && first->y < (float)(r.y + r.h));
        text_px_is(&app, tab_rect(&app, p.tabs, GUI_TAB_SETTINGS).x + cw, p.tabs.y + 2 * s, "Settings", yellow, black);
        {
            char start[24];
            snprintf(start, sizeof start, "%s", app.editor.note); /* as far as the bar shows it at once */
            text_px_is(&app, p.bar.x + 3 * cw, p.bar.y + 2 * s, start, red, black);
        }
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_GROUP);
        leave_editor(&app);
        lines(&app, "default");
    }
    step_line();
    /* YoRHa's Settings: its groups' column wide enough for the last row with its cursor; a page of a group open, its
       name over its rows */
    {
        static const char list[] = "Display\tfullscreen\t0\tint\t0\t1\tStart in fullscreen\tFullscreen\t0=Off|1=On\n"
                                   "Display/Window\tscale\t1\tint\t1\t4\tThe window's scale\tScale\t\n";
        GuiFormBoxes f;
        GuiRect b;
        int s = app.scale, cw = 8 * s, row;
        gui_settings_free(&app.settings);
        memset(&app.settings, 0, sizeof app.settings);
        assert(!gui_settings_read(&app.settings, list));
        gui_settings_mark(&app.settings);
        app.theme = *gui_theme_builtin("yorha");
        app.theme.corners = app.theme.grid = app.theme.diagonals = false;
        open_settings(&app);
        app.editor.row = app.groups; /* the last row */
        render(&app);
        form_boxes(&app, &f);
        b = list_box(&f.left, app.groups);
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "\xfe Reset all to the defaults", rgb(app.theme.page),
                   rgb(app.theme.focus));
        for (row = 0; row < app.groups && strcmp(app.group_keys[row].key, "Display"); ++row) {}
        app.editor.row = row;
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_GROUP && app.editor.row == 0);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(app.page >= 0);
        render(&app);
        form_boxes(&app, &f);
        text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, app.settings.keys[app.page].key,
                   rgb(app.theme.page), rgb(app.theme.ink));
        leave_editor(&app);
        lines(&app, "default");
    }
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
