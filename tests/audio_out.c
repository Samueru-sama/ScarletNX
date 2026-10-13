/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the production audout IPC commands with a deterministic clock and a copying PCM sink. */
#include "hle/services/services_int.h"
#include <stdarg.h>

#include "hle/services/audio.c"

u8* g_mem;
int g_verbose;
static u64 test_now;
static unsigned test_checks, test_objects, test_events;
static int test_no_device;
struct PAudio {
    u8 pcm[32768];
    u32 bytes;
    unsigned queued, cleared;
    int started;
    float volume;
};

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "audio_out.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

/* This single-threaded fixture checks timing and ownership, without needing an audio device. */
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
u64 plat_ns(void) { return test_now; }
void plat_yield(void) { test_fail("unexpected initialization contention", __LINE__); }
void plat_sleep_ns(i64 ns) {
    (void)ns;
    test_fail("the test must not start the audio clock thread", __LINE__);
}
PAudio* plat_audio_open(u32 rate, u32 channels) {
    CHECK(rate == 48000 && channels == 2);
    if (test_no_device) return NULL;
    PAudio* a = calloc(1, sizeof *a);
    CHECK(a != NULL);
    a->volume = 1.0f;
    return a;
}
void plat_audio_close(PAudio* a) { free(a); }
int plat_audio_queue(PAudio* a, const void* samples, u32 bytes) {
    CHECK(a && bytes && bytes <= sizeof a->pcm - a->bytes);
    memcpy(a->pcm + a->bytes, samples, bytes);
    a->bytes += bytes;
    a->queued++;
    return 1;
}
void plat_audio_start(PAudio* a, int started) {
    if (a) a->started = started;
}
void plat_audio_clear(PAudio* a) {
    if (a) {
        a->bytes = 0;
        a->cleared++;
    }
}
void plat_audio_volume(PAudio* a, float volume) {
    if (a) a->volume = volume;
}
void mem_query(u64 addr, MemBlock* b) {
    memset(b, 0, sizeof *b);
    b->addr = addr < 0x8000 ? 0 : 0x8000;
    b->size = addr < 0x8000 ? 0x8000 : AS_SIZE - 0x8000;
    b->perm = addr < 0x8000 ? PERM_RW : PERM_NONE;
}
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t size) {
    SvcObj* obj = calloc(1, sizeof *obj);
    CHECK(obj != NULL);
    obj->name = name;
    obj->dispatch = dispatch;
    obj->refs = 1;
    if (size) {
        obj->data = calloc(1, size);
        CHECK(obj->data != NULL);
    }
    test_objects++;
    return obj;
}
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    if (obj->destroy) obj->destroy(obj);
    free(obj->data);
    free(obj);
    test_objects--;
}
void out_obj(IpcCtx* c, SvcObj* obj) {
    CHECK(c->nout_objs < 8);
    c->out_objs[c->nout_objs++] = obj;
}
void out_copy_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_copy < 8);
    c->out_copy[c->nout_copy++] = handle;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    test_fail("unexpected stub command", __LINE__);
    return -1;
}
int quiet_ok(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
int audren_update(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
int buf_in(IpcCtx* c, int i, u64* addr, u64* size) {
    *addr = *size = 0;
    if (i >= c->na) return 0;
    *addr = c->a[i].addr;
    *size = c->a[i].size;
    return 1;
}
int buf_out(IpcCtx* c, int i, u64* addr, u64* size) {
    *addr = *size = 0;
    if (i >= c->nb) return 0;
    *addr = c->b[i].addr;
    *size = c->b[i].size;
    return 1;
}
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.name = name;
    test_events++;
    return e;
}
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    CHECK(o->refs > 0);
    if (--o->refs) return;
    free(o);
    test_events--;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
}
u32 ev_handle(KEvent* e) {
    CHECK(e != NULL);
    return 7;
}

static IpcCtx request(u32 cmd, const void* raw, u32 size) {
    IpcCtx c;
    memset(&c, 0, sizeof c);
    c.cmd = cmd;
    c.raw = raw;
    c.raw_size = size;
    return c;
}
static u32 reply32(IpcCtx* c, u32 off) {
    u32 v;
    CHECK(off + 4 <= c->out_size);
    memcpy(&v, c->out + off, sizeof v);
    return v;
}
static u64 query64(SvcObj* o, u32 cmd) {
    IpcCtx c = request(cmd, NULL, 0);
    u64 v;
    CHECK(o->dispatch(o, &c) == 0 && c.out_size == 8);
    memcpy(&v, c.out, sizeof v);
    return v;
}
static void command(SvcObj* o, u32 cmd) {
    IpcCtx c = request(cmd, NULL, 0);
    CHECK(o->dispatch(o, &c) == 0);
}
static SvcObj* open_output(SvcObj* manager) {
    u32 args[2] = {48000, 2};
    IpcCtx c = request(3, args, sizeof args);
    CHECK(manager->dispatch(manager, &c) == 0 && c.nout_objs == 1);
    CHECK(reply32(&c, 0) == 48000 && reply32(&c, 4) == 2 && reply32(&c, 8) == 2 && reply32(&c, 12) == 1);
    return c.out_objs[0];
}
static int append(SvcObj* o, u32 cmd, u64 tag, u64 sample_addr, u64 capacity, u64 bytes, u64 offset) {
    u64 descriptor[5] = {0, sample_addr, capacity, bytes, offset};
    memcpy(GP(0x100), descriptor, sizeof descriptor);
    IpcCtx c = request(cmd, &tag, sizeof tag);
    c.na = 1;
    c.a[0].addr = 0x100;
    c.a[0].size = sizeof descriptor;
    return o->dispatch(o, &c);
}
static u32 released(SvcObj* o, u32 cmd) {
    IpcCtx c = request(cmd, NULL, 0);
    c.nb = 1;
    c.b[0].addr = 0x200;
    c.b[0].size = 64 * 8;
    memset(GP(0x200), 0xFF, 64 * 8);
    CHECK(o->dispatch(o, &c) == 0);
    return reply32(&c, 0);
}

int main(void) {
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    SvcObj* manager = new_audout();
    SvcObj* o = open_output(manager);
    AudOut* q = o->data;
    PAudio* host = q->output;
    CHECK(host && !host->started);
    for (u32 i = 0; i < 196; i++) GP(0x400)[i] = (u8)(i * 7);
    u8 expected[192];
    memcpy(expected, GP(0x404), sizeof expected);
    CHECK(append(o, 7, 0xCAFE, 0x400, 196, 192, 4) == 0);
    CHECK(host->queued == 1 && host->bytes == 192 && !memcmp(host->pcm, expected, 192));
    memset(GP(0x400), 0, 196); /* The host owns its copy, even when the guest later reuses the memory. */
    CHECK(!memcmp(host->pcm, expected, 192));

    test_now = 1000000000;
    CHECK(released(o, 8) == 0 && gr64(0x200) == 0);
    command(o, 1);
    CHECK(host->started);
    test_now += 500000;
    CHECK(query64(o, 10) == 24);
    command(o, 2);
    CHECK(!host->started);
    test_now += 1000000000;
    CHECK(query64(o, 10) == 24 && released(o, 5) == 0);
    command(o, 1);
    test_now += 500000;
    CHECK(released(o, 8) == 1 && gr64(0x200) == 0xCAFE && gr64(0x208) == 0);
    CHECK(query64(o, 10) == 48);

    float volume = 0.25f;
    IpcCtx c = request(12, &volume, sizeof volume);
    CHECK(o->dispatch(o, &c) == 0 && host->volume == volume);
    c = request(13, NULL, 0);
    CHECK(o->dispatch(o, &c) == 0 && c.out_size == 4 && !memcmp(c.out, &volume, 4));
    CHECK(append(o, 3, 0xBEEF, 0x400, 196, 192, 0) == 0);
    command(o, 11);
    CHECK(host->cleared == 1 && host->bytes == 0);
    CHECK(released(o, 5) == 1 && gr64(0x200) == 0xBEEF && gr64(0x208) == 0);
    CHECK(query64(o, 10) == 48); /* Flushing does not count the discarded samples as played. */

    command(o, 2);
    unsigned before = host->queued;
    CHECK(append(o, 7, 1, 0x7FF0, 192, 192, 0) == 0); /* spans an unreadable allocation */
    CHECK(host->queued == before);
    command(o, 11);
    CHECK(released(o, 8) == 1);
    for (u32 i = 0; i < 64; i++) CHECK(append(o, 7, 100 + i, 0x400, 196, 192, 0) == 0);
    CHECK(append(o, 7, 999, 0x400, 196, 192, 0) == RC(153, 8));
    CHECK(host->queued == before + 64); /* A full ring must reject the host copy too. */
    command(o, 11);
    CHECK(released(o, 8) == 64 && gr64(0x200) == 100 && gr64(0x3F8) == 163);
    svcobj_unref(o);
    CHECK(audio.outs == NULL);

    test_no_device = 1;
    o = open_output(manager);
    CHECK(((AudOut*)o->data)->output == NULL);
    CHECK(append(o, 7, 77, 0x400, 196, 192, 0) == 0);
    command(o, 1);
    test_now += 1000000;
    CHECK(released(o, 8) == 1 && gr64(0x200) == 77 && query64(o, 10) == 48);
    svcobj_unref(o);
    svcobj_unref(manager);
    CHECK(test_objects == 0 && test_events == 0 && audio.outs == NULL);
    free(g_mem);
    printf("audout: %u checks passed\n", test_checks);
    return 0;
}
