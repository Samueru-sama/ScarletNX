/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Controllers' button map: the pad drawn with what each control sends (pad_screen.h), as the game's menu has
   it, its preset the row gamepad_buttons and its own map the row gamepad_map */
#include "app.h"
static int setting_row(const App* app, const char* key) {
    int i;
    for (i = 0; i < app->settings.count; ++i)
        if (app->settings.values[i].key && !strcmp(app->settings.values[i].key, key)) return i;
    return -1;
}
/* a page of gamepad_map's, while gamepad_buttons has the presets' words */
bool gui_app_pad_page(const App* app, int row) {
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
void gui_app_open_pad(App* app, int row) {
    gui_app_leave_editor(app);
    app->editing = true;
    app->form = GUI_FORM_PAD;
    app->page = row;
    app->editor.note[0] = 0;
    pad_start(app);
    gui_app_render(app); /* SDL looks for pads for most of a second: the page shows first, saying so */
    app->pads_read = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!app->pads_read)
        snprintf(app->editor.note, sizeof app->editor.note, "Controllers cannot be read: %s", SDL_GetError());
    pad_attach(app);
}
void gui_app_pad_close(App* app) {
    if (app->gamepad) SDL_CloseGamepad(app->gamepad);
    app->gamepad = NULL;
    if (app->pads_read) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    app->pads_read = false;
}
/* what the page did: a preset or a map kept at once, as the Settings' other picks */
void gui_app_pad_did(App* app, int did) {
    GuiSettings* s = &app->settings;
    int map = setting_row(app, "gamepad_map");
    if (did == SNX_PAD_DID_LEAVE) {
        gui_app_leave_form(app);
        return;
    }
    if (did != SNX_PAD_DID_PRESET && did != SNX_PAD_DID_MAP) return;
    s->values[setting_row(app, "gamepad_buttons")].word = app->pad.in_use;
    if (did == SNX_PAD_DID_MAP) {
        snx_pad_map_write(&app->pad.own, s->values[map].text, sizeof s->values[map].text);
        app->editor.note[0] = 0; /* a map it could not read is replaced */
    }
    app->editor.dirty = true;
    gui_app_keep_settings(app);
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
        gui_app_pad_did(app, snx_pad_screen_press(&app->pad, control));
        return;
    }
    snx_pad_screen_map(&app->pad, &map);
    bit = map.bit[control];
    if (bit >= 0 && bit < SNX_PAD_BUTTONS && KEYS[bit] >= 0)
        gui_app_pad_did(app, snx_pad_screen_key(&app->pad, KEYS[bit]));
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
            gui_app_pad_did(app, snx_pad_screen_key(&app->pad, y ? (side ? SNX_PAD_KEY_DOWN : SNX_PAD_KEY_UP)
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
        gui_app_keep_settings(app);
        return;
    }
    if (!gui_app_menu_key_of(k, &key)) return;
    switch (key) {
        case GUI_UP: gui_app_pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_UP)); break;
        case GUI_DOWN: gui_app_pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_DOWN)); break;
        case GUI_LEFT: gui_app_pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_LEFT)); break;
        case GUI_RIGHT: gui_app_pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_RIGHT)); break;
        case GUI_SELECT: gui_app_pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_SELECT)); break;
        case GUI_CANCEL: gui_app_pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_BACK)); break;
        default: break;
    }
}
/* the page's events: keys, its pad's presses, pads plugged in and out; false for what the rest takes */
bool gui_app_pad_event(App* app, const SDL_Event* e) {
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
void gui_app_render_pad(App* app) {
    const GuiTheme* theme = &app->theme;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale;
    char title[160], keys[256], *c;
    GuiPlace place;
    int i;
    SDL_SetRenderDrawColor(app->renderer, theme->page.r, theme->page.g, theme->page.b, 255);
    SDL_RenderClear(app->renderer);
    snprintf(title, sizeof title, "SETTINGS  /  %s  /  %s", app->group_keys[app->group].key,
             app->settings.keys[app->page].key);
    for (c = title; *c; ++c) *c = (char)SDL_toupper((unsigned char)*c);
    if (boxed(app)) { /* the theme's boxes round it: the pad drawn in the box between the title and the bar */
        GuiRect r;
        gui_app_place_screen(app, &place, title, gui_app_screen_keys(app, SAY_PAD, keys, sizeof keys), 0,
                             GUI_SHOWS_TABS);
        gui_app_chrome(app, &place);
        gui_app_title_box(app, place.title, theme->head == GUI_STYLE_NONE ? "" : title);
        r = inner(app, place.list);
        pad_sync(app);
        snx_pad_screen_lay(&app->pad, (float)r.x, (float)r.y, (float)r.w, (float)r.h, cw, ch, &app->pad_drawing);
    } else {
        if (theme->head == GUI_STYLE_BAR) gui_app_band(app, 0, 1, theme->ink);
        text(app, 1, 0, title, rgb(theme->head == GUI_STYLE_BAR ? theme->page : theme->ink));
        text(app, 1, 1, SAY_PAD, rgb(theme->ink));
        if (app->editor.note[0])
            text(app, 1, 2, app->editor.note, rgb(theme->failure));
        else if (!app->pads_read)
            text(app, 1, 2, "Looking for controllers...", rgb(theme->soft));
        pad_sync(app);
        snx_pad_screen_lay(&app->pad, cw, 3 * ch, (float)(app->cols - 2) * cw, (float)SDL_max(1, app->rows - 4) * ch,
                           cw, ch, &app->pad_drawing);
    }
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
    if (!boxed(app)) return;
    gui_app_vignette(app);
    gui_app_bar(app, place.bar,
                app->editor.note[0] ? app->editor.note
                : !app->pads_read   ? "Looking for controllers..."
                                    : "",
                rgb(app->editor.note[0] ? theme->failure : theme->soft), keys);
}
