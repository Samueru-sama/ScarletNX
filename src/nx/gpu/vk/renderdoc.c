/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc captures of chosen frames, for unattended runs: SNX_RENDERDOC=PREFIX loads RenderDoc's library before the
   Vulkan instance exists (its capture layer is an implicit one, which the loader enables with
   ENABLE_VULKAN_RENDERDOC_CAPTURE), and SNX_RENDERDOC_FRAMES=N,M,... names the presents to capture. A capture holds one
   guest frame: everything recorded after present N-1 up to present N, written to PREFIX_frameK.rdc (K is RenderDoc's
   own count). Frames that never reach the swapchain (offscreen, headless) are captured the same way, because the
   capture starts and ends here rather than at RenderDoc's present hook. The in-application API is RenderDoc's
   renderdoc_app.h (MIT, Baldur Karlsson), fetched by cmake/dependencies.cmake. */
#include <stdlib.h>
#include <string.h>
#include "gpu/vk/vk.h"
#include "renderdoc_app.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"

#define RD_FRAMES 64 /* presents to capture at most */

static RENDERDOC_API_1_1_2* g_rd;
static u64 g_rd_frames[RD_FRAMES];
static int g_rd_nframes, g_rd_open;

void vk_renderdoc_load(void) {
    const char *prefix = getenv("SNX_RENDERDOC"), *frames = getenv("SNX_RENDERDOC_FRAMES"), *p;
    pRENDERDOC_GetAPI get_api;
    void* lib;
    if (!prefix || !*prefix) return;
#ifdef _WIN32
    lib = plat_dl_open("renderdoc.dll");
#else
    lib = plat_dl_open("librenderdoc.so");
#endif
    if (!lib) {
        WARN("renderdoc: SNX_RENDERDOC is set but RenderDoc's library didn't load: %s\n", plat_dl_error());
        return;
    }
    get_api = (pRENDERDOC_GetAPI)plat_dl_sym(lib, "RENDERDOC_GetAPI");
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_1_2, (void**)&g_rd) || !g_rd) {
        WARN("renderdoc: the library has no in-application API 1.1.2\n");
        g_rd = NULL;
        return;
    }
    /* the capture layer comes in through the loader, which only enables it with this set */
    if (plat_setenv("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1")) WARN("renderdoc: couldn't enable the capture layer\n");
    g_rd->SetCaptureFilePathTemplate(prefix);
    /* no overlay in the captured frames, and nothing taken but the presents asked for */
    g_rd->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    g_rd->SetCaptureKeys(NULL, 0);
    for (p = frames; p && *p && g_rd_nframes < RD_FRAMES;) {
        char* e;
        unsigned long long n = strtoull(p, &e, 10);
        if (e == p) break;
        if (n) g_rd_frames[g_rd_nframes++] = n;
        p = *e == ',' ? e + 1 : e;
    }
    if (p && *p) WARN("renderdoc: SNX_RENDERDOC_FRAMES: \"%s\" and what follows are ignored\n", p);
    LOG("renderdoc: loaded, %d frame(s) to capture to %s\n", g_rd_nframes, prefix);
}

static int rd_wanted(u64 present) {
    int i;
    for (i = 0; i < g_rd_nframes; i++)
        if (g_rd_frames[i] == present) return 1;
    return 0;
}
/* at the present that ends frame `present`, after its last submit and before the swapchain present, under vk_lock: the
   capture of this frame ends here, and the next frame's starts */
void vk_renderdoc_present(u64 present) {
    void* dev;
    if (!g_rd || !g_vk.instance) return;
    dev = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(g_vk.instance);
    if (g_rd_open) {
        g_rd_open = 0;
        if (g_rd->EndFrameCapture(dev, NULL)) {
            char path[1024];
            u32 len = 0, n = g_rd->GetNumCaptures();
            /* GetCapture writes the whole path, unbounded: its length first */
            if (n && g_rd->GetCapture(n - 1, NULL, &len, NULL) && len < sizeof path &&
                g_rd->GetCapture(n - 1, path, NULL, NULL))
                LOG("renderdoc: captured present %llu to %s\n", (unsigned long long)present, path);
        } else
            WARN("renderdoc: the capture of present %llu failed\n", (unsigned long long)present);
    }
    if (rd_wanted(present + 1)) {
        g_rd->StartFrameCapture(dev, NULL);
        g_rd_open = 1;
    }
}
