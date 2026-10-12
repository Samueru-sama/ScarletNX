/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_program.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <string.h>

/* Program identity and setup status only; shader descriptors vary between inspected SDKs. */
typedef struct {
    u64 key, generation;
    u32 shader_count;
    u8 alive, configured;
} Program;
#define PROGRAM_SLOTS 16384
static Program programs[PROGRAM_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, init_failed, finalized, live, reinitialized, unknown_finalizes;
static u64 shader_sets, shader_set_failed, shader_set_untracked, table_full;

static Program* program_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = (u32)((key >> 4) * 2654435761u) & (PROGRAM_SLOTS - 1);
    for (u32 step = 0; step < PROGRAM_SLOTS; step++) {
        Program* p = &programs[(home + step) & (PROGRAM_SLOTS - 1)];
        if (p->key == key) return p;
        if (!p->key) return create ? p : NULL;
    }
    return NULL;
}

int nvn_program_wants(const char* name) {
    return !strcmp(name, "nvnProgramInitialize") || !strcmp(name, "nvnProgramFinalize") ||
           !strcmp(name, "nvnProgramSetShaders") || !strcmp(name, "nvnProgramSetShadersExt");
}

void nvn_program_before(const CPU* c, const char* name, NvnProgramCall* call) {
    call->program = c->x[0];
    call->shader_count =
        !strcmp(name, "nvnProgramSetShaders") || !strcmp(name, "nvnProgramSetShadersExt") ? (u32)c->x[1] : 0;
}

void nvn_program_after(const CPU* c, const char* name, const NvnProgramCall* call) {
    pm_lock(&lock);
    if (!strcmp(name, "nvnProgramInitialize")) {
        if (!c->x[0]) {
            init_failed++;
        } else {
            Program* p = program_slot(call->program, 1);
            if (!p)
                table_full++;
            else {
                if (p->alive)
                    reinitialized++;
                else
                    live++;
                p->key = call->program;
                p->generation++;
                p->shader_count = 0;
                p->configured = 0;
                p->alive = 1;
                initialized++;
            }
        }
    } else if (!strcmp(name, "nvnProgramFinalize")) {
        Program* p = program_slot(call->program, 0);
        if (!p || !p->alive)
            unknown_finalizes++;
        else {
            p->alive = 0;
            live--;
            finalized++;
        }
    } else {
        Program* p = program_slot(call->program, 0);
        if (!p || !p->alive)
            shader_set_untracked++;
        else if (!c->x[0])
            shader_set_failed++;
        else {
            p->configured = 1;
            p->shader_count = call->shader_count;
            shader_sets++;
        }
    }
    pm_unlock(&lock);
}

int nvn_program_check(u64 program) {
    pm_lock(&lock);
    Program* p = program_slot(program, 0);
    int result = p && p->alive ? (p->configured ? 1 : -1) : 0;
    pm_unlock(&lock);
    return result;
}

void nvn_program_report(void) {
    pm_lock(&lock);
    LOG("nvn-program: initialized=%llu init-failed=%llu finalized=%llu live=%llu reinitialized=%llu unknown-finalizes=%llu shader-sets=%llu shader-set-failed=%llu shader-set-untracked=%llu table-full=%llu\n",
        (unsigned long long)initialized, (unsigned long long)init_failed, (unsigned long long)finalized,
        (unsigned long long)live, (unsigned long long)reinitialized, (unsigned long long)unknown_finalizes,
        (unsigned long long)shader_sets, (unsigned long long)shader_set_failed,
        (unsigned long long)shader_set_untracked, (unsigned long long)table_full);
    pm_unlock(&lock);
}
