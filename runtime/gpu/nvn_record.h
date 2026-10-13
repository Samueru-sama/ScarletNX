/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_RECORD_H
#define SNX_NVN_RECORD_H
#include "abi/cpu.h"

int nvn_record_wants(const char* name);
void nvn_record_before(const CPU* c, const char* name);
void nvn_record_after(const CPU* c, const char* name, u64 command_buffer);
void nvn_record_report(void);
void nvn_record_enable_descriptor_replay(void);
void nvn_record_enable_shader_transport(void);
void nvn_record_enable_clear_execute(void);
void nvn_record_enable_copy_execute(void);
void nvn_record_enable_draw_prepare(void);
void nvn_record_enable_texture_load(void);
void nvn_record_enable_attachment_prepare(void);
void nvn_record_enable_shader_materialize(void);
void nvn_record_set_view_layout(u32 first_instruction, u32 second_instruction, u32 third_instruction);
#endif
