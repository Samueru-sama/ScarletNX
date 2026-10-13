/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GDBSTUB_H
#define SNX_GDBSTUB_H
#define SNX_GDB_PORT 1234
/* Native host debugging via a separate UNIX gdbserver. Call before threads start. */
int snx_gdb_start(int stop);
#endif
