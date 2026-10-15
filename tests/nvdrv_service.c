/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvdrv's IPC side (nvdrv.c) through its generated dispatcher: Ioctl2's and Ioctl3's inline buffers reaching the
   device, the NvError of a bad fd, QueryEvent, and a Close that comes while an ioctl is running on the fd. The GPU and
   host1x are stand-ins that record what nvdrv asks of them */
#include "hle/services/nvdrv.h"
#include "hle/kernel/ipc.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "hle/services/nvdrv.c"

u8* g_mem;
int g_verbose;
void (*g_gpu_mapping_begin)(u64, u64, int);
void (*g_gpu_mapping_end)(void);
static unsigned test_checks, test_kobjs;
static u32 test_next_handle = 0x100;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "nvdrv_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

/* single-threaded: the locks only have to be balanced, which the counter checks */
static int test_locks;
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) {
    (void)m;
    test_locks++;
}
void pm_unlock(PMutex* m) {
    (void)m;
    CHECK(test_locks > 0);
    test_locks--;
}
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    (void)c, (void)m, (void)timeout_ns;
    test_fail("nothing here waits for a syncpoint", __LINE__);
    return 1;
}
u64 plat_ns(void) { return 0; }
u64 guest_ns(void) { return 0; }
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
    if (size) obj->data = calloc(1, size);
    return obj;
}
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    free(obj->data);
    free(obj);
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
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}

/* kernel objects, counted; a handle takes a reference and names its object */
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    CHECK(o->refs > 0);
    if (--o->refs) return;
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
void kevent_clear(KEvent* e) { e->signaled = 0; }
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

/* the GPU: submissions and retired channels, recorded */
static struct {
    u64 entries[8];
    u32 n, channel, incs, expect, submits, closed_channel, calls;
} test_gpu;
void gpu_submit(u64* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect) {
    CHECK(n <= 8 && wait_id == ~0u);
    memcpy(test_gpu.entries, entries, n * sizeof *entries);
    test_gpu.n = n;
    test_gpu.channel = channel;
    test_gpu.incs = incs;
    test_gpu.expect = expect;
    test_gpu.submits++;
}
void gpu_call(void (*fn)(void*), void* arg) {
    test_gpu.calls++;
    fn(arg); /* the GPU thread has nothing queued before it */
}
void gpu_channel_close(u32 channel) { test_gpu.closed_channel = channel; }
void gpu_mem_written(u64 gva, u64 size) { (void)gva, (void)size; }

/* host1x: one channel per NVDEC or VIC fd. Its ioctl can run a hook in the middle, as another guest thread would */
typedef struct {
    int is_vic, closed;
    u32 ioctls;
} TestChannel;
static TestChannel test_channels[4];
static unsigned test_nchannels;
static void (*test_during_ioctl)(void);
void* host1x_open(int is_vic) {
    CHECK(test_nchannels < 4);
    TestChannel* ch = &test_channels[test_nchannels++];
    ch->is_vic = is_vic;
    return ch;
}
void host1x_close(void* ch) {
    if (!ch) return;
    TestChannel* t = ch;
    CHECK(!t->closed);
    t->closed = 1;
}
u32 host1x_ioctl(void* ch, u32 cmd, u8* d, u32 n) {
    TestChannel* t = ch;
    CHECK(!t->closed);
    t->ioctls++;
    if (test_during_ioctl) {
        void (*hook)(void) = test_during_ioctl;
        test_during_ioctl = NULL;
        hook();
    }
    CHECK(!t->closed); /* the channel outlives a Close made during its ioctl */
    if (n >= 4) memcpy(d, &cmd, 4);
    return 0;
}
void host1x_forget_handle(u32 h) { (void)h; }

/* guest memory: the request's buffers */
enum { IN0 = 0x1000, IN1 = 0x2000, OUT0 = 0x3000, OUT1 = 0x4000, PATH = 0x5000 };
static SvcObj* nvdrv;
static IpcCtx ctx;
static u32 raw[4];
/* the descriptors each command declares, empty: an AutoSelect buffer goes in both its lists (A and X for an input,
   B and C for an output), and the test fills in the ones it uses */
static void request(u32 cmd, u32 a0, u32 a1) {
    memset(&ctx, 0, sizeof ctx);
    raw[0] = a0;
    raw[1] = a1;
    ctx.cmd = cmd;
    ctx.raw = (const u8*)raw;
    ctx.raw_size = sizeof raw;
    ctx.na = cmd == 0 || cmd == 1 || cmd == 12 ? 1 : cmd == 11 ? 2 : 0; /* Open's path, the ioctls' ins */
    ctx.nx = cmd == 1 || cmd == 12 ? 1 : cmd == 11 ? 2 : 0;
    ctx.nb = ctx.nc = cmd == 1 || cmd == 11 ? 1 : cmd == 12 ? 2 : 0; /* the ioctls' outs */
}
static IpcBuf buf(u64 addr, u64 size) {
    IpcBuf b = {addr, size, 0};
    return b;
}
static u32 reply(u32 off) {
    u32 v;
    CHECK(off + 4 <= ctx.out_size);
    memcpy(&v, ctx.out + off, 4);
    return v;
}
/* Open (0): the path in A, the fd and the NvError back */
static u32 nv_open(const char* path, u32* error) {
    strcpy((char*)GP(PATH), path);
    request(0, 0, 0);
    ctx.a[0] = buf(PATH, strlen(path) + 1);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 8);
    *error = reply(4);
    return reply(0);
}
static u32 nv_close(u32 fd) {
    request(2, fd, 0);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4);
    return reply(0);
}
/* Ioctl (1) with its in and out buffers mapped */
static u32 nv_ioctl1(u32 fd, u32 cmd, u64 size) {
    request(1, fd, cmd);
    ctx.a[0] = buf(IN0, size);
    ctx.b[0] = buf(OUT0, size);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4);
    return reply(0);
}

/* Ioctl2 (11): SUBMIT_GPFIFO_EX's header in A0 or X0, the GPFIFO entries inline in A1 or X1 */
#define SUBMIT_GPFIFO_EX 0xC018481Bu
static void submit_header(u32 num, u32 flags) {
    u8 h[0x18];
    memset(h, 0, sizeof h);
    memcpy(h + 8, &num, 4);
    memcpy(h + 12, &flags, 4);
    memcpy(GP(IN0), h, sizeof h);
}
static void test_ioctl2(u32 gpu) {
    const u64 entries[3] = {0x0000000100200000ull, 0x0000000100300000ull, 0x0000000400400000ull};
    memcpy(GP(IN1), entries, sizeof entries);
    submit_header(3, 2); /* FenceIncrement: the host adds two increments after the entries */
    memset(GP(OUT0), 0xCC, 0x18);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.a[0] = buf(IN0, 0x18);
    ctx.a[1] = buf(IN1, sizeof entries);
    ctx.b[0] = buf(OUT0, 0x18);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    CHECK(test_gpu.submits == 1 && test_gpu.n == 3 && !memcmp(test_gpu.entries, entries, sizeof entries));
    u32 sp = test_gpu.channel;
    CHECK(sp >= 20 && sp < SYNCPT_N && test_gpu.incs == 2 && test_gpu.expect == 2);
    /* the fence the submission promises comes back in the output */
    CHECK(gr32(OUT0 + 16) == sp && gr32(OUT0 + 20) == 2);

    /* the same through the pointer buffer: the header in X0, the entries in X1, the A list empty */
    submit_header(2, 2);
    memset(GP(OUT0), 0xCC, 0x18);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.x[0] = buf(IN0, 0x18);
    ctx.x[1] = buf(IN1 + 8, 16);
    ctx.c[0] = buf(OUT0, 0x18);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0);
    CHECK(test_gpu.submits == 2 && test_gpu.n == 2 && !memcmp(test_gpu.entries, entries + 1, 16));
    CHECK(test_gpu.channel == sp && test_gpu.expect == 4 && gr32(OUT0 + 20) == 4);

    /* mapped header, inline entries through X1: an AutoSelect buffer takes index 1 in both lists */
    submit_header(1, 0);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.a[0] = buf(IN0, 0x18);
    ctx.x[1] = buf(IN1 + 16, 8);
    ctx.b[0] = buf(OUT0, 0x18);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0);
    CHECK(test_gpu.submits == 3 && test_gpu.n == 1 && test_gpu.entries[0] == entries[2] && test_gpu.incs == 0);

    /* the entries count is cut to what the inline buffer holds */
    submit_header(3, 0);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.a[0] = buf(IN0, 0x18);
    ctx.a[1] = buf(IN1, 8);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0 && test_gpu.n == 1);
}

/* Ioctl3 (12): ctrl-gpu's GET_CHARACTERISTICS writes the characteristics to the inline output, B1 or C1 */
#define GET_CHARACTERISTICS 0xC0B04705u
static void test_ioctl3(u32 ctrlgpu) {
    u64 header[2] = {0xA0, 0x7000};
    memcpy(GP(IN0), header, sizeof header);
    memset(GP(OUT1), 0xCC, 0xB0);
    request(12, ctrlgpu, GET_CHARACTERISTICS);
    ctx.a[0] = buf(IN0, 0xB0);
    ctx.b[0] = buf(OUT0, 0xB0);
    ctx.b[1] = buf(OUT1, 0xA0);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    CHECK(gr32(OUT1) == 0x120 && gr32(OUT1 + 4) == 0xB && gr32(OUT1 + 8) == 0xA1); /* GM20B, arch 0x120 */
    CHECK(GP(OUT1)[0xA0] == 0xCC);                                                 /* nothing past its size */
    CHECK(gr64(OUT0) == 0xA0 && gr64(OUT0 + 8) == 0x7000 && gr32(OUT0 + 16) == 0x120);
    /* through C1, B empty */
    memset(GP(OUT1), 0xCC, 0xB0);
    request(12, ctrlgpu, GET_CHARACTERISTICS);
    ctx.x[0] = buf(IN0, 0xB0);
    ctx.c[0] = buf(OUT0, 0xB0);
    ctx.c[1] = buf(OUT1, 0x10);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0);
    CHECK(gr32(OUT1) == 0x120 && GP(OUT1)[0x10] == 0xCC);
    /* Ioctl2's inline buffer is an input: ctrl-gpu writes nothing to it */
    memset(GP(IN1), 0xCC, 0xA0);
    request(11, ctrlgpu, GET_CHARACTERISTICS);
    ctx.a[0] = buf(IN0, 0xB0);
    ctx.a[1] = buf(IN1, 0xA0);
    ctx.b[0] = buf(OUT0, 0xB0);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0 && GP(IN1)[0] == 0xCC);
}

/* QueryEvent (4) {fd, event id}: the NvError, then the event as a copy handle */
static KEvent* query_event(u32 fd, u32 id) {
    request(4, fd, id);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    CHECK(ctx.nout_copy == 1 && !ctx.nout_move);
    KObj* o = handle_object(ctx.out_copy[0]);
    CHECK(o->type == KO_EVENT);
    return (KEvent*)o;
}

/* a Close from another thread while an ioctl runs on the fd: the fd stays (closed) until the ioctl leaves, another
   ioctl on it answers NvError 8 (InvalidState), and Open doesn't hand its number out meanwhile */
static u32 race_fd, race_other_fd;
static void close_during_ioctl(void) {
    IpcCtx running = ctx; /* the ioctl's request, which the dispatcher answers into when this returns */
    CHECK(nv_close(race_fd) == 0);
    CHECK(nv.nvf[race_fd].dev == DEV_NVDEC && nv.nvf[race_fd].closed && nv.nvf[race_fd].users == 1);
    CHECK(!test_channels[0].closed);
    CHECK(nv_ioctl1(race_fd, 0xC0044801u, 4) == 8);
    CHECK(nv_close(race_fd) == 8); /* a second Close finds the fd closed, and changes nothing */
    CHECK(nv.nvf[race_fd].closed && !test_channels[0].closed);
    u32 error;
    race_other_fd = nv_open("/dev/nvhost-vic", &error);
    CHECK(!error && race_other_fd != race_fd);
    ctx = running;
}
static void test_close_during_ioctl(void) {
    u32 error;
    race_fd = nv_open("/dev/nvhost-nvdec", &error);
    CHECK(!error && race_fd > 0 && race_fd < 256 && test_nchannels == 1);
    CHECK(nv_ioctl1(race_fd, 0xC0044801u, 4) == 0 && gr32(OUT0) == 0xC0044801u && test_channels[0].ioctls == 1);
    test_during_ioctl = close_during_ioctl;
    CHECK(nv_ioctl1(race_fd, 0xC0044802u, 4) == 0);
    CHECK(test_channels[0].ioctls == 2 && test_channels[0].closed); /* the last ioctl out released it */
    CHECK(!nv.nvf[race_fd].dev && !nv.nvf[race_fd].closed && !nv.nvf[race_fd].users);
    CHECK(nv_ioctl1(race_fd, 0xC0044801u, 4) == 8);
    u32 again = nv_open("/dev/nvhost-nvdec", &error);
    CHECK(!error && again == race_fd); /* free now: the lowest fd again */
    CHECK(nv_close(again) == 0 && nv_close(race_other_fd) == 0);
    CHECK(test_channels[1].closed && test_channels[2].closed);
}

int main(void) {
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    nv_init();
    nvdrv = new_nvdrv();
    u32 error;

    /* bad fds: never opened, past the table, and the null fd 0 */
    CHECK(nv_ioctl1(7, 0xC0044801u, 4) == 8);
    CHECK(nv_ioctl1(0xFFFF, 0xC0044801u, 4) == 8);
    CHECK(nv_ioctl1(0, 0xC0044801u, 4) == 8);
    request(11, 9, SUBMIT_GPFIFO_EX);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 8);
    request(12, 9, GET_CHARACTERISTICS);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 8);

    u32 gpu = nv_open("/dev/nvhost-gpu", &error);
    CHECK(!error && gpu == 1);
    u32 ctrlgpu = nv_open("/dev/nvhost-ctrl-gpu", &error);
    CHECK(!error && ctrlgpu == 2);
    u32 ctrl = nv_open("/dev/nvhost-ctrl", &error);
    CHECK(!error && ctrl == 3);
    test_ioctl2(gpu);
    test_ioctl3(ctrlgpu);

    /* a channel's own events (1 to 3), made on first query and kept by the fd; the ctrl fd's ids name event slots */
    KEvent* sm = query_event(gpu, 1);
    CHECK(query_event(gpu, 1) == sm && query_event(gpu, 3) != sm);
    KEvent* slot = query_event(ctrl, 0x10000005u);
    CHECK(slot == nv.nvev[5].ev && query_event(ctrl, 5) == slot);

    /* closing the GPU channel retires its syncpoint once the GPU thread has run what was queued on it */
    u32 sp = test_gpu.channel;
    CHECK(nv_close(gpu) == 0 && test_gpu.closed_channel == sp && !nv.syncpt_used[sp] && !nv.nvf[gpu].dev);
    CHECK(sm->o.refs == 2); /* the game's two handles keep it, the fd's reference is gone */
    CHECK(nv_ioctl1(gpu, SUBMIT_GPFIFO_EX, 0x18) == 8);
    CHECK(nv_close(gpu) == 8 && nv_close(0) == 8 && nv_close(0xFFFF) == 8); /* not open: as an ioctl to it */
    test_close_during_ioctl();

    CHECK(nv_close(ctrlgpu) == 0 && nv_close(ctrl) == 0);
    for (u32 h = 0x100; h < test_next_handle; h++) kobj_unref(test_handles[h - 0x100]);
    for (int i = 0; i < 64; i++)
        if (nv.nvev[i].ev) kobj_unref(&nv.nvev[i].ev->o);
    CHECK(test_kobjs == 0 && test_locks == 0);
    svcobj_unref(nvdrv);
    free(nv.gmmu);
    free(g_mem);
    printf("nvdrv_service: %u checks passed\n", test_checks);
    return 0;
}
