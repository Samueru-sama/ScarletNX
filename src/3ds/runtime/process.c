/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/internal.h"
#include "3ds/gpu/pica.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common/utilities/common.h"
static bool map(SnxCtrProcess* p, uint32_t va, uint32_t size, unsigned permissions) {
    if (!size) return true;
    if ((uint64_t)p->physical_next + size > p->physical_end ||
        snx_ctr_map(p->space, va, p->physical_next, size, permissions) != SNX_CTR_MEM_OK)
        return false;
    memset(p->physical_owner + (p->physical_next - CTR_PHYS_BASE) / 4096, CTR_STATIC, size / 4096);
    p->physical_next += size;
    return true;
}
/* The ExHeader's address-mapping capabilities, as Azahar's Process::ParseKernelCaps and
   KernelSystem::HandleSpecialMapping (core/hle/kernel/{process,memory}.cpp, GPL-2.0-or-later, Copyright 2014-2025
   Citra Emulator Project / Azahar Emulator Project, azahar-emu/azahar a4b335e) read and apply them: a range is two
   descriptors (type 0x7f, start page, read-only bit 20, then the end page), a page is one (type 0xffe). Mappings
   resolve against the special areas in that order, VRAM before IO (their virtual ranges overlap). MMIO areas are
   not mapped, as Azahar leaves them, so an access to one faults instead of reading invented values. */
static bool special_mappings(SnxCtrProcess* p, const uint32_t* caps) {
    static const struct {
        uint32_t va, pa, size;
    } areas[] = {
        {0x1f000000u, 0x18000000u, 0x00600000u},            /* VRAM */
        {0x1ec00000u, 0x10100000u, 0x00400000u},            /* IO: not mapped */
        {0x1ff00000u, 0x1ff00000u, 0x00080000u},            /* DSP RAM */
        {0x1e800000u, 0x1f000000u, 0x00400000u - 0x20000u}, /* New 3DS extra RAM */
    };
    for (unsigned i = 0; i < 28; ++i) {
        uint32_t d = caps[i], type = d >> 20, va, size, permissions;
        if ((type & 0xffe) == 0xff8) {
            if (i + 1 >= 28 || ((caps[i + 1] >> 20) & 0xffe) != 0xff8) continue; /* incomplete: ignored */
            uint32_t end = caps[++i] << 12;
            va = d << 12;
            size = va < end ? end - va : 0;
            permissions = (d & (1u << 20)) ? SNX_CTR_READ : SNX_CTR_READ | SNX_CTR_WRITE;
        } else if ((type & 0xfff) == 0xffe) {
            va = d << 12;
            size = 4096;
            permissions = SNX_CTR_READ | SNX_CTR_WRITE;
        } else
            continue;
        if (!size) continue;
        unsigned area = 0;
        while (area < 4 &&
               !(va >= areas[area].va && (uint64_t)va + size <= (uint64_t)areas[area].va + areas[area].size))
            ++area;
        if (area == 4 || area == 1) continue; /* unhandled or MMIO */
        uint32_t pa = areas[area].pa + (va - areas[area].va);
        /* an area this board lacks (extra RAM on an Old 3DS) has no physical region: left unmapped */
        if (!snx_ctr_physical_region(snx_ctr_memory_profile(p->memory), pa, size)) continue;
        if (snx_ctr_map(p->space, va, pa, size, permissions) != SNX_CTR_MEM_OK) return false;
    }
    return true;
}
void snx_ctr_process_free(SnxCtrProcess* p) {
    if (!p) return;
    free(p->gsp.regs);
    if (p->gsp.pica) snx_ctr_pica_free(p->gsp.pica);
    free(p->gsp.pica);
    snx_ctr_romfs_free(p->romfs);
    if (p->tree) {
        fs_tree_destroy(p->tree);
        pm_destroy(&p->tree->lock);
        free(p->tree);
    }
    snx_ctr_space_free(p->space);
    snx_ctr_memory_free(p->memory);
    free(p);
}
SnxCtrProcess* snx_ctr_process_new(const SnxCtrImage* image, SnxCtrModel model) {
    const SnxCtrProfile* profile = snx_ctr_profile(model);
    if (!image || !image->code || !profile || !image->stack_size || image->stack_size > 0x800000) return NULL;
    uint32_t app_limit = model == SNX_CTR_OLD ? 0x4000000 : 0x7c00000;
    SnxCtrProcess* p = calloc(1, sizeof *p + app_limit / 4096);
    if (!p) return NULL;
    p->app_limit = app_limit;
    p->gsp.rights_thread = UINT32_MAX; /* no GPU right holder until an AcquireRight */
    p->physical_next = CTR_PHYS_BASE;
    p->physical_end = p->physical_next + p->app_limit;
    p->objects[1] = (CtrObject){.type = CTR_OBJ_PROCESS, .refs = 1};
    p->objects[2] = (CtrObject){.type = CTR_OBJ_THREAD, .refs = 1};
    p->current_thread = 2;
    p->objects[2].data.thread.priority = 0x30;
    p->memory = snx_ctr_memory_new(model);
    if (!p->memory || !(p->space = snx_ctr_space_new(p->memory))) goto fail;
    size_t offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const SnxCtrSegment* s = &image->segment[i];
        uint64_t bytes = (uint64_t)s->pages * 4096;
        uint64_t size = bytes + (i == 2 ? ((uint64_t)image->bss_size + 4095) & ~UINT64_C(4095) : 0);
        if (size > UINT32_MAX || bytes > image->code_size - offset ||
            !map(p, s->address, (uint32_t)size, SNX_CTR_READ | SNX_CTR_WRITE))
            goto fail;
        if (bytes && snx_ctr_write(p->space, s->address, image->code + offset, (size_t)bytes) != SNX_CTR_MEM_OK)
            goto fail;
        unsigned permissions = i == 0   ? SNX_CTR_READ | SNX_CTR_EXEC
                               : i == 1 ? SNX_CTR_READ
                                        : SNX_CTR_READ | SNX_CTR_WRITE;
        if (size && snx_ctr_protect(p->space, s->address, (uint32_t)size, permissions) != SNX_CTR_MEM_OK) goto fail;
        offset += (size_t)bytes;
    }
    uint32_t stack = (image->stack_size + 4095) & ~4095u;
    if (offset != image->code_size || !map(p, SNX_CTR_STACK_TOP - stack, stack, SNX_CTR_READ | SNX_CTR_WRITE) ||
        !map(p, SNX_CTR_INITIAL_TLS, 4096, SNX_CTR_READ | SNX_CTR_WRITE) ||
        !snx_ctr_cpu_init(&p->cpu, p->space, image->segment[0].address, SNX_CTR_STACK_TOP, false))
        goto fail;
    p->cpu.state.cpu.tpidruro = SNX_CTR_INITIAL_TLS;
    p->objects[2].data.thread.context = p->cpu.state.cpu;
    /* Configuration-page layout and kernel personality facts from Citra/Azahar
       config_mem.{h,cpp}, memory.cpp (GPL-2.0-or-later, revision in docs/3DS.md).
       No firmware is loaded. User mappings are read-only. */
    /* Board AXI WRAM, not application FCRAM: do not charge this kernel page
       to the application's heap quota or alias it to application allocations. */
    if (snx_ctr_map(p->space, CTR_CONFIG, CTR_CONFIG, 4096, SNX_CTR_READ | SNX_CTR_WRITE) != SNX_CTR_MEM_OK) goto fail;
    unsigned char config[4096] = {0};
    config[2] = config[0x62] = 0x3a;
    config[3] = config[0x63] = 2;
    config[0x14] = 1;
    config[0x16] = 1;
    config[0x10] = config[0x64] = 2;
    config[0x18] = config[0x68] = 0x50;
    config[0x19] = config[0x69] = 0xf4;
    uint32_t sizes[] = {p->app_limit, model == SNX_CTR_OLD ? 0x2c00000u : 0x6400000u,
                        model == SNX_CTR_OLD ? 0x1400000u : 0x2000000u};
    config[0x30] = model == SNX_CTR_OLD ? 0 : 6;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j) config[0x40 + i * 4 + j] = (unsigned char)(sizes[i] >> (j * 8));
    if (snx_ctr_write_ram(p->space, CTR_CONFIG, config, sizeof config) != SNX_CTR_MEM_OK ||
        snx_ctr_protect(p->space, CTR_CONFIG, 4096, SNX_CTR_READ) != SNX_CTR_MEM_OK)
        goto fail;
    /* Shared-page time snapshot (Azahar shared_page.h): UTC milliseconds since
       1900 plus an ARM11 base-clock reference. Clients interpolate via SVC 28.
       Host-paced monotonic ticks, not an instruction/cycle-accuracy claim. */
    struct timespec now;
    if (
#if defined(__ANDROID__) && __ANDROID_API__ < 29
        clock_gettime(CLOCK_REALTIME, &now)
#else
        !timespec_get(&now, TIME_UTC)
#endif
        || now.tv_sec < 0 || (uint64_t)now.tv_sec > (UINT64_MAX - 999) / 1000 - UINT64_C(2208988800))
        goto fail;
    p->clock_origin = snx_seconds();
    uint64_t rtc = ((uint64_t)now.tv_sec + UINT64_C(2208988800)) * 1000 + (uint64_t)now.tv_nsec / 1000000;
    unsigned char shared[4096] = {0};
    for (unsigned bank = 0; bank < 2; ++bank) {
        for (unsigned i = 0; i < 8; ++i) {
            shared[0x20 + bank * 0x20 + i] = (unsigned char)(rtc >> (i * 8));
            shared[0x30 + bank * 0x20 + i] = (unsigned char)(UINT64_C(268111856) >> (i * 8));
        }
    }
    if (snx_ctr_map(p->space, 0x1ff81000, 0x1ff81000, 4096, 3) != SNX_CTR_MEM_OK ||
        snx_ctr_write_ram(p->space, 0x1ff81000, shared, sizeof shared) != SNX_CTR_MEM_OK ||
        snx_ctr_protect(p->space, 0x1ff81000, 4096, SNX_CTR_READ) != SNX_CTR_MEM_OK)
        goto fail;
    if (!special_mappings(p, image->kernel_caps)) goto fail;
    return p;
fail:
    snx_ctr_process_free(p);
    return NULL;
}
/* One NX-style shared tree over the extracted title directory: romfs and
   exefs are shared read-only and always open; sdmc is the read-write area,
   created on demand. The process owns the tree. */
bool snx_ctr_process_mount_fs(SnxCtrProcess* p, const char* directory) {
    if (!p || !directory || !*directory || p->tree) return false;
    FsTree* tree = calloc(1, sizeof *tree);
    if (!tree) return false;
    if (fs_tree_init(tree, directory) != FS_TREE_OK) {
        pm_destroy(&tree->lock);
        free(tree);
        return false;
    }
    p->tree = tree;
    pm_lock(&tree->lock);
    unsigned shared = 0, failed = 0;
    bool ok = true;
    for (unsigned i = 0; i < 2 && ok; ++i) {
        FsNode* node = NULL;
        if (!fs_tree_lookup(tree, NULL, i ? "exefs" : "romfs", &node))
            ok = !fs_tree_share(tree, node, &shared, &failed);
    }
    pm_unlock(&tree->lock);
    if (!ok || !snx_ctr_romfs_open(tree, &p->romfs)) ok = false;
    FsNode* sd = NULL;
    if (ok) {
        pm_lock(&tree->lock);
        FsTreeResult rc = fs_tree_lookup(tree, NULL, "sdmc", &sd);
        if (rc == FS_TREE_NOTFOUND) rc = fs_tree_create(tree, NULL, "sdmc", 1, 0, &sd);
        pm_unlock(&tree->lock);
        ok = !rc && sd;
    }
    if (!ok) { /* leave no partial mount behind; the process itself stays the caller's */
        snx_ctr_romfs_free(p->romfs);
        p->romfs = NULL;
        fs_tree_destroy(p->tree);
        pm_destroy(&p->tree->lock);
        free(p->tree);
        p->tree = NULL;
        p->sd = NULL;
        return false;
    }
    p->sd = sd;
    return true;
}
/* HLE shared pages come from board FCRAM past the application pool, outside
   pool accounting: each call reserves the next page-aligned block. */
uint32_t snx_ctr_shared_pages(SnxCtrProcess* p, uint32_t size) {
    if (!p || !size || (size & 4095)) return 0;
    const SnxCtrProfile* profile = snx_ctr_memory_profile(p->memory);
    uint32_t limit = CTR_PHYS_BASE + profile->fcram_size;
    if (!p->physical_shared) p->physical_shared = p->physical_end;
    if ((uint64_t)p->physical_shared + size > limit) return 0;
    uint32_t at = p->physical_shared;
    p->physical_shared += size;
    return at;
}
SnxArmCPU* snx_ctr_process_cpu(SnxCtrProcess* p) { return p ? &p->cpu.state.cpu : NULL; }
SnxCtrAddressSpace* snx_ctr_process_space(SnxCtrProcess* p) { return p ? p->space : NULL; }
