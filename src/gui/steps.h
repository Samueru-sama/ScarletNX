/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_STEPS_H
#define SNX_GUI_STEPS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A run as a loading screen tells it: the driver's and the runner's lines it knows become steps, each running
   until the next one starts, and the last ends with the process. */
#define GUI_STEPS_MAX 24
#define GUI_NOTES_MAX 128
typedef enum { GUI_STEP_ACTIVE, GUI_STEP_DONE, GUI_STEP_FAILED } GuiStepState;
typedef struct {
    char label[12], detail[96];
    GuiStepState state;
    uint32_t value, total; /* the recompile's blocks */
    uint64_t shown_ms;
    uint32_t serial;
} GuiStep;
/* a line printed during a step, for the detailed screen */
typedef struct {
    char text[112];
    uint64_t shown_ms;
    uint32_t step; /* its step's serial */
} GuiNote;
typedef struct {
    GuiStep step[GUI_STEPS_MAX]; /* the latest, the oldest dropped */
    int count;
    GuiNote note[GUI_NOTES_MAX]; /* the latest, the oldest dropped */
    int notes;
    uint32_t serial;
    char line[256];
    size_t length;
    bool gpu;
} GuiSteps;
void gui_steps_reset(GuiSteps* steps);
void gui_steps_feed(GuiSteps* steps, const char* data, size_t size, uint64_t now_ms);
void gui_steps_finish(GuiSteps* steps, bool ok);
/* how far the recompile is, 0 to 100; 100 once the game launches; -1 before anything says */
int gui_steps_percent(const GuiSteps* steps);
/* the latest max notes of a step, oldest first, into out; their count */
int gui_step_notes(const GuiSteps* steps, const GuiStep* step, const GuiNote** out, int max);
#endif
