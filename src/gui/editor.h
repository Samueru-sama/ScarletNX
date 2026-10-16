/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A form, a screen of rows the menu opens: the theme editor, a row for the folder the theme is saved in, a row per key
   of theme.ini (gui_theme_keys) and Save; or another table of keys over an object of its own, as the Settings. draw.c
   and boxes.c draw it, the theme editor with a preview of the menu or a run in the theme as it is; this holds what it
   edits and what each key does to it, without SDL. */
#ifndef SNX_GUI_EDITOR_H
#define SNX_GUI_EDITOR_H
#include <stdbool.h>
#include <stddef.h>
#include "model.h"
#include "theme.h"

typedef struct {
    const GuiThemeKey* keys; /* a row each */
    int count;
    char* object;       /* what the keys' offsets are in; NULL: the theme, its rows between the folder's and Save */
    const char* action; /* a form's last row, as the theme editor's Save, when not NULL: its label */
    /* the theme editor's: the theme, the extras of the file it started from (written back with it), its folder */
    GuiTheme theme;
    GuiThemeFile file;
    char id[64];
    int row, top; /* the focused row, and the first one shown */
    bool typing;  /* a value being typed for the focused row, in text */
    char text[80];
    bool dirty, leaving; /* changed since it was saved; ESC pressed once over changes */
    int preview;         /* GUI_SCREEN_MENU or GUI_SCREEN_COMPILING */
    char note[160];      /* what the last key did, when it did not do what it was asked */
    bool flat;           /* no section column: the form's rows are all of one */
    int step;            /* the last GUI_EDITOR_ACTION's key: 0 Select, -1 Left, 1 Right */
} GuiEditor;
#define GUI_EDITOR_FOLDER 0 /* the theme editor's first row */
int gui_editor_rows(const GuiEditor* editor);
/* the key of a row, NULL for the folder and Save, or a form's action */
const GuiThemeKey* gui_editor_key_of(const GuiEditor* editor, int row);
/* edits theme, its file's extras taken over (file may be NULL), saved as themes/id */
void gui_editor_start(GuiEditor* editor, const GuiTheme* theme, GuiThemeFile* file, const char* id);
/* edits object through count keys: no folder, no Save, its changes kept as they are made; action, when not NULL, a last
   row that Select on returns GUI_EDITOR_SAVE for */
void gui_editor_form(GuiEditor* editor, const GuiThemeKey* keys, int count, void* object, const char* action);
void gui_editor_stop(GuiEditor* editor);
/* what a key asks of the screen: nothing more, to leave it, to save (a form: to do its action), or the focused
   GUI_KEY_ACTION row's (step says which key) */
typedef enum { GUI_EDITOR_STAY, GUI_EDITOR_LEAVE, GUI_EDITOR_SAVE, GUI_EDITOR_ACTION } GuiEditorDo;
/* Up/Down and the pages move; Left/Right change a word, a flag or a number; Select changes a word or a flag, starts
   typing the others and takes what was typed; Cancel stops typing, or leaves (asking once over changes). On an action
   row Select, Left and Right are the caller's */
GuiEditorDo gui_editor_key(GuiEditor* editor, GuiKey key);
/* while typing: text added, or the last character taken off */
void gui_editor_type(GuiEditor* editor, const char* text);
void gui_editor_erase(GuiEditor* editor);
/* a row's label and value as the screen shows them */
void gui_editor_label(const GuiEditor* editor, int row, char* out, size_t size);
void gui_editor_value(const GuiEditor* editor, int row, char* out, size_t size);
/* themes_dir/id/theme.ini written: 0, else nonzero with the note saying why */
int gui_editor_save(GuiEditor* editor, const char* themes_dir);
/* rows rows on screen: top follows the focus */
void gui_editor_scroll(GuiEditor* editor, int rows);
#endif
