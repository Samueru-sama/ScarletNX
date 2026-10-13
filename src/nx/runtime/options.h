/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the settings from <Game>.ini (options.c) */
#ifndef SNX_OPTIONS_H
#define SNX_OPTIONS_H

int snx_startup_options(int argc, char** argv); /* shared startup flags, before any window/error dialog */
extern int g_opt_headless;                      /* startup only, never changed by an INI reload */
extern int g_opt_gdb, g_opt_gdb_stop;

void ini_override(const char* k, const char* v); /* a --key=value from the command line, before ini_open. Keeps v */
void ini_set(const char* k, const char* v);
void ini_open(const char* path); /* watch the file: ini_poll reloads it when it changes, and writes back what changed */
void ini_poll(void);
extern int g_opt_docked, g_opt_lang, g_opt_fps;
extern int g_opt_diag, g_opt_shadows, g_opt_prof;
extern int g_opt_network; /* network: 1 = the game is online through the host (hle/services/network) */
extern float g_opt_resf;

#endif
