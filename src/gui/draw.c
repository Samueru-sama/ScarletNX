/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's drawing: the screens as lines down the page, a run as a loading screen, the boxes over a screen
   in those lines, the boot's splash, the theme editor and its preview; render() picks the screen */
#include "app.h"
SDL_Texture* gui_app_create_font(SDL_Renderer* renderer) {
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
void gui_app_set_area(App* app) {
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
/* rows of the whole width, as a band behind text: on to the window's edges, over the camera's cutout beside the
   text and what is left below the last row */
void gui_app_band(App* app, int row, int rows, GuiRgb c) {
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
/* the footer from row: the usage, then the working folder, which games/ and themes/ are found in. The theme's bar style
   puts them on a band of ink over rows rows, its plain one in soft and ink; a rule above when it says so */
/* what ScarletNX's folders take, counted on a thread, and what is left on their disk; "" before the first count */
void gui_app_disk_text(const App* app, char* out, size_t size) {
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
    if (bar) gui_app_band(app, row, rows, ink);
    if (app->theme.footer_rule) rule(app, row, soft);
    gui_usage_text(usage, sizeof(usage), app->usage.available, app->usage.memory_bytes, app->usage.cpu_percent,
                   SDL_GetNumLogicalCPUCores());
    gui_app_disk_text(app, disk, sizeof disk);
    n = strlen(usage);
    if (disk[0]) snprintf(usage + n, sizeof usage - n, "  %s", disk);
    text(app, 1, row, usage, rgb(bar ? page : soft));
    text(app, 1, row + 1, "CWD:", rgb(bar ? page : soft));
    text(app, 6, row + 1, app->cwd ? app->cwd : "(unavailable)", rgb(bar ? page : ink));
}
/* a selectable line: activated, focused (in the focus colour, or on a band) or plain */
static void entry(App* app, int x, int y, const char* label, int activated, int focused) {
    if (focused && app->theme.focus_bar) {
        gui_app_band(app, y, 1, app->theme.focus);
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
void gui_app_run_name(const App* app, char* out, size_t size) {
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
size_t gui_app_step_runs(const GuiStep* s, int how, Uint64 now, char run[3][112], size_t length[3]) {
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

    gui_app_run_name(app, name, sizeof name);
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
        gui_app_step_runs(s, theme->progress, now, runs[rows], lengths[rows]);
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
const char* gui_app_ask_question(const App* app) {
    if (app->confirm.exit) return "Exit ScarletNX?";
    return screen(app) == GUI_SCREEN_COMPILING ? "Abort the recompile? Its work is lost." : "Abort the run?";
}
/* the question's box, in cells: a blank row, the question, a blank row, its two choices and a blank row, two columns
   either side of the question */
void gui_app_ask_box(const App* app, int* col, int* row, int* cols) {
    *cols = SDL_min(app->cols - 2, (int)SDL_utf8strlen(gui_app_ask_question(app)) + 4);
    *col = (app->cols - *cols) / 2;
    *row = SDL_max(0, (app->rows - 6) / 2);
}
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
void gui_app_left_paths(const char* line, char* old, char* now, size_t size) {
    const char* by = strstr(line, SAY_LEFT_BY);
    snprintf(old, size, "%.*s", (int)(by - line), line);
    by += strlen(SAY_LEFT_BY);
    snprintf(now, size, "%.*s", (int)strcspn(by, "\n"), by);
}
/* the box of an older version's files in rows: its first row, its rows, and the files that fit in it */
static void left_place(App* app, int* row, int* rows, int* shown) {
    int width = SDL_max(8, app->cols - 7), title, room, used = 0;
    const char* at;
    char old[4200], now[4200];
    /* a blank row, the title, a blank row, the files, a blank row or how many more, the two choices and a blank row */
    title = wrapped(app, 0, 0, "", SAY_LEFT_TITLE, width, NULL);
    room = app->rows - 2 - (title + 6);
    for (*shown = 0, at = app->left; *shown < app->lefts; ++*shown, at = strchr(at, '\n') + 1) {
        int need;
        gui_app_left_paths(at, old, now, sizeof old);
        need = wrapped(app, 0, 0, "- ", old, width, NULL) + wrapped(app, 0, 0, "-> ", now, width, NULL);
        if (*shown && used + need > room) break;
        used += need;
    }
    *rows = title + used + 6;
    *row = SDL_max(0, (app->rows - *rows) / 2);
}
/* a box's choice, at row y in the box from column 2 to the screen's last but one: focused on a band (the theme's
   focus bar) or in the focus colour */
static void box_choice(App* app, int y, const char* label, int focused) {
    const GuiTheme* t = &app->theme;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale, line = (float)app->scale;
    char shown[128];
    snprintf(shown, sizeof shown, "%s%s", t->marker, label);
    if (focused && t->focus_bar)
        fill(app, 2 * cw + line, (float)y * ch, (float)(app->cols - 3) * cw - 2 * line, ch, t->focus, 255);
    text(app, 4, y, shown, rgb(focused && t->focus_bar ? t->page : focused ? t->focus : t->ink));
}
int gui_app_left_choice(App* app, float x, float y) {
    int row, rows, shown, r = (int)(y / (16.0f * (float)app->scale)), c = (int)(x / (8.0f * (float)app->scale));
    if (boxed(app)) return gui_app_left_boxed_choice(app, x, y);
    left_place(app, &row, &rows, &shown);
    if (c < 2 || c >= app->cols - 1) return -1;
    return r == row + rows - 3 ? 0 : r == row + rows - 2 ? 1 : -1;
}
static void render_left(App* app) {
    const GuiTheme* t = &app->theme;
    SDL_Color warning = rgb(t->warning), failure = rgb(t->failure), ink = rgb(t->ink), soft = rgb(t->soft);
    int width = SDL_max(8, app->cols - 7), shown, rows, row, y;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale, line = (float)app->scale;
    const char* at;
    char old[4200], now[4200], label[64];
    if (!app->left_shown || !app->lefts) return;
    if (boxed(app)) {
        gui_app_left_boxed(app);
        return;
    }
    left_place(app, &row, &rows, &shown);
    fill(app, 2 * cw, (float)row * ch, (float)(app->cols - 3) * cw, (float)rows * ch, t->soft, 255); /* framed */
    fill(app, 2 * cw + line, (float)row * ch + line, (float)(app->cols - 3) * cw - 2 * line,
         (float)rows * ch - 2 * line, t->page, 255);
    y = row + 1 + wrapped(app, 4, row + 1, "", SAY_LEFT_TITLE, width, &warning) + 1;
    at = app->left;
    for (int i = 0; i < shown; ++i, at = strchr(at, '\n') + 1) {
        gui_app_left_paths(at, old, now, sizeof old);
        y += wrapped(app, 4, y, "- ", old, width, &failure);
        y += wrapped(app, 4, y, "-> ", now, width, &ink);
    }
    if (shown < app->lefts) {
        snprintf(label, sizeof label, "and %d more, which Share log lists", app->lefts - shown);
        text(app, 4, y, label, soft);
    }
    box_choice(app, row + rows - 3, SAY_LEFT_OK, app->left_focus == 0);
    box_choice(app, row + rows - 2, SAY_LEFT_CLEAN, app->left_focus == 1);
}
/* what Clean old settings did, over the screen: each file, and what was done to it or why not (in the failure colour),
   as many as fit, how many more, and OK */
/* its box in rows: its first row, its rows, and the files that fit */
static void cleaned_place(App* app, int* row, int* rows, int* shown) {
    int width = SDL_max(8, app->cols - 7), used = 0, room, title;
    const char* at;
    char file[4400], what[4400], head[96];
    bool ok;
    /* a blank row, the title, a blank row, the files, a blank row or how many more, OK and a blank row */
    gui_app_cleaned_title(app, head, sizeof head);
    title = wrapped(app, 0, 0, "", head, width, NULL);
    room = app->rows - 2 - (title + 5);
    for (*shown = 0, at = app->cleaned.text; at && *shown < app->cleaned.count; ++*shown) {
        int need;
        at = gui_cleaned_entry(at, &ok, file, what, sizeof file);
        need = wrapped(app, 0, 0, "- ", file, width, NULL) + wrapped(app, 0, 0, "  ", what, width, NULL);
        if (*shown && used + need > room) break;
        used += need;
    }
    *rows = title + used + 5;
    *row = SDL_max(0, (app->rows - *rows) / 2);
}
static void render_cleaned(App* app) {
    const GuiTheme* t = &app->theme;
    SDL_Color warning = rgb(t->warning), failure = rgb(t->failure), ink = rgb(t->ink), soft = rgb(t->soft);
    int width = SDL_max(8, app->cols - 7), shown, rows, row, y, i;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale, line = (float)app->scale;
    const char* at;
    char file[4400], what[4400], head[96], label[64];
    bool ok;
    if (!app->cleaned_shown) return;
    if (boxed(app)) {
        gui_app_cleaned_boxed(app);
        return;
    }
    gui_app_cleaned_title(app, head, sizeof head);
    cleaned_place(app, &row, &rows, &shown);
    fill(app, 2 * cw, (float)row * ch, (float)(app->cols - 3) * cw, (float)rows * ch, t->soft, 255); /* framed */
    fill(app, 2 * cw + line, (float)row * ch + line, (float)(app->cols - 3) * cw - 2 * line,
         (float)rows * ch - 2 * line, t->page, 255);
    y = row + 1 + wrapped(app, 4, row + 1, "", head, width, &warning) + 1;
    for (i = 0, at = app->cleaned.text; at && i < shown; ++i) {
        at = gui_cleaned_entry(at, &ok, file, what, sizeof file);
        y += wrapped(app, 4, y, "- ", file, width, ok ? &ink : &failure);
        y += wrapped(app, 4, y, "  ", what, width, ok ? &soft : &failure);
    }
    if (shown < app->cleaned.count) {
        snprintf(label, sizeof label, "and %d more, which Share log lists", app->cleaned.count - shown);
        text(app, 4, y, label, soft);
    }
    box_choice(app, row + rows - 2, "OK", 1);
}
static void render_ask(App* app) {
    static const char* const choices[2][2] = {{"Keep going", "Abort"}, {"Stay", "Exit"}}; /* by exit, GUI_CONFIRM_* */
    const GuiTheme* t = &app->theme;
    int steps = t->run == GUI_RUN_STEPS && !app->confirm.exit, col, row, cols, i;
    GuiRgb page = steps ? t->run_colors.page : t->page, ink = steps ? t->run_colors.ink : t->ink;
    GuiRgb soft = steps ? t->run_colors.soft : t->soft, warning = steps ? t->run_colors.warning : t->warning;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale, line = (float)app->scale;
    if (!app->confirm.shown || (!app->menu.running && !app->confirm.exit)) return;
    if (boxed(app)) {
        gui_app_ask_boxed(app);
        return;
    }

    gui_app_ask_box(app, &col, &row, &cols);
    fill(app, (float)col * cw, (float)row * ch, (float)cols * cw, 6 * ch, soft, 255); /* framed in soft */
    fill(app, (float)col * cw + line, (float)row * ch + line, (float)cols * cw - 2 * line, 6 * ch - 2 * line, page,
         255);
    text(app, col + 2, row + 1, gui_app_ask_question(app), rgb(warning));
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
void gui_app_splash_start(App* app) {
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
        gui_app_draw_run_boxed(app);
        render_ask(app);
        overlay(app);
        return;
    }
    if (boxed(app) && idle) {
        if (app->menu.view == GUI_LOAD_GAMES)
            gui_app_draw_games_boxed(app);
        else if (pick_screen(app))
            gui_app_draw_pick_boxed(app);
        else
            gui_app_draw_menu_boxed(app);
        render_ask(app);
        render_left(app);
        overlay(app);
        return;
    }
    title = theme->title[idle ? menu_screen(app) : screen(app)];
    if (head && title[0]) {
        if (theme->head == GUI_STYLE_BAR) gui_app_band(app, 0, 1, theme->ink);
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
            snprintf(line, sizeof line, "%s: " SAY_PICK, gui_items[app->menu.active]);
            text(app, 1, 1, line, ink);
        }
        if (head) text(app, 1, 2, SAY_KEYS_HOW, soft);
        char* in_use = keys ? gui_app_keys_in_use(app) : NULL;
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
        text(app, 1, 1, app->finished ? app->message : gui_items[app->menu.selected],
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
void gui_app_preview_box(const App* app, int* col, int* row, int* cols, int* rows) {
    *cols = SDL_max(16, app->cols / 2 - 2);
    *rows = *cols * 5 / 16;
    *col = app->cols - *cols;
    *row = 4;
}
/* the menu or a run drawn in the theme being edited, at 1x, in app->preview; false when it cannot be made */
bool gui_app_preview_texture(App* app) {
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
    gui_app_set_area(app);
    return true;
}
static void render_preview(App* app) {
    SDL_FRect box;
    int col, row, cols, rows, run = app->editor.preview != GUI_SCREEN_MENU;
    gui_app_preview_box(app, &col, &row, &cols, &rows);
    if (!gui_app_preview_texture(app)) return;
    box.x = (float)(col * 8 * app->scale);
    box.y = (float)(row * 16 * app->scale);
    box.w = (float)(cols * 8 * app->scale);
    box.h = (float)(rows * 16 * app->scale);
    SDL_RenderTexture(app->renderer, app->preview, NULL, &box);
    text(app, col, row + rows, run ? "Preview: a run (Ctrl+P: the menu)" : "Preview: the menu (Ctrl+P: a run)",
         rgb(app->theme.soft));
}
static void render_editor(App* app) {
    GuiEditor* e = &app->editor;
    const GuiTheme* theme = &app->theme;
    const GuiThemeKey* focused = gui_editor_key_of(e, e->row);
    char label[64], value[96], title[96];
    int r, col, row, cols, rows, width, labels = 24;
    SDL_SetRenderDrawColor(app->renderer, theme->page.r, theme->page.g, theme->page.b, 255);
    SDL_RenderClear(app->renderer);
    gui_app_preview_box(app, &col, &row, &cols, &rows);
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
    if (theme->head == GUI_STYLE_BAR) gui_app_band(app, 0, 1, theme->ink);
    text(app, 1, 0, title, rgb(theme->head == GUI_STYLE_BAR ? theme->page : theme->ink));
    text(app, 1, 1,
         app->editor.picking              ? SAY_PICKER
         : app->form == GUI_FORM_THEME    ? SAY_EDITOR
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
    gui_app_draw_picker(app);
}
void gui_app_render(App* app) {
    gui_app_layout(app);
    if (splash_shown(app)) return;
    if (app->editing && app->form == GUI_FORM_PAD)
        gui_app_render_pad(app);
    else if (app->editing && boxed(app))
        gui_app_draw_form_boxed(app);
    else if (app->editing)
        render_editor(app);
    else
        draw_screen(app);
    render_cleaned(app);
    SDL_RenderPresent(app->renderer);
}
/* the box's title of what Clean old settings did: how many files it did, and how many it could not */
void gui_app_cleaned_title(const App* app, char* out, size_t size) {
    int not_done = app->cleaned.count - app->cleaned.done;
    if (not_done)
        snprintf(out, size, "Clean old settings: %d done, %d not", app->cleaned.done, not_done);
    else
        snprintf(out, size, "Clean old settings: %d done", app->cleaned.done);
}
