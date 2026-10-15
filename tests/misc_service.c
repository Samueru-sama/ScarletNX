/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* mm:u (misc.c) and hid's IHidServer (hid.c) through their generated dispatchers, with the kernel objects they hand
   out made here and the shared memory a host buffer */
#include "hle/services/services_int.h"
#include <stdarg.h>

#include "hle/services/hid.c"
#include "hle/services/misc.c"

u8* g_mem;
int g_verbose;
int g_opt_docked, g_opt_lang = 1;
u64 g_title_id = 0x0100BF00112C0000ull;
PMutex g_klock;
static unsigned test_checks, test_objects, test_kobjs;
static u32 test_next_handle = 0x100;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "misc_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(1);
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
/* sf's InvalidCmifRequest, for a request that doesn't send what its command declares */
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
/* sf's InvalidNumInObjects, for a domain request with another number of objects */
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    fprintf(stderr, "unexpected stub: %s command %u\n", o->name, c->cmd);
    test_fail("a command without a handler", __LINE__);
    return -1;
}
int quiet_ok(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}

/* kernel objects: a handle takes a reference, as h_new does, and the test keeps count of what is alive */
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    CHECK(o->refs > 0);
    if (--o->refs) return;
    if (o->destroy) o->destroy(o);
    free(o);
    test_kobjs--;
}
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.type = KO_EVENT;
    e->o.name = name;
    test_kobjs++;
    return e;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
static void shm_destroy(KObj* o) { free(((KShmem*)o)->host); }
KShmem* kshmem_new(u64 size, const char* name) {
    KShmem* s = calloc(1, sizeof *s);
    CHECK(s != NULL);
    s->o.type = KO_SHMEM;
    s->o.name = name;
    s->o.destroy = shm_destroy;
    s->size = size;
    s->host = calloc(1, size);
    CHECK(s->host != NULL);
    test_kobjs++;
    return s;
}
u8* kshmem_ptr(KShmem* s) { return s->host; }
/* the handle table: the handle a test reads back names the object it was made for */
static KObj* test_handles[64];
u32 h_new(KObj* o) {
    CHECK(test_next_handle - 0x100 < 64);
    kobj_ref(o);
    test_handles[test_next_handle - 0x100] = o;
    return test_next_handle++;
}
static KObj* handle_object(u32 h) {
    CHECK(h >= 0x100 && h < test_next_handle);
    return test_handles[h - 0x100];
}
static void close_handles(void) {
    for (u32 h = 0x100; h < test_next_handle; h++) kobj_unref(test_handles[h - 0x100]);
    test_next_handle = 0x100;
}
u32 ev_handle(KEvent* e) { return h_new(&e->o); }
int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    *event = h_new(&e->o);
    return 0;
}
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
}

typedef struct {
    IpcCtx c;
    u8 raw[32];
} Request;
static IpcCtx* request(Request* r, u32 cmd) {
    memset(r, 0, sizeof *r);
    r->c.cmd = cmd;
    r->c.raw = r->raw;
    r->c.raw_size = sizeof r->raw;
    return &r->c;
}
static void argument32(Request* r, u32 off, u32 v) { memcpy(r->raw + off, &v, 4); }
static void argument64(Request* r, u32 off, u64 v) { memcpy(r->raw + off, &v, 8); }
static u32 reply32(Request* r, u32 off) {
    u32 v;
    CHECK(off + 4 <= r->c.out_size);
    memcpy(&v, r->c.out + off, 4);
    return v;
}
static void call(SvcObj* o, Request* r) { CHECK(o->dispatch(o, &r->c) == 0); }

/* mm:u: Initialize (4) {module, priority, event clear mode} gives a request id. SetAndWait (6) {id, min, max} sets
   its clock, which Get (7) {id} reads back: NvMM reads the decoder's clock back after asking for it */
static u32 mm_init(SvcObj* mm, u32 module) {
    Request r;
    request(&r, 4);
    argument32(&r, 0, module);
    argument32(&r, 4, 1);
    argument32(&r, 8, 1);
    call(mm, &r);
    CHECK(r.c.out_size == 4);
    return reply32(&r, 0);
}
static void mm_set_and_wait(SvcObj* mm, u32 cmd, u32 key, u32 min, u32 max) {
    Request r;
    request(&r, cmd);
    argument32(&r, 0, key);
    argument32(&r, 4, min);
    argument32(&r, 8, max);
    call(mm, &r);
    CHECK(r.c.out_size == 0);
}
static u32 mm_get_setting(SvcObj* mm, u32 cmd, u32 key) {
    Request r;
    request(&r, cmd);
    argument32(&r, 0, key);
    call(mm, &r);
    CHECK(r.c.out_size == 4);
    return reply32(&r, 0);
}
static void mm_finalize_key(SvcObj* mm, u32 cmd, u32 key) {
    Request r;
    request(&r, cmd);
    argument32(&r, 0, key);
    call(mm, &r);
    CHECK(r.c.out_size == 0);
}
static void test_mm(void) {
    SvcObj* mm = new_mm();
    u32 nvdec = mm_init(mm, 5), ram = mm_init(mm, 2);
    CHECK(nvdec && ram && nvdec != ram);
    CHECK(mm_get_setting(mm, 7, nvdec) == 0); /* nothing asked for yet */
    mm_set_and_wait(mm, 6, nvdec, 576000000, 716800000);
    mm_set_and_wait(mm, 6, ram, 1600000000, 1600000000);
    CHECK(mm_get_setting(mm, 7, nvdec) == 576000000); /* the minimum asked for */
    CHECK(mm_get_setting(mm, 7, ram) == 1600000000);
    mm_set_and_wait(mm, 6, nvdec, 268800000, 716800000);
    CHECK(mm_get_setting(mm, 7, nvdec) == 268800000 && mm_get_setting(mm, 7, ram) == 1600000000);
    mm_finalize_key(mm, 5, nvdec);
    CHECK(mm_get_setting(mm, 7, nvdec) == 0 && mm_get_setting(mm, 7, ram) == 1600000000);
    mm_set_and_wait(mm, 6, nvdec, 1, 2); /* a finalized request is gone */
    CHECK(mm_get_setting(mm, 7, nvdec) == 0);
    CHECK(mm_init(mm, 5) != nvdec); /* each Initialize is a new request */
    /* the old forms (0 to 3) name the module instead of an id */
    Request r;
    request(&r, 0);
    argument32(&r, 0, 8);
    argument32(&r, 4, 1);
    call(mm, &r);
    CHECK(r.c.out_size == 0);
    mm_set_and_wait(mm, 2, 8, 300000000, 400000000);
    CHECK(mm_get_setting(mm, 3, 8) == 300000000);
    mm_finalize_key(mm, 1, 8);
    CHECK(mm_get_setting(mm, 3, 8) == 0);
    svcobj_unref(mm);
}

/* hid IHidServer: SetSupportedNpadStyleSet (100) {u32 style set, aruid at 8}, GetSupportedNpadStyleSet (101) {aruid}
   and AcquireNpadStyleSetUpdateEventHandle (106) {u32 npad id, aruid at 8, u64 event pointer at 16} */
static u32 no1_style(void) {
    u32 style;
    memcpy(&style, kshmem_ptr(hid.shm) + HO_HidSharedMemory_npad + HO_HidNpadInternalState_style_set, 4);
    return style;
}
static u32 get_styles(SvcObj* h) {
    Request r;
    request(&r, 101);
    r.c.has_pid = 1; /* the aruid */
    argument64(&r, 0, 0x55);
    call(h, &r);
    CHECK(r.c.out_size == 4);
    return reply32(&r, 0);
}
static void set_styles(SvcObj* h, u32 styles) {
    Request r;
    request(&r, 100);
    r.c.has_pid = 1; /* the aruid */
    argument32(&r, 0, styles);
    argument64(&r, 8, 0x55);
    call(h, &r);
    CHECK(r.c.out_size == 0);
}
static KEvent* style_event(SvcObj* h, u32 id) {
    Request r;
    request(&r, 106);
    r.c.has_pid = 1; /* the aruid */
    argument32(&r, 0, id);
    argument64(&r, 8, 0x55);
    argument64(&r, 16, 0x1234560);
    call(h, &r);
    CHECK(r.c.out_size == 0 && r.c.nout_copy == 1 && !r.c.nout_move && !r.c.nout_objs);
    KObj* o = handle_object(r.c.out_copy[0]);
    CHECK(o->type == KO_EVENT);
    return (KEvent*)o;
}
static void test_hid(void) {
    svc_hid_init();
    SvcObj* h = new_hid();
    CHECK(get_styles(h) == 0x7F); /* every style until the game says which it takes */
    CHECK(no1_style() == 1);      /* No1 is a Pro Controller */
    KEvent* no1 = style_event(h, 0);
    CHECK(no1 == hid.style_ev[0] && no1->signaled); /* signaled at first, so the game reads the styles */
    CHECK(style_event(h, 0x20) == hid.style_ev[8] && style_event(h, 0x10) == hid.style_ev[9]);
    CHECK(style_event(h, 3) == hid.style_ev[3]);
    no1->signaled = 0;
    set_styles(h, (1u << 1) | (1u << 2)); /* handheld and dual Joy-Con */
    CHECK(get_styles(h) == 6);
    CHECK(no1_style() == 4 && no1->signaled); /* No1 changes to one style, dual Joy-Con, and says so */
    no1->signaled = 0;
    set_styles(h, 1u << 1); /* handheld only: No1 has no style it can take */
    CHECK(get_styles(h) == 2 && no1_style() == 0 && no1->signaled);
    set_styles(h, 0x7F);
    CHECK(get_styles(h) == 0x7F && no1_style() == 1);
    svcobj_unref(h);
    close_handles();
    /* the service's own references: everything it made goes with them */
    kobj_unref(&hid.shm->o);
    kobj_unref(&hid.xpad_shm->o);
    kobj_unref(&hid.six_shm->o);
    kobj_unref(&hid.joysix_shm->o);
    for (int i = 0; i < 10; i++) kobj_unref(&hid.style_ev[i]->o);
}

int main(void) {
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    test_mm();
    test_hid();
    CHECK(test_objects == 0 && test_kobjs == 0);
    free(g_mem);
    printf("misc_service: %u checks passed\n", test_checks);
    return 0;
}
