/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the display service (vi.c) */
#ifndef SNX_VI_H
#define SNX_VI_H

#define VI_REFRESH_HZ 60 /* the console's display, which vsync ticks and the game's swap interval count in */

void vi_vsync_tick(void);

#endif
