/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_data_dir, the folder the driver and the menu share: the checkout, else a portable copy's folder (a games/ beside
   the AppImage or the program), else the user's data folder. */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* join(const char* a, const char* b) {
    char* p = snx_path_join(a, b);
    assert(p);
    return p;
}

int main(void) {
    char *root = snx_temp_dir(NULL), *checkout, *program, *mount, *copy, *data, *xdg, *expected, *file;
    assert(root);
    checkout = join(root, "checkout");
    program = join(root, "program");
    mount = join(root, "mount");
    copy = join(root, "copy");
    xdg = join(root, "xdg");
    assert(!snx_mkdirs(checkout) && !snx_mkdirs(program) && !snx_mkdirs(mount) && !snx_mkdirs(copy));
    file = join(checkout, "CMakeLists.txt");
    assert(!snx_write_file(file, "", 0));
    free(file);
    unsetenv("APPIMAGE");
    unsetenv("APPDIR");
    setenv("XDG_DATA_HOME", xdg, 1);
    expected = join(xdg, "scarletnx");

    /* a build tree's checkout wins */
    data = snx_data_dir(checkout, program);
    assert(data && !strcmp(data, checkout));
    free(data);
    /* no checkout and nothing beside the program: the user's data folder */
    data = snx_data_dir(program, program);
    assert(data && !strcmp(data, expected));
    free(data);
    data = snx_data_dir(NULL, NULL);
    assert(data && !strcmp(data, expected));
    free(data);
    /* a games/ beside the program (the Windows zip, a portable install) */
    file = join(program, "games");
    assert(!snx_mkdirs(file));
    free(file);
    data = snx_data_dir(NULL, program);
    assert(data && !strcmp(data, program));
    free(data);

    /* an AppImage: its programs run from the mount, the copy is the AppImage file's folder */
    file = join(copy, "ScarletNX.AppImage");
    assert(!snx_write_file(file, "", 0));
    setenv("APPIMAGE", file, 1);
    setenv("APPDIR", mount, 1);
    free(file);
    file = join(mount, "bin");
    data = snx_data_dir(NULL, file);
    assert(data && !strcmp(data, expected)); /* no games/ beside the AppImage yet */
    free(data);
    data = join(copy, "games");
    assert(!snx_mkdirs(data));
    free(data);
    data = snx_data_dir(NULL, file);
    assert(data && !strcmp(data, copy));
    free(data);
    free(file);
    /* APPIMAGE inherited by a program outside the mount: that program's own folder decides */
    data = snx_data_dir(NULL, program);
    assert(data && !strcmp(data, program));
    free(data);

    puts("snx_data_dir: checkout, beside the program, beside the AppImage, the user's data folder");
    free(expected);
    free(xdg);
    free(copy);
    free(mount);
    free(program);
    free(checkout);
    free(root);
    return 0;
}
