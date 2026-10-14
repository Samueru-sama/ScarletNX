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
#include "steps.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

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
    GuiBanner banner;
    GuiSteps steps;
    Uint64 clock;                /* fixed time for tests; 0 is the real clock */
    Uint64 fade_ms, leaving_ms; /* a screen fading in from black since; the run fading out since */
    Uint64 entering_ms;         /* the menu fading out since, before a run fades in */
    const char* drivers;
    char *cwd, *file, *games_dir, *keys_path;
    char* themes_dirs[2]; /* themes/ in the working directory, then beside the GUI */
    char message[512];
    int cols, rows, scale, log_top;
    Uint64 sampled;
    bool has_output, finished, aborting, abnormal_exit, native;
} App;
static const char* const items[] = {"Load Switch NSP/XCI/NRO", "Load Linux ARM ELF", "Setup device keys",
                                   "Fullscreen", "Enable libgccjit", "Enable gdbstub", "Theme", "Exit"};
static SDL_Color rgb(GuiRgb c) { return (SDL_Color){c.r, c.g, c.b, 255}; }
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
/* a line that waits on the user: plain at x, or framed, placed and blinking as the theme's prompt says */
static void prompt(App* app, GuiPromptLine line, int x, int y, const char* value, SDL_Color color) {
    const GuiPrompt* p = &app->theme.prompt;
    if (!p->lines[line]) {
        text(app, x, y, value, color);
        return;
    }
    if (p->blink && app->theme.blink_ms && (now_ms(app) / app->theme.blink_ms) % 2) return;
    char framed[160];
    snprintf(framed, sizeof framed, "%s%s%s", p->open, value, p->close);
    int width = (int)SDL_utf8strlen(framed);
    if (p->align == GUI_ALIGN_CENTER) x = (app->cols - width) / 2;
    if (p->align == GUI_ALIGN_RIGHT) x = app->cols - 1 - width;
    text(app, SDL_max(0, x), y, framed, color);
}
/* rows of the whole width, as a band behind text */
static void band(App* app, int row, int rows, GuiRgb c) {
    int width, height;
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    SDL_FRect r = {0, (float)(row * 16 * app->scale), (float)width, (float)(rows * 16 * app->scale)};
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(app->renderer, &r);
}
/* a line between two rows, across the text area */
static void rule(App* app, int row) {
    SDL_FRect r = {(float)(8 * app->scale), (float)(row * 16 * app->scale - app->scale),
                   (float)((app->cols - 2) * 8 * app->scale), (float)app->scale};
    SDL_SetRenderDrawColor(app->renderer, app->theme.soft.r, app->theme.soft.g, app->theme.soft.b, 255);
    SDL_RenderFillRect(app->renderer, &r);
}
/* a selectable line: activated, focused (in the focus colour, or on a band) or plain */
static void entry(App* app, int x, int y, const char* label, bool activated, bool focused) {
    if (focused && app->theme.focus_bar) {
        band(app, y, 1, app->theme.focus);
        text(app, x, y, label, rgb(app->theme.page));
    } else
        text(app, x, y, label, rgb(activated ? app->theme.active : focused ? app->theme.focus : app->theme.ink));
}
static GuiScreen screen(const App* app) {
    if (app->menu.running || app->finished) return app->native || app->banner.seen ? GUI_SCREEN_LOADING
                                                                                    : GUI_SCREEN_COMPILING;
    return app->menu.view == GUI_LOAD_GAMES ? GUI_SCREEN_GAMES
           : app->menu.view == GUI_LOAD_KEYS ? GUI_SCREEN_KEYS
                                             : GUI_SCREEN_MENU;
}
static void next_theme(App* app) {
    const char* const* dirs = (const char* const*)app->themes_dirs;
    gui_themes_scan(&app->themes, dirs, 2);
    size_t i = 0;
    while (i < app->themes.count && strcmp(app->themes.ids[i], app->theme.id)) ++i;
    if (app->themes.count) gui_theme_load(&app->theme, dirs, 2, app->themes.ids[(i + 1) % app->themes.count]);
}
static int menu_spacing(const App* app) { return app->rows >= 30 ? 2 : 1; }
static int menu_row(const App* app, int item) {
    return 4 + item * menu_spacing(app) +
           (app->menu.view != GUI_LOAD_NONE && item > app->menu.active ? 2 : 0);
}
static void menu_label(const App* app, int item, char* label, size_t size) {
    if (item == GUI_FULLSCREEN || item == GUI_GCCJIT || item == GUI_GDBSTUB) {
        bool enabled = item == GUI_FULLSCREEN ? app->menu.fullscreen
                       : item == GUI_GCCJIT  ? app->menu.gccjit : app->menu.gdbstub;
        snprintf(label, size, "%s[%s] %s", app->theme.marker, enabled ? "ON" : "OFF", items[item]);
    } else if (item == GUI_THEME) {
        snprintf(label, size, "%s%s: %s", app->theme.marker, items[item], app->theme.name);
    } else {
        snprintf(label, size, "%s%s", app->theme.marker, items[item]);
    }
}
/* still the menu, fading out, while a run starts */
static bool entering(const App* app) {
    return app->entering_ms && now_ms(app) < app->entering_ms + app->theme.fade_out_ms;
}
static void layout(App* app) {
    int width, height;
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    app->scale = SDL_min(width / 640, height / 400);
    if (app->scale < 1) app->scale = 1;
    app->cols = SDL_min(width / (8 * app->scale), GUI_MAX_COLS);
    app->rows = SDL_min(height / (16 * app->scale), GUI_MAX_ROWS);
    bool run = (app->menu.running || app->finished) && !entering(app);
    app->log_top = run ? 4 : menu_row(app, GUI_EXIT) + 6;
    gui_browser_resize(&app->browser, (size_t)SDL_max(1, app->rows - 10));
    int log_rows = SDL_max(1, app->rows - app->log_top - 4);
    gui_console_resize(&app->console, SDL_max(1, app->cols - 2), log_rows);
}
/* text from a pixel position, chars at most, for the lines that slide or fade in */
static float text_px(App* app, float x, float y, const char* value, size_t chars, SDL_Color color, float alpha) {
    float w = 8.0f * app->scale, h = 16.0f * app->scale, limit = (float)(app->cols - 1) * w;
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(app->font, (Uint8)(alpha * 255));
    for (size_t i = 0; *value && i < chars; ++i, x += w) {
        unsigned char c = gui_glyph(SDL_StepUTF8(&value, NULL));
        if (x + w > limit) continue;
        SDL_FRect source = {(float)((c % 16) * 8), (float)((c / 16) * 16), 8, 16}, destination = {x, y, w, h};
        SDL_RenderTexture(app->renderer, app->font, &source, &destination);
    }
    SDL_SetTextureAlphaMod(app->font, 255);
    return x;
}
static void fill(App* app, float x, float y, float w, float h, GuiRgb c, Uint8 alpha) {
    SDL_FRect r = {x, y, w, h};
    SDL_SetRenderDrawBlendMode(app->renderer, alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, alpha);
    SDL_RenderFillRect(app->renderer, &r);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
}
/* an arc of a circle, from and sweep in degrees, thickness pixels inward */
static void arc(App* app, float cx, float cy, float r, float from, float sweep, float thickness, GuiRgb c) {
    SDL_FPoint p[49];
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, 255);
    for (float t = 0; t < thickness; t += 1.0f) {
        for (int i = 0; i <= 48; ++i) {
            float a = (from + sweep * (float)i / 48) * SDL_PI_F / 180;
            p[i] = (SDL_FPoint){cx + (r - t) * SDL_cosf(a), cy + (r - t) * SDL_sinf(a)};
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
    const char* name = app->file ? app->file : "";
    bool from_file = true;
    for (int i = 0; i < app->steps.count; ++i)
        if (!strcmp(app->steps.step[i].label, "LAUNCH")) {
            name = app->steps.step[i].detail;
            from_file = false;
        }
    for (const char* q = name; *q; ++q)
        if (*q == '/' || *q == '\\') name = q + 1;
    size_t n = 0;
    const char* dot = from_file ? strrchr(name, '.') : NULL;
    for (const char* q = name; *q && q != dot && n + 1 < size; ++q)
        out[n++] = *q >= 'a' && *q <= 'z' ? (char)(*q - 32) : *q;
    out[n] = 0;
}
/* a run as a loading screen: the title, the name and the ring; the steps typed down the page, their status last;
   the progress bar and its percentage */
/* a step's line: its label, its detail in capitals and its status */
static size_t step_runs(const GuiStep* s, char run[3][112], size_t length[3]) {
    snprintf(run[0], 112, "%s:", s->label);
    snprintf(run[1], 112, " %s", s->detail);
    for (char* q = run[1]; *q; ++q)
        if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    if (s->state == GUI_STEP_FAILED)
        snprintf(run[2], 112, "...FAILED");
    else if (s->state == GUI_STEP_DONE)
        snprintf(run[2], 112, "...OK");
    else if (s->total)
        snprintf(run[2], 112, " %u / %u", s->value, s->total);
    else
        run[2][0] = 0;
    size_t total = 0;
    for (int r = 0; r < 3; ++r) total += length[r] = SDL_utf8strlen(run[r]);
    return total;
}
/* a row cut to the columns it has: a step's detail, keeping its label and state, or a line, ending in "..." */
static void clip_row(char run[3][112], size_t length[3], bool note, int room) {
    size_t total = length[0] + length[1] + length[2];
    if (room < 8 || total <= (size_t)room) return;
    int cut = note ? 0 : 1;
    size_t marker = note ? 3 : 0, other = total - length[cut];
    size_t left = (size_t)room > other + marker ? (size_t)room - other - marker : 0;
    const char* end = run[cut];
    for (size_t i = 0; i < left; ++i) SDL_StepUTF8(&end, NULL);
    size_t at = (size_t)(end - run[cut]);
    snprintf(run[cut] + at, 112 - at, "%s", note ? "..." : "");
    length[cut] = left + marker;
}
/* a run as a loading screen: the title, the name and the ring; the steps typed down the page, their status last;
   the progress bar and its percentage */
static void render_steps(App* app) {
    const GuiTheme* theme = &app->theme;
    const GuiPalette* colors = &theme->run_colors;
    Uint64 now = now_ms(app);
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale;
    int width, height;
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    if (theme->texture)
        for (int y = 3; y < height; y += 4) fill(app, 0, (float)y, (float)width, 1, (GuiRgb){255, 255, 255}, 14);
    SDL_Color ink = rgb(colors->ink), soft = rgb(colors->soft), failure = rgb(colors->failure);
    char name[96], head[128];
    run_name(app, name, sizeof name);
    int col = theme->ring == GUI_RING_LEFT ? 4 : 1;
    const char* title = theme->title[screen(app)];
    int name_col = col + (int)SDL_utf8strlen(title) + (title[0] ? 1 : 0);
    if (theme->ring == GUI_RING_LEFT) ring(app, cw * 2, ch * 1.5f);
    if (theme->ring == GUI_RING_RIGHT) ring(app, (float)width - cw * 2, ch * 1.5f);
    if (theme->ring == GUI_RING_NEAR_TEXT) /* after the name and the dots' whole width, so it stays put */
        ring(app, cw * (float)(name_col + (int)SDL_utf8strlen(name) + 4) + 7.0f * (float)app->scale, ch * 1.5f);
    text(app, col, 1, title, ink);
    snprintf(head, sizeof head, "%s%s", name, dots(app));
    text(app, name_col, 1, head, soft);
    if (app->finished) text(app, 1, 2, app->message, app->abnormal_exit ? failure : soft);
    /* the rows: each step, then every line it kept when detailed, folded once it is done when collapse says so;
       each ends before the right edge, with room for a step's dots and the cursor */
    enum { ROWS = GUI_STEPS_MAX + GUI_NOTES_MAX };
    static char runs[ROWS][3][112];
    static size_t lengths[ROWS][3], typed[ROWS];
    static const GuiStep* owner[ROWS];
    static bool is_note[ROWS];
    static Uint64 shown[ROWS];
    Uint64 since = app->fade_ms; /* a line that came while the menu faded out comes in with the run */
    int rows = 0, last_step = -1;
    for (int i = 0; i < app->steps.count; ++i) {
        const GuiStep* s = &app->steps.step[i];
        owner[rows] = s;
        is_note[rows] = false;
        shown[rows] = SDL_max(s->shown_ms, since);
        step_runs(s, runs[rows], lengths[rows]);
        clip_row(runs[rows], lengths[rows], false, app->cols - 6);
        last_step = rows++;
        if (!theme->detailed || (theme->collapse && s->state == GUI_STEP_DONE)) continue;
        const GuiNote* notes[GUI_NOTES_MAX];
        int n = gui_step_notes(&app->steps, s, notes, GUI_NOTES_MAX);
        for (int k = 0; k < n; ++k, ++rows) {
            owner[rows] = s;
            is_note[rows] = true;
            shown[rows] = SDL_max(notes[k]->shown_ms, since);
            snprintf(runs[rows][0], sizeof runs[rows][0], "%s", notes[k]->text);
            runs[rows][1][0] = runs[rows][2][0] = 0;
            lengths[rows][0] = SDL_utf8strlen(runs[rows][0]);
            lengths[rows][1] = lengths[rows][2] = 0;
            clip_row(runs[rows], lengths[rows], true, app->cols - 5);
        }
    }
    /* when each row types: async, each from when it came; sync, one after the other, a finished step's faster */
    Uint64 free_at = 0;
    int typing = -1;
    for (int r = 0; r < rows; ++r) {
        size_t total = lengths[r][0] + lengths[r][1] + lengths[r][2];
        typed[r] = total;
        if (theme->line_in != GUI_LINE_TYPING || !theme->typing_ms) continue;
        float per = (float)theme->typing_ms / (theme->typing_sync && owner[r]->state != GUI_STEP_ACTIVE ? 3.0f : 1.0f);
        Uint64 start = theme->typing_sync ? SDL_max(shown[r], free_at) : shown[r];
        free_at = start + (Uint64)(per * (float)total);
        typed[r] = now < start ? 0 : (size_t)SDL_min((float)total, (float)(now - start) / per);
        if (now >= start && typed[r] < total) typing = r;
    }
    int top = 3, bottom = theme->bar ? app->rows - 5 : app->rows - 2;
    int fit = SDL_max(1, bottom - top), first = SDL_max(0, rows - fit);
    int cursor = typing >= 0 ? typing : last_step;
    for (int r = first; r < rows; ++r) {
        const GuiStep* s = owner[r];
        bool last = s == &app->steps.step[app->steps.count - 1];
        bool failed = s->state == GUI_STEP_FAILED, active = s->state == GUI_STEP_ACTIVE;
        size_t total = lengths[r][0] + lengths[r][1] + lengths[r][2];
        Uint64 age = now > shown[r] ? now - shown[r] : 0;
        bool moving = theme->line_in == GUI_LINE_SLIDE || theme->line_in == GUI_LINE_FADE;
        float p = moving && theme->line_in_ms ? SDL_min(1.0f, (float)age / (float)theme->line_in_ms) : 1.0f;
        float x = cw * (is_note[r] ? 3 : 1) -
                  (theme->line_in == GUI_LINE_SLIDE ? 16.0f * (float)app->scale * (1 - p) * (1 - p) : 0);
        float y = ch * (float)(top + r - first);
        SDL_Color color[3] = {failed ? failure : last ? ink : soft, failed ? failure : last || active ? ink : soft,
                              failed ? failure : ink};
        if (is_note[r]) color[0] = soft;
        size_t left = typed[r];
        for (int k = 0; k < 3; ++k) {
            size_t drawn = SDL_min(left, lengths[r][k]);
            x = text_px(app, x, y, runs[r][k], drawn, color[k], p);
            left -= drawn;
        }
        if (!is_note[r] && typed[r] == total && active) x = text_px(app, x, y, dots(app), 3, ink, p);
        bool blink = !theme->blink_ms || (now / theme->blink_ms) % 2 == 0;
        bool working = typing == r || (typing < 0 && active);
        if (r == cursor && working && blink) text_px(app, x, y, "_", 1, ink, p);
    }
    if (theme->bar) {
        int percent = gui_steps_percent(&app->steps);
        fill(app, cw, ch * (float)(app->rows - 4) + ch / 2, (float)(app->cols - 2) * cw, (float)app->scale,
             colors->dim, 255);
        enum { SEGMENTS = 40 };
        float span = (float)(app->cols - 8) * cw, gap = 2.0f * (float)app->scale;
        float seg = (span - gap * (SEGMENTS - 1)) / SEGMENTS, sy = ch * (float)(app->rows - 3) + ch * 0.3f;
        int filled = percent < 0 ? 0 : (percent * SEGMENTS + 50) / 100;
        for (int k = 0; k < SEGMENTS; ++k) {
            SDL_FRect r = {cw + (float)k * (seg + gap), sy, seg, ch * 0.4f};
            if (k < filled)
                fill(app, r.x, r.y, r.w, r.h, colors->ink, 255);
            else {
                SDL_SetRenderDrawColor(app->renderer, colors->dim.r, colors->dim.g, colors->dim.b, 255);
                SDL_RenderRect(app->renderer, &r);
            }
        }
        char number[16];
        if (percent < 0) snprintf(number, sizeof number, " --%%");
        else snprintf(number, sizeof number, "%3d%%", percent);
        text(app, app->cols - 6, app->rows - 3, number, ink);
        text(app, 1, app->rows - 2, name, ink);
    }
    if (app->finished)
        prompt(app, app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, app->rows - 1,
               "Press [ESC] to return to menu", soft);
    else if (app->aborting)
        prompt(app, GUI_PROMPT_ABORTING, 1, app->rows - 1, "Aborting process tree...", rgb(colors->warning));
    else
        text(app, 1, app->rows - 1, "Press [ESC] to abort and return to menu", soft);
}
/* the screen fading in from black, or the menu or the run fading out to it */
static void overlay(App* app) {
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
    int width, height;
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    fill(app, 0, 0, (float)width, (float)height, (GuiRgb){0, 0, 0}, (Uint8)(a * 255));
}
static void render(App* app) {
    layout(app);
    const GuiTheme* theme = &app->theme;
    bool idle = (!app->menu.running && !app->finished) || entering(app);
    bool head = theme->head != GUI_STYLE_NONE, foot = theme->footer != GUI_STYLE_NONE;
    SDL_Color ink = rgb(theme->ink), soft = rgb(theme->soft), focus = rgb(theme->focus), paper = rgb(theme->page);
    SDL_Color active = rgb(theme->active), failure = rgb(theme->failure);
    SDL_SetRenderDrawColor(app->renderer, theme->page.r, theme->page.g, theme->page.b, 255);
    SDL_RenderClear(app->renderer);
    if (!idle && theme->run == GUI_RUN_STEPS) {
        GuiRgb page = theme->run_colors.page;
        SDL_SetRenderDrawColor(app->renderer, page.r, page.g, page.b, 255);
        SDL_RenderClear(app->renderer);
        render_steps(app);
        overlay(app);
        SDL_RenderPresent(app->renderer);
        return;
    }
    const char* title = theme->title[idle ? (app->menu.view == GUI_LOAD_GAMES  ? GUI_SCREEN_GAMES
                                             : app->menu.view == GUI_LOAD_KEYS ? GUI_SCREEN_KEYS
                                                                               : GUI_SCREEN_MENU)
                                          : screen(app)];
    if (head && title[0]) {
        if (theme->head == GUI_STYLE_BAR) band(app, 0, 1, theme->ink);
        text(app, 1, 0, title, theme->head == GUI_STYLE_BAR ? paper : ink);
    }
    if (head && theme->head_rule) rule(app, 4);
    if (app->menu.view == GUI_LOAD_GAMES && idle) {
        if (head) {
            text(app, 1, 1, "Up/Down: select. Enter/Space: open. Left/Right or PgUp/PgDown: page.", ink);
            text(app, 1, 2, app->games_dir, soft);
        }
        entry(app, 1, 3, "Back [ESC]", false, app->browser.selected == 0);
        size_t start = app->browser.page * app->browser.page_size;
        size_t count = gui_browser_items(&app->browser);
        for (size_t i = 0; i < count; ++i)
            entry(app, 1, 5 + (int)i, app->games.names[start + i],
                  app->browser.activated && app->browser.active == start + i, app->browser.selected == i + 1);
        if (!app->games.count) text(app, 1, 5, "No executables found in games/.", soft);
        char page[128];
        snprintf(page, sizeof(page), "Page %zu/%zu   %zu executable(s)", app->browser.page + 1,
                 gui_browser_pages(&app->browser), app->games.count);
        text(app, 1, app->rows - 5, page, soft);
        text(app, 1, app->rows - 4, app->message, focus);
    } else if (app->menu.view == GUI_LOAD_KEYS && idle) {
        if (head) {
            text(app, 1, 1, "Setup device keys: drag and drop to this window", ink);
            text(app, 1, 2, "Ctrl+V: paste a path. ESC: back.", soft);
            text(app, 1, 3, app->keys_path ? app->keys_path : "Home directory unavailable.", ink);
        }
        if (app->keys_path && snx_is_file(app->keys_path))
            text(app, 1, 4, "Keys already setup. This will overwrite the previous ones", failure);
        text(app, 1, 6, app->message, focus);
        entry(app, 1, 8, "Back [ESC]", false, app->browser.selected == 0);
        prompt(app, GUI_PROMPT_KEYS, 1, SDL_max(app->rows / 2, 10), "Drop prod.keys here", focus);
    } else if (idle) {
        if (head) {
            text(app, 1, 1, "Arrows/mouse: move. Space/Enter/click: select.", ink);
            text(app, 1, 2,
                 app->menu.view == GUI_LOAD_DROP ? "Drop a file now. Ctrl+V: paste a path. ESC: back."
                                                 : "Choose a load option. ESC: back.",
                 soft);
        }
        for (int i = 0; i < GUI_ITEM_COUNT; ++i) {
            bool is_active = app->menu.activated && app->menu.active == i;
            bool is_focused = app->menu.selected == i &&
                              (app->menu.view == GUI_LOAD_NONE || !app->menu.focus_load);
            int row = menu_row(app, i);
            char label[100];
            menu_label(app, i, label, sizeof(label));
            entry(app, 1, row, label, is_active, is_focused);
            if (i == app->menu.active && app->menu.view != GUI_LOAD_NONE) {
                entry(app, 3, row + 1, "-> Select via drag and drop",
                      app->menu.load_activated && app->menu.load_active == GUI_LOAD_DRAG,
                      app->menu.focus_load && app->menu.load_selected == GUI_LOAD_DRAG);
                entry(app, 3, row + 2, "-> Select file from games/ instead",
                      app->menu.load_activated && app->menu.load_active == GUI_LOAD_DIRECTORY,
                      app->menu.focus_load && app->menu.load_selected == GUI_LOAD_DIRECTORY);
            }
        }
        text(app, 1, app->log_top - 4, app->message, app->abnormal_exit ? failure : focus);
        if (app->has_output && !entering(app)) text(app, 1, app->log_top - 2, "Last output (no scrollback):", soft);
    } else if (head) {
        text(app, 1, 1, app->finished ? app->message : items[app->menu.selected],
             app->abnormal_exit ? failure : app->finished ? focus : active);
        text(app, 1, 2, app->file ? app->file : "", ink);
        if (app->finished)
            prompt(app, app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, 3, "Press [ESC] to return to menu",
                   failure);
        else if (app->aborting)
            prompt(app, GUI_PROMPT_ABORTING, 1, 3, "Aborting process tree...", failure);
        else
            text(app, 1, 3, "Press [ESC] to abort and return to menu", failure);
    }
    if (app->has_output && app->menu.view != GUI_LOAD_GAMES && app->menu.view != GUI_LOAD_KEYS && !entering(app)) {
        SDL_SetTextureColorMod(app->font, theme->ink.r, theme->ink.g, theme->ink.b);
        for (int y = 0; y < app->console.rows; ++y)
            for (int x = 0; x < app->console.cols; ++x)
                if (app->log_top + y < app->rows - 4 && app->console.cells[y][x] != ' ')
                    glyph(app, x + 1, app->log_top + y, app->console.cells[y][x]);
    }
    if (idle && (app->menu.view == GUI_LOAD_NONE ||
                 ((app->menu.view == GUI_LOAD_OPTIONS || app->menu.view == GUI_LOAD_DROP) && !app->menu.focus_load)))
        text(app, 1, app->rows - 4, gui_setting_description(&app->menu), ink);
    if (foot) {
        bool bar = theme->footer == GUI_STYLE_BAR;
        if (bar) band(app, app->rows - 3, 3, theme->ink);
        if (theme->footer_rule) rule(app, app->rows - 3);
        char footer[256];
        if (app->usage.available)
            snprintf(footer, sizeof(footer), "GUI + children: RAM %.1f MiB (sum RSS)  CPU %.1f%% (100%% = 1 core)",
                     (double)app->usage.memory_bytes / 1048576, app->usage.cpu_percent);
        else
            snprintf(footer, sizeof(footer), "GUI + children: resource usage unavailable");
        text(app, 1, app->rows - 3, footer, bar ? paper : soft);
        text(app, 1, app->rows - 2, "CWD:", bar ? paper : soft);
        text(app, 6, app->rows - 2, app->cwd ? app->cwd : "(unavailable)", bar ? paper : ink);
    }
    overlay(app);
    SDL_RenderPresent(app->renderer);
}
/* back to the menu from a finished run, which fades in */
static void leave_run(App* app) {
    app->finished = false;
    app->menu.view = GUI_LOAD_NONE;
    app->message[0] = 0;
    app->leaving_ms = 0;
    app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
    layout(app);
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
            gui_banner_feed(&app->banner, buffer, (size_t)n);
            gui_steps_feed(&app->steps, buffer, (size_t)n, now_ms(app));
            app->has_output = true;
        }
        int status;
        if (gui_process_finished(app->process, &status)) {
            app->menu.running = false;
            /* Normal exits wait for acknowledgement. ESC already acknowledged
               an abort, so return straight to the menu after nonblocking reaping. */
            app->finished = !app->aborting;
            app->abnormal_exit = app->aborting || status != 0;
            gui_steps_finish(&app->steps, !app->abnormal_exit);
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
    if (app->leaving_ms && now_ms(app) - app->leaving_ms >= app->theme.fade_out_ms) leave_run(app);
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
    const char* argv[7] = {native ? absolute : driver};
    size_t n = 1;
    if (!native) argv[n++] = absolute;
    n += gui_launch_options(&app->menu, native, argv + n);
    argv[n] = NULL;
    bool started = gui_process_start(app->process, argv, app->message, sizeof(app->message));
    gui_banner_reset(&app->banner);
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
    app->menu.view = GUI_LOAD_NONE;
    app->menu.running = true;
    app->finished = false;
    app->abnormal_exit = false;
    app->message[0] = 0;
    gui_console_init(&app->console, app->cols - 2, app->rows - 8);
    app->has_output = true;
    layout(app);
}
static char* device_keys_path(void) {
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
        app->menu.focus_load = true;
        launch(app, absolute, false);
    }
}
static void menu_key(App* app, GuiKey key) {
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
            app->menu.view = GUI_LOAD_OPTIONS;
            app->menu.focus_load = true;
            app->menu.load_activated = false;
            app->menu.selected = app->menu.active;
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
    if (app->menu.theme_next) {
        app->menu.theme_next = false;
        next_theme(app);
    }
    if (key == GUI_CANCEL || (key == GUI_SELECT && app->menu.view == GUI_LOAD_KEYS)) app->message[0] = 0;
    if (app->menu.view == GUI_LOAD_GAMES) {
        gui_games_scan(&app->games, app->games_dir, app->message, sizeof(app->message));
        app->browser = (GuiBrowser){.count = app->games.count};
        layout(app);
    }
}
static bool hit_text(const App* app, float x, float y, int col, int row, const char* label) {
    int end = SDL_min(app->cols - 1, col + (int)SDL_utf8strlen(label));
    return x >= col && x < end && y >= row && y < row + 1;
}
static void pointer(App* app, float window_x, float window_y, bool click) {
    if (app->menu.running || app->finished) return;
    layout(app);
    float x, y;
    if (!SDL_RenderCoordinatesFromWindow(app->renderer, window_x, window_y, &x, &y)) return;
    x /= 8 * app->scale;
    y /= 16 * app->scale;
    if (app->menu.view == GUI_LOAD_KEYS) {
        if (click && hit_text(app, x, y, 1, 8, "Back [ESC]")) menu_key(app, GUI_CANCEL);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        if (hit_text(app, x, y, 1, 3, "Back [ESC]")) {
            app->browser.selected = 0;
            if (click) menu_key(app, GUI_SELECT);
            return;
        }
        size_t start = app->browser.page * app->browser.page_size;
        for (size_t i = 0; i < gui_browser_items(&app->browser); ++i) {
            if (!hit_text(app, x, y, 1, 5 + (int)i, app->games.names[start + i])) continue;
            app->browser.selected = i + 1;
            if (click) menu_key(app, GUI_SELECT);
            return;
        }
        return;
    }
    if (app->menu.view != GUI_LOAD_NONE) {
        int row = menu_row(app, app->menu.active);
        const char* labels[] = {"-> Select via drag and drop", "-> Select file from games/ instead"};
        for (int i = 0; i < 2; ++i) {
            if (!hit_text(app, x, y, 3, row + i + 1, labels[i])) continue;
            app->menu.load_selected = i;
            app->menu.focus_load = true;
            if (click) menu_key(app, GUI_SELECT);
            return;
        }
    }
    for (int i = 0; i < GUI_ITEM_COUNT; ++i) {
        char label[100];
        menu_label(app, i, label, sizeof(label));
        if (!hit_text(app, x, y, 1, menu_row(app, i), label)) continue;
        app->menu.selected = i;
        app->menu.focus_load = false;
        if (click) menu_key(app, GUI_SELECT);
        return;
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
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        pointer(app, e->motion.x, e->motion.y, false);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT) {
        pointer(app, e->button.x, e->button.y, true);
        return;
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
    if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) &&
        (app->menu.view == GUI_LOAD_DROP || app->menu.view == GUI_LOAD_KEYS) && !e->key.repeat) {
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
    app->theme = *gui_theme_builtin("default");
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
    app->themes_dirs[0] = snx_path_join(app->cwd ? app->cwd : ".", "themes");
    app->drivers = SDL_GetBasePath();
    if (app->drivers) app->themes_dirs[1] = snx_path_join(app->drivers, "themes");
    app->keys_path = device_keys_path();
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
    free(app->themes_dirs[0]);
    free(app->themes_dirs[1]);
    gui_themes_free(&app->themes);
    free(app->keys_path);
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
