/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A setting of the game's .ini, described once by the module that uses it: its key, default, help, type, range and
   where its value goes. options.c writes the file from the tables in its sections (the unattended-run options' table
   is only read), and each module reads its keys with ini_find and ini_read (ini.c) */
#ifndef SNX_INI_H
#define SNX_INI_H
#include <stddef.h>

enum {
    INI_INT,   /* an int at var, clamped to lo..hi */
    INI_FLOAT, /* a float at var, clamped to lo..hi */
    INI_TEXT   /* read by set */
};
#define INI_STARTUP 1 /* read when the game starts: a reload while it runs leaves it alone (options.c's sections) */

typedef struct IniOpt IniOpt;
struct IniOpt {
    const char* key;  /* NULL ends a table */
    const char* def;  /* NULL: no default. A row of options.c's sections is written once set */
    const char* help; /* NULL: the help of the one before covers it */
    int type, flags;
    void* var;
    double lo, hi;
    /* INI_TEXT: reads the value. INI_INT and INI_FLOAT: called after each read into var (the raw text in v), or NULL */
    void (*set)(const IniOpt* o, const char* v);
};

const IniOpt* ini_find(const IniOpt* table, const char* key); /* key's entry in table, or NULL */
void ini_read(const IniOpt* o, const char* v); /* v into o's variable; a value that isn't a number leaves it */
/* o's variable as ini_read reads it back. An INI_TEXT has none: its set keeps what it read */
void ini_format(const IniOpt* o, char* out, size_t n);

#endif
