/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The screens in boxes, where the theme says so ([body] boxes): place.c puts each box, and nothing goes over
   another. The menu, Games, the screen a file is picked on, a run, the Settings and the theme editor, the boxes over
   them, and the pointer on each */
#include "app.h"
#ifdef SDL_PLATFORM_ANDROID
#define SAY_MENU_KEYS SAY_MENU
#else
#define SAY_MENU_KEYS SAY_MENU " ESC: exit."
#endif
static int tab_hit(App* app, float x, float y);
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
void gui_app_bar(App* app, GuiRect r, const char* about, SDL_Color color, const char* keys) {
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
void gui_app_title_box(App* app, GuiRect r, const char* title) {
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
    gui_app_disk_text(app, disk, sizeof disk);
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
static const char* const tab_names[GUI_TABS] = {"Menu", "Games", "Settings"};
int gui_app_tab_shown(const App* app) {
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
    int i, s = app->scale, shown = gui_app_tab_shown(app);
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
const char* gui_app_screen_keys(const App* app, const char* keys, char* out, size_t size) {
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
/* the page's own lines behind the boxes, in the dim colour: a faint grid, thin diagonals */
static void backdrop(App* app) {
    const GuiTheme* t = &app->theme;
    int width, height, s = app->scale, x, y;
    if (!t->grid && !t->diagonals) return;
    area(app, &width, &height);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, t->dim.r, t->dim.g, t->dim.b, 72);
    for (x = 0; t->grid && x < width; x += 6 * s) SDL_RenderLine(app->renderer, (float)x, 0, (float)x, (float)height);
    for (y = 0; t->grid && y < height; y += 6 * s) SDL_RenderLine(app->renderer, 0, (float)y, (float)width, (float)y);
    SDL_SetRenderDrawColor(app->renderer, t->dim.r, t->dim.g, t->dim.b, 160);
    for (x = -height; t->diagonals && x < width; x += 112 * s)
        SDL_RenderLine(app->renderer, (float)x, 0, (float)(x + height), (float)height);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
}
/* the screen's corners darker over all of it, as an old monitor's, where the theme says so: from the middle out to
   the corners, the edges' middles darkened less */
void gui_app_vignette(App* app) {
    /* around the edge from the top left corner, in halves of the screen, and how dark */
    static const int round[8][3] = {{0, 0, 30}, {1, 0, 8}, {2, 0, 30}, {2, 1, 8},
                                    {2, 2, 30}, {1, 2, 8}, {0, 2, 30}, {0, 1, 8}};
    SDL_Vertex v[9];
    int order[24], width, height, i;
    if (!app->theme.corners) return;
    area(app, &width, &height);
    memset(v, 0, sizeof v);
    v[0].position.x = (float)width / 2; /* black, its alpha 0 in the middle */
    v[0].position.y = (float)height / 2;
    for (i = 0; i < 8; ++i) {
        v[i + 1].position.x = (float)(round[i][0] * width) / 2;
        v[i + 1].position.y = (float)(round[i][1] * height) / 2;
        v[i + 1].color.a = (float)round[i][2] / 100;
        order[3 * i] = 0;
        order[3 * i + 1] = i + 1;
        order[3 * i + 2] = (i + 1) % 8 + 1;
    }
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(app->renderer, NULL, v, 9, order, 24);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
}
/* what a screen in boxes draws before its own: the page's lines, the edges, the tabs */
void gui_app_chrome(App* app, const GuiPlace* p) {
    backdrop(app);
    edge(app, p->edge_top);
    edge(app, p->edge_bottom);
    draw_tabs(app, p->tabs);
}
/* a screen's boxes: the tabs, its title, the bar for its keys, a list asking for list_cols columns, three fifths of the
   width at most (0: one box, the whole width), and the status box, as shows says */
void gui_app_place_screen(App* app, GuiPlace* p, const char* title, const char* keys, int list_cols, int shows) {
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
    gui_app_bar(app, r, app->message[0] ? app->message : about,
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
    gui_app_place_screen(app, &m->place, boxed_title(app, menu_screen(app)),
                         gui_app_screen_keys(app, SAY_MENU_KEYS, keys, sizeof keys),
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
        char* in_use = gui_app_keys_in_use(app);
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
void gui_app_draw_menu_boxed(App* app) {
    GuiMenuBoxes m;
    GuiEntry focus;
    char label[160], keys[256];
    int i;
    menu_boxes(app, &m);
    focus = menu_focus(&m);
    gui_app_chrome(app, &m.place);
    gui_app_title_box(app, m.place.title, boxed_title(app, menu_screen(app)));
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
    gui_app_vignette(app);
    bar_for(app, m.place.bar, entry_about(app, focus), gui_app_screen_keys(app, SAY_MENU_KEYS, keys, sizeof keys));
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
    *r = over_rect(app, SDL_max((int)SDL_utf8strlen(gui_app_ask_question(app)), 16),
                   cell_h(app) + 2 * (item + 6 * s) + 12 * s);
    list_place(app, choices,
               box_rect(r->x + cell_w(app), r->y + cell_h(app) + 10 * s, r->w - 2 * cell_w(app), 2 * (item + 6 * s)), 2,
               app->confirm.selected);
}
void gui_app_ask_boxed(App* app) {
    static const char* const choices[2][2] = {{"Keep going", "Abort"}, {"Stay", "Exit"}}; /* by exit, GUI_CONFIRM_* */
    GuiRect r;
    GuiList l;
    char label[64];
    int i;
    ask_boxes(app, &r, &l);
    over_box(app, r, NULL);
    text_in(app, inner(app, r), r.x + cell_w(app), r.y + 4 * app->scale, gui_app_ask_question(app),
            rgb(app->theme.warning));
    for (i = 0; i < 2; ++i) {
        snprintf(label, sizeof label, "%s%s", app->theme.marker, choices[app->confirm.exit][i]);
        list_item(app, &l, i, label, app->confirm.selected == i, false);
    }
}
/* the files an older version left, over the menu: each where it is (-) and where this one would keep it (->), as many
   as fit, how many more, and OK, focused: any key closes it */
void gui_app_left_boxed(App* app) {
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
        gui_app_left_paths(at, old, now, sizeof old);
        need = wrap_rows(old, cols - 2, 1) + wrap_rows(now, cols - 3, 1);
        if (shown && used + need > room) break;
        used += need;
    }
    more = app->lefts - shown;
    r = over_rect(app, cols, head_h(app) + (used + (more > 0)) * cell_h(app) + item + 16 * s);
    y = over_box(app, r, "An older version left these files: move each from - to ->") + 2 * s;
    text = inner(app, r);
    for (i = 0, at = app->left; i < shown; ++i, at = strchr(at, '\n') + 1) {
        gui_app_left_paths(at, old, now, sizeof old);
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
    gui_app_place_screen(app, p, boxed_title(app, GUI_SCREEN_GAMES),
                         gui_app_screen_keys(app, SAY_GAMES, keys, sizeof keys),
                         longest + 2 + (int)SDL_utf8strlen(app->theme.marker) + (int)SDL_utf8strlen(app->theme.cursor),
                         GUI_SHOWS_STATUS | GUI_SHOWS_TABS);
}
/* the games a page of the boxed list holds: those that fit under Back */
size_t gui_app_games_page(App* app) {
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
void gui_app_draw_games_boxed(App* app) {
    GuiGamesBoxes g;
    GuiRect r;
    char label[400], line[1100], keys[256];
    const char* about;
    size_t i, items = gui_browser_items(&app->browser);
    SDL_Color soft = rgb(app->theme.soft), ink = rgb(app->theme.ink);
    int y;
    games_boxes(app, &g);
    gui_app_chrome(app, &g.place);
    gui_app_title_box(app, g.place.title, boxed_title(app, GUI_SCREEN_GAMES));
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
    gui_app_vignette(app);
    about = app->browser.selected ? app->games.paths[g.start + app->browser.selected - 1] : "Back to the menu";
    bar_for(app, g.place.bar, about, gui_app_screen_keys(app, SAY_GAMES, keys, sizeof keys));
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
    gui_app_place_screen(app, &k->place, boxed_title(app, menu_screen(app)),
                         gui_app_screen_keys(app, SAY_KEYS_HOW, keys, sizeof keys), 0, GUI_SHOWS_TABS);
    r = inner(app, k->place.list);
    list_place(
        app, &k->back,
        box_rect(r.x, k->place.list.y + k->place.list.h - item - 4 * app->scale, SDL_min(r.w, 24 * cell_w(app)), item),
        1, -1);
    k->prompt = k->place.list.y + k->place.list.h / 2;
}
void gui_app_draw_pick_boxed(App* app) {
    GuiPickBoxes k;
    GuiRect r;
    bool keys = app->menu.view == GUI_LOAD_KEYS;
    char line[1100], hints_line[256], *in_use = keys ? gui_app_keys_in_use(app) : NULL;
    int y;
    pick_boxes(app, &k);
    gui_app_chrome(app, &k.place);
    gui_app_title_box(app, k.place.title, boxed_title(app, menu_screen(app)));
    y = pane(app, k.place.list, gui_items[keys ? GUI_KEYS : app->menu.active]);
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
    gui_app_vignette(app);
    snprintf(line, sizeof line, "%s: " SAY_PICK_BOX, gui_items[keys ? GUI_KEYS : app->menu.active]);
    bar_for(app, k.place.bar, line, gui_app_screen_keys(app, SAY_KEYS_HOW, hints_line, sizeof hints_line));
}
/* a run of the console layout in boxes: the driver's output in the wide box, beside it what runs, its file and how it
   ended, the status under them; the ESC line in the bar, as the theme's prompt frames it */
void gui_app_run_place(App* app, GuiPlace* p) {
    int width, height;
    area(app, &width, &height);
    gui_app_place_screen(app, p, boxed_title(app, screen(app)), "", width / cell_w(app), GUI_SHOWS_STATUS);
}
/* the output box's cells under its head */
void gui_app_run_console(App* app, const GuiPlace* p, int* cols, int* rows) {
    GuiRect r = inner(app, p->list);
    *cols = SDL_max(1, r.w / cell_w(app));
    *rows = SDL_max(1, (p->list.h - head_h(app) - 4 * app->scale) / cell_h(app));
}
void gui_app_draw_run_boxed(App* app) {
    GuiPlace p;
    GuiRect r, text;
    const GuiTheme* t = &app->theme;
    int y, x, i, s = app->scale, cols, rows;
    GuiPromptLine line =
        app->finished ? (app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE) : GUI_PROMPT_ABORTING;
    gui_app_run_place(app, &p);
    gui_app_chrome(app, &p);
    gui_app_title_box(app, p.title, boxed_title(app, screen(app)));
    y = pane(app, p.list, "Output");
    r = inner(app, p.list);
    gui_app_run_console(app, &p, &cols, &rows);
    SDL_SetTextureColorMod(app->font, t->ink.r, t->ink.g, t->ink.b);
    for (i = 0; i < SDL_min(rows, app->console.rows); ++i)
        for (x = 0; x < SDL_min(cols, app->console.cols); ++x)
            if (app->console.cells[i][x] != ' ')
                glyph(app, (float)(r.x + x * cell_w(app)), (float)(y + i * cell_h(app)), app->console.cells[i][x]);
    y = pane(app, p.side, gui_items[app->menu.selected]);
    text = inner(app, p.side);
    text = box_rect(text.x, y, text.w, p.side.y + p.side.h - y);
    y += wrap_in(app, text, text.x, y, app->file ? app->file : "", 1, rgb(t->ink)) * cell_h(app);
    if (app->finished)
        wrap_in(app, text, text.x, y + cell_h(app) / 2, app->message, 0,
                rgb(app->abnormal_exit ? t->failure : t->focus));
    else if (app->aborting)
        wrap_in(app, text, text.x, y + cell_h(app) / 2, "Aborting process tree...", 0, rgb(t->failure));
    status_box(app, p.status);
    gui_app_vignette(app);
    gui_app_bar(app, p.bar, app->file ? app->file : "", rgb(t->soft), "");
    text = box_rect(p.bar.x + 3 * cell_w(app), p.bar.y, p.bar.w - 4 * cell_w(app), p.bar.h);
    if (app->finished || app->aborting)
        prompt_in(app, text, p.bar.y + 2 * s + cell_h(app), line, app->finished ? SAY_DONE : "Aborting process tree...",
                  rgb(t->failure));
    else
        text_in(app, text, text.x + text.w - (int)strlen(SAY_ABORT) * cell_w(app), p.bar.y + 2 * s + cell_h(app),
                SAY_ABORT, rgb(t->failure));
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
    return gui_app_screen_keys(app,
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
/* the line at the foot of a group's rows, about what they hold, NULL on the screens with none */
static const char* form_note(const App* app) {
    if (app->form != GUI_FORM_SETTINGS && app->form != GUI_FORM_GROUP) return NULL;
    return app->settings.count > GUI_SETTINGS_OWN ? "For the next runs. [default]: as the game's .ini says"
                                                  : "The runtime listed no settings";
}
static void form_boxes(App* app, GuiFormBoxes* f) {
    const char* note = form_note(app);
    char title[96], keys[256];
    int rows;
    int theme = app->form == GUI_FORM_THEME, width, height, g, longest = (int)strlen("Reset all to the defaults");
    area(app, &width, &height);
    for (g = 0; g < app->groups; ++g) longest = SDL_max(longest, (int)strlen(app->group_keys[g].key));
    gui_app_place_screen(app, &f->place, form_title(app, title, sizeof title), form_keys(app, keys, sizeof keys),
                         theme ? width / cell_w(app)
                               : longest + 2 + (int)SDL_utf8strlen(app->theme.marker) +
                                     (int)SDL_utf8strlen(app->theme.cursor),
                         GUI_SHOWS_TABS);
    f->rows = &app->editor;
    if (app->form == GUI_FORM_SETTINGS) { /* the focused group's rows, to see before opening it */
        int g = app->editor.row;
        memset(&f->peek, 0, sizeof f->peek);
        if (g == 0) {
            gui_app_values(app);
            gui_editor_form(&f->peek, gui_app_form_keys, GUI_APP_ROWS, &app->app_form, NULL);
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
    rows = note ? wrap_rows(note, inner(app, f->place.side).w / cell_w(app), 0) : 1; /* the note whole */
    f->info = box_rect(f->place.side.x, f->place.side.y + f->place.side.h - rows * cell_h(app) - 4 * app->scale,
                       f->place.side.w, rows * cell_h(app) + 2 * app->scale);
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
    else if (k && e->object == (char*)app->settings.values) /* a setting: a pick, else its default and [default] */
        gui_settings_shown(&app->settings, (int)(k - app->settings.keys), value, sizeof value);
    else
        gui_editor_value(e, r, value, sizeof value);
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
void gui_app_draw_form_boxed(App* app) {
    GuiFormBoxes f;
    char title[96], keys[256], line[1100];
    const char* about;
    SDL_Color color;
    int r, labels, theme = app->form == GUI_FORM_THEME;
    SDL_SetRenderDrawColor(app->renderer, app->theme.page.r, app->theme.page.g, app->theme.page.b, 255);
    SDL_RenderClear(app->renderer);
    form_boxes(app, &f);
    gui_app_chrome(app, &f.place);
    gui_app_title_box(app, f.place.title, form_title(app, title, sizeof title));
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
        if (w > 0 && gui_app_preview_texture(app)) {
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
             app->form == GUI_FORM_FOLDERS                   ? "Game folders"
             : app->form == GUI_FORM_GROUP && app->page >= 0 ? app->settings.keys[app->page].key
             : g < app->groups                               ? app->group_keys[g].key
                                                             : "Reset all to the defaults");
        labels = form_labels(f.rows);
        for (r = f.right.first; r < f.right.first + f.right.shown; ++r)
            form_row(app, &f.right, f.rows, r, labels, f.rows == &app->editor && r == app->editor.row,
                     app->form == GUI_FORM_FOLDERS && r == app->folder_armed && r);
        list_marks(app, &f.right);
        if (form_note(app))
            wrap_in(app, inner(app, f.info), f.info.x + cell_w(app), f.info.y, form_note(app), 0, rgb(app->theme.soft));
    }
    gui_app_vignette(app);
    about = form_about(app, &color);
    gui_app_bar(app, f.place.bar, about, color, form_keys(app, keys, sizeof keys));
}
/* a row of a form under the pointer: focused, or on the focused one Select, as a key would */
static void form_row_pointer(App* app, int r) {
    if (r == app->editor.row && !app->editor.typing)
        gui_app_editor_key(app, GUI_SELECT);
    else if (r != app->editor.row) {
        if (app->editor.typing) gui_app_editor_key(app, GUI_CANCEL);
        app->editor.row = r;
    }
}
/* a click on a form in boxes: a tab; a row of the list or of the right box; a group of the list opens it from a
   group's rows; the theme editor's preview switches */
void gui_app_form_boxed_pointer(App* app, float x, float y, bool touch) {
    GuiFormBoxes f;
    int i;
    if ((i = tab_hit(app, x, y)) >= 0) {
        gui_app_go_tab(app, i);
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
        if (app->editor.typing) gui_app_editor_key(app, GUI_CANCEL);
        gui_app_keep_settings(app);
        gui_app_leave_editor(app);
        gui_app_settings_top(app, i); /* another group: from its row, as Enter there */
        if (i < app->groups) gui_app_open_group(app, i);
        return;
    }
    if ((i = list_hit(&f.right, x, y, touch)) < 0) return;
    if (app->form == GUI_FORM_SETTINGS) { /* a row of the focused group: it opens, on that row */
        if (app->editor.row >= app->groups) return;
        gui_app_open_group(app, app->editor.row);
        app->editor.row = SDL_min(i, gui_editor_rows(&app->editor) - 1);
        return;
    }
    form_row_pointer(app, i);
}
/* the pointer on the question in boxes, at pixel x, y: a choice under it takes the focus, a click takes it; a click
   off the box keeps going */
void gui_app_ask_boxed_pointer(App* app, float x, float y, bool click) {
    GuiRect r;
    GuiList l;
    int i;
    ask_boxes(app, &r, &l);
    if ((i = list_hit(&l, x, y, true)) >= 0) {
        app->confirm.selected = i;
        if (click && gui_confirm_key(&app->confirm, GUI_SELECT)) gui_app_confirmed(app);
    } else if (click && (x < (float)r.x || x >= (float)(r.x + r.w) || y < (float)r.y || y >= (float)(r.y + r.h)))
        gui_confirm_key(&app->confirm, GUI_CANCEL);
}
/* the tab at pixel x, y, -1 for none */
static int tab_hit(App* app, float x, float y) {
    GuiPlace p;
    int i;
    if (!app->theme.tabs) return -1;
    gui_app_place_screen(app, &p, "", "", 0, GUI_SHOWS_TABS);
    for (i = 0; i < GUI_TABS; ++i) {
        GuiRect r = tab_rect(app, p.tabs, i);
        if (x >= (float)r.x && x < (float)(r.x + r.w) && y >= (float)r.y && y < (float)(r.y + r.h)) return i;
    }
    return -1;
}
/* the pointer on a screen in boxes, at pixel x, y: the box under it takes the focus, and a click selects it; on the
   screen a file is picked on, a click on the box the waiting line is in opens the picker */
void gui_app_boxed_pointer(App* app, float x, float y, bool click, bool touch) {
    GuiMenuBoxes m;
    int i;
    if ((i = tab_hit(app, x, y)) >= 0) {
        if (click) gui_app_go_tab(app, i);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        GuiGamesBoxes g;
        games_boxes(app, &g);
        if ((i = list_hit(&g.list, x, y, touch)) < 0) return;
        app->browser.selected = (size_t)i;
        if (click) gui_app_menu_key(app, GUI_SELECT);
        return;
    }
    if (pick_screen(app)) {
        GuiPickBoxes k;
        pick_boxes(app, &k);
        if (click && list_hit(&k.back, x, y, touch) == 0)
            gui_app_menu_key(app, GUI_CANCEL);
        else if (click && y >= (float)k.place.list.y && y < (float)k.back.at.y && x >= (float)k.place.list.x &&
                 x < (float)(k.place.list.x + k.place.list.w))
            gui_app_menu_key(app, GUI_SELECT); /* the box the waiting line is in */
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
    if (click) gui_app_menu_key(app, GUI_SELECT);
}
/* the menu's rows go past the screen: its letters are too big for it */
bool gui_app_menu_scrolls(App* app) {
    GuiMenuBoxes m;
    if (!boxed(app)) return menu_row_in_full(app, GUI_EXIT) > menu_last_row(app);
    gui_app_layout(app);
    menu_boxes(app, &m);
    return m.list.shown < m.list.count;
}
