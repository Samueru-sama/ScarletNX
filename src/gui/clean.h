/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Settings > App's Clean old settings, without SDL: the [Deprecated] section the runtime gives a game's .ini for the
   keys it no longer reads, taken out; what an older version left where this one no longer looks, moved where this one
   keeps it, or deleted when that place already holds this version's own, so nothing is lost. Every file is an entry
   of what was done, or of why it was not */
#ifndef SNX_GUI_CLEAN_H
#define SNX_GUI_CLEAN_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char* text;      /* an entry a line: '+' done or '!' not, a tab, the file, a tab, what was done or why not */
    int count, done; /* the entries, and the ones done */
} GuiCleaned;
/* text without its [Deprecated] sections, in place: the line [Deprecated] (a CR before its end allowed), every line
   after it up to the next [section] or the end, and the blank lines just before it. Its new size, size for none */
size_t gui_clean_deprecated(char* text, size_t size);
/* the .ini files directly in folder with a [Deprecated] section: how many. With done, each is written again without
   it, an entry each */
int gui_clean_inis(GuiCleaned* done, const char* folder);
/* what an older version left at old, kept by this one at now: moved there (its folder made) while nothing is there,
   else deleted, a folder with what it holds. An entry, unless old is gone */
void gui_clean_left(GuiCleaned* done, const char* old, const char* now);
/* an entry of the caller's: file, and what was done to it or why not */
void gui_cleaned_add(GuiCleaned* done, bool ok, const char* file, const char* what);
/* entry at's fields; the next entry, NULL after the last */
const char* gui_cleaned_entry(const char* at, bool* ok, char* file, char* what, size_t size);
void gui_cleaned_free(GuiCleaned* done);
#endif
