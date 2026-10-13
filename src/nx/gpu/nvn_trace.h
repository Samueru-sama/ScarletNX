/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_TRACE_H
#define SNX_NVN_TRACE_H

/* Opt-in inventory of NVN function pointers requested through the SDK bootstrap loader. */
void nvn_trace_init(void);
void nvn_trace_report(void);

#endif
