/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The .ini's settings tables (runtime/ini.c): a value read into its variable, clamped, and written back */
#include "runtime/ini.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int number, told;
static float ratio;
static char text[16];
static void tell(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    ++told;
}
static void keep(const IniOpt* o, const char* v) {
    (void)o;
    snprintf(text, sizeof text, "%s", v);
}
static const IniOpt TABLE[] = {{"number", "1", "help", INI_INT, 0, &number, -5, 10, tell},
                               {"ratio", "1", NULL, INI_FLOAT, INI_STARTUP, &ratio, 0.25, 4, NULL},
                               {"text", NULL, NULL, INI_TEXT, 0, NULL, 0, 0, keep},
                               {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL}};

int main(void) {
    const IniOpt *n = ini_find(TABLE, "number"), *r = ini_find(TABLE, "ratio"), *t = ini_find(TABLE, "text");
    char out[32];
    assert(n == &TABLE[0] && r == &TABLE[1] && t == &TABLE[2] && !ini_find(TABLE, "numbe") && !ini_find(TABLE, ""));
    /* a number, clamped, and set told after each read */
    ini_read(n, "7");
    assert(number == 7 && told == 1);
    ini_read(n, "99");
    assert(number == 10 && told == 2);
    ini_read(n, "-1e9");
    assert(number == -5 && told == 3);
    ini_read(n, "3.9"); /* an int keeps the whole part */
    assert(number == 3);
    /* not a number, or NaN: the variable stays and set isn't called */
    ini_read(n, "seven");
    ini_read(n, "");
    ini_read(n, "nan");
    assert(number == 3 && told == 4);
    ini_read(r, "inf");
    assert(ratio == 4);
    ini_read(r, "0.5");
    assert(ratio == 0.5f);
    /* written back as it reads */
    ini_format(n, out, sizeof out);
    assert(!strcmp(out, "3"));
    ini_format(r, out, sizeof out);
    assert(!strcmp(out, "0.5"));
    /* text: set reads it whole, and nothing is written back */
    ini_read(t, "a b c");
    assert(!strcmp(text, "a b c"));
    ini_format(t, out, sizeof out);
    assert(!out[0]);
    puts("ini passed");
    return 0;
}
