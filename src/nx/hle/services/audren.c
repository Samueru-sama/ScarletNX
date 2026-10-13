/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the audio renderer's service side: each IAudioRenderer keeps an ARState (audren_dsp.c) that RequestUpdate
   fills, and the audio clock renders a frame of every started one each 5 ms, before it signals the renderer
   event, sending the device sink's output to the host */
#include "hle/services/services_int.h"
#include "hle/services/audren_dsp.h"
#include "hle/kernel/ipc.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

typedef struct ARHost {
    struct ARHost* next;
    SvcObj* obj;
    ARState* st;
    PMutex lk;   /* the state, between RequestUpdate and the clock's frame */
    PAudio* out; /* the host output, opened by the first frame with a device sink */
    i16* stereo;
} ARHost;
static PMutex ar_lk = PMUTEX_INIT; /* the list, and a host's removal from it */
static ARHost* ar_list;

static void ar_destroy(SvcObj* o) {
    ARHost* h = (ARHost*)(size_t)o->u[4];
    if (!h) return;
    pm_lock(&ar_lk);
    for (ARHost** at = &ar_list; *at; at = &(*at)->next)
        if (*at == h) {
            *at = h->next;
            break;
        }
    pm_unlock(&ar_lk);
    if (h->out) plat_audio_close(h->out);
    audren_state_free(h->st);
    free(h->stereo);
    free(h);
    o->u[4] = 0;
}

/* OpenAudioRenderer's AudioRendererParameter (0x34 bytes) */
void audren_open(SvcObj* o, const u8* params) {
    ARHost* h = calloc(1, sizeof *h);
    if (!h) return;
    h->obj = o;
    h->st = audren_state_new(params);
    h->stereo = h->st ? calloc((size_t)h->st->frame * 2, sizeof *h->stereo) : NULL;
    if (!h->st || !h->stereo) {
        WARN("audren: no renderer state for these parameters (rate %u, %u samples a frame)\n",
             (unsigned)(params[0] | params[1] << 8 | params[2] << 16), (unsigned)(params[4] | params[5] << 8));
        audren_state_free(h->st);
        free(h->stereo);
        free(h);
        return;
    }
    pm_init(&h->lk);
    o->u[4] = (u64)(size_t)h;
    o->destroy = ar_destroy;
    pm_lock(&ar_lk);
    h->next = ar_list;
    ar_list = h;
    pm_unlock(&ar_lk);
    LOG("audren: opened at %u Hz, %u samples a frame, %u mix buffers, %u voices, %u effects, %u sinks\n", h->st->rate,
        h->st->frame, h->st->buffers, h->st->nvoices, h->st->neffects, h->st->nsinks);
}

int audren_update(SvcObj* o, IpcCtx* c) {
    u64 ia, is, oa, os;
    buf_in(c, 0, &ia, &is);
    buf_out(c, 0, &oa, &os);
    if (!ia || !oa || is < 0x40) return RC(153, 41); /* InvalidUpdateInfo */
    ARHost* h = (ARHost*)(size_t)o->u[4];
    if (!h) return RC(153, 2);
    pm_lock(&h->lk);
    int rc = audren_state_update(h->st, GP(ia), is, GP(oa), os);
    pm_unlock(&h->lk);
    if (rc == -41) return RC(153, 41);
    if (rc == -4) return RC(153, 4);
    if (rc) return RC(153, 2);
    LOG_LIMIT(1, "audren: rendering, revision %d\n", (int)((h->st->rev >> 24) & 0xFF) - '0');
    return 0;
}

/* the audio clock's 5 ms: a frame of each started renderer (u[3] 0), its device sink queued for the host. A host
   queue past 100 ms means the device takes less than the clock gives: that frame is dropped, so the delay stays
   bounded */
void audren_tick(void) {
    pm_lock(&ar_lk);
    for (ARHost* h = ar_list; h; h = h->next) {
        if (__atomic_load_n(&h->obj->u[3], __ATOMIC_ACQUIRE) != 0) continue;
        pm_lock(&h->lk);
        int device = audren_frame(h->st, h->stereo);
        pm_unlock(&h->lk);
        if (!device) continue;
        if (!h->out) {
            h->out = plat_audio_open(h->st->rate, 2);
            if (h->out) plat_audio_start(h->out, 1);
        }
        u32 bytes = h->st->frame * 4;
        if (h->out && plat_audio_queued(h->out) <= h->st->rate / 10 * 4) plat_audio_queue(h->out, h->stereo, bytes);
    }
    pm_unlock(&ar_lk);
}
