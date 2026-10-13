/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bsd:u's IPC commands, sent as the SDK and libnx send them (FreeBSD's constants and sockaddr layout), over the
   host's loopback, and the network option's offline mode */
#include "hle/services/services_int.h"
#include <stdarg.h>

#include "hle/services/network/bsd.c"

u8* g_mem;
int g_verbose;
int g_opt_network;
static unsigned failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "bsd_service.c:%d: check failed: %s\n", __LINE__, #x);                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t size) {
    SvcObj* o = calloc(1, sizeof *o);
    o->name = name;
    o->dispatch = dispatch;
    o->refs = 1;
    if (size) o->data = calloc(1, size);
    return o;
}
void svcobj_unref(SvcObj* o) {
    if (!o || --o->refs) return;
    if (o->destroy) o->destroy(o);
    free(o->data);
    free(o);
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    failures++;
}
int ipc_warn_once(const char* name, u32 cmd) {
    (void)name, (void)cmd;
    return 0;
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}
int buf_in(IpcCtx* c, int i, u64* a, u64* s) {
    *a = *s = 0;
    if (i >= c->na) return 0;
    *a = c->a[i].addr;
    *s = c->a[i].size;
    return 1;
}
int buf_out(IpcCtx* c, int i, u64* a, u64* s) {
    *a = *s = 0;
    if (i >= c->nb) return 0;
    *a = c->b[i].addr;
    *s = c->b[i].size;
    return 1;
}

/* guest memory: buffers at fixed addresses */
enum { IN0 = 0x1000, IN1 = 0x2000, OUT0 = 0x3000, OUT1 = 0x4000, BUFSIZE = 0x1000 };
static SvcObj* bsd;
static IpcCtx ctx;
static u32 raw[8];
static void request(u32 cmd, u32 a0, u32 a1, u32 a2) {
    memset(&ctx, 0, sizeof ctx);
    memset(raw, 0, sizeof raw);
    raw[0] = a0;
    raw[1] = a1;
    raw[2] = a2;
    ctx.raw = (const u8*)raw;
    ctx.raw_size = sizeof raw;
    ctx.cmd = cmd;
}
static void in(u64 addr, u64 size) {
    ctx.a[ctx.na].addr = addr;
    ctx.a[ctx.na++].size = size;
}
static void out(u64 addr, u64 size) {
    ctx.b[ctx.nb].addr = addr;
    ctx.b[ctx.nb++].size = size;
}
/* sends the request; *err gets the errno, the call's result is returned */
static i32 call(u32* err) {
    CHECK(bsd->dispatch(bsd, &ctx) == 0);
    u32 expected = ctx.cmd == 0                                                                         ? 4
                   : (ctx.cmd == 9 || ctx.cmd == 12 || ctx.cmd == 15 || ctx.cmd == 16 || ctx.cmd == 17) ? 12
                                                                                                        : 8;
    CHECK(ctx.out_size == expected);
    i32 r;
    memcpy(&r, ctx.out, 4);
    if (err) memcpy(err, ctx.out + 4, 4);
    return r;
}
static u32 word(int i) {
    u32 v;
    memcpy(&v, ctx.out + i * 4, 4);
    return v;
}
/* FreeBSD's sockaddr_in for 127.0.0.1 */
static void sockaddr_lo(u64 at, u16 port_be) {
    u8 s[16] = {16, 2, 0, 0, 127, 0, 0, 1};
    memcpy(s + 2, &port_be, 2);
    memcpy(GP(at), s, 16);
}

int main(void) {
    g_mem = calloc(1, 0x10000);
    network_init();
    snx_net_set_log(NULL);
    bsd = new_bsd_u();
    u32 e;

    request(0, 0, 0, 0); /* RegisterClient */
    call(NULL);
    request(2, 2, 1, 0); /* Socket(AF_INET, SOCK_STREAM) */
    i32 l = call(&e);
    CHECK(l >= 0 && !e);
    u32 one = 1;
    memcpy(GP(IN0), &one, 4);
    request(21, (u32)l, 0xFFFF, 0x4); /* SetSockOpt SO_REUSEADDR */
    in(IN0, 4);
    CHECK(call(&e) == 0);
    request(21, (u32)l, 0xFFFF, 0x10000); /* Horizon's SO_NN_SHUTDOWN_EXEMPT: accepted */
    in(IN0, 4);
    CHECK(call(&e) == 0);
    sockaddr_lo(IN0, 0);
    request(13, (u32)l, 0, 0); /* Bind */
    in(IN0, 16);
    CHECK(call(&e) == 0);
    request(18, (u32)l, 4, 0); /* Listen */
    CHECK(call(&e) == 0);
    request(16, (u32)l, 0, 0); /* GetSockName */
    out(OUT0, 16);
    CHECK(call(&e) == 0 && word(2) == 16);
    u8* name = GP(OUT0);
    u16 port;
    memcpy(&port, name + 2, 2);
    CHECK(name[0] == 16 && name[1] == 2 && port && name[4] == 127);

    request(2, 2, 1 | 0x20000000, 0); /* a SOCK_NONBLOCK client */
    i32 c = call(&e);
    CHECK(c > l);
    request(20, (u32)c, 3, 0); /* Fcntl F_GETFL */
    CHECK((call(&e) & 0x800) == 0x800);
    sockaddr_lo(IN0, port);
    request(14, (u32)c, 0, 0); /* Connect */
    in(IN0, 16);
    i32 r = call(&e);
    CHECK(r == 0 || (r == -1 && e == SNX_EINPROGRESS));
    request(12, (u32)l, 0, 0); /* Accept */
    out(OUT0, 16);
    i32 s = call(&e);
    CHECK(s > c && word(2) == 16);

    /* Poll: the client writable, then readable */
    u8 pfd[8] = {0};
    memcpy(pfd, &c, 4);
    pfd[4] = 0x04; /* POLLOUT */
    memcpy(GP(IN0), pfd, 8);
    request(6, 1, 2000, 0);
    in(IN0, 8);
    out(OUT0, 8);
    CHECK(call(&e) == 1 && (GP(OUT0)[6] & 0x04));
    memcpy(GP(IN1), "switch", 6);
    request(10, (u32)s, 0, 0); /* Send */
    in(IN1, 6);
    CHECK(call(&e) == 6);
    pfd[4] = 0x01;
    memcpy(GP(IN0), pfd, 8);
    request(6, 1, 2000, 0);
    in(IN0, 8);
    out(OUT0, 8);
    CHECK(call(&e) == 1 && (GP(OUT0)[6] & 0x01));
    request(19, (u32)c, BSD_FIONREAD, 1); /* Ioctl FIONREAD */
    out(OUT1, 4);
    CHECK(call(&e) == 0 && gr32(OUT1) == 6);
    request(8, (u32)c, 0x2, 0); /* Recv MSG_PEEK (FreeBSD's) */
    out(OUT0, 3);
    CHECK(call(&e) == 3);
    request(25, (u32)c, 0, 0); /* Read */
    out(OUT0, BUFSIZE);
    CHECK(call(&e) == 6 && !memcmp(GP(OUT0), "switch", 6));
    request(8, (u32)c, 0x80, 0); /* Recv MSG_DONTWAIT: nothing left */
    out(OUT0, BUFSIZE);
    CHECK(call(&e) == -1 && e == SNX_EAGAIN);

    /* Select: s isn't readable, c is writable */
    u8 sets[3][16];
    memset(sets, 0, sizeof sets);
    sets[0][s / 8] |= (u8)(1 << (s % 8));
    sets[1][c / 8] |= (u8)(1 << (c % 8));
    memcpy(GP(IN0), sets[0], 16);
    memcpy(GP(IN1), sets[1], 16);
    memset(raw, 0, sizeof raw);
    request(5, (u32)(s > c ? s : c) + 1, 0, 0);
    raw[6] = 0; /* timeout {0, 0}, not null */
    in(IN0, 16);
    in(IN1, 16);
    in(0, 0);
    out(OUT0, 16);
    out(OUT1, 16);
    out(0, 0);
    CHECK(call(&e) == 1 && !(GP(OUT0)[s / 8] & (1 << (s % 8))) && (GP(OUT1)[c / 8] & (1 << (c % 8))));

    /* options */
    request(17, (u32)s, 0xFFFF, 0x1008); /* GetSockOpt SO_TYPE */
    out(OUT0, 4);
    CHECK(call(&e) == 0 && gr32(OUT0) == 1 && word(2) == 4);
    request(17, (u32)s, 0xFFFF, 0x1007); /* SO_ERROR */
    out(OUT0, 4);
    CHECK(call(&e) == 0 && gr32(OUT0) == 0);
    u64 tv[2] = {0, 250000};
    memcpy(GP(IN0), tv, 16);
    request(21, (u32)s, 0xFFFF, 0x1006); /* SO_RCVTIMEO */
    in(IN0, 16);
    CHECK(call(&e) == 0);
    request(8, (u32)s, 0, 0); /* a blocking Recv gives up after it */
    out(OUT0, BUFSIZE);
    CHECK(call(&e) == -1 && e == SNX_EAGAIN);

    /* UDP and RecvFrom's address */
    request(2, 2, 2, 17);
    i32 u1 = call(&e);
    request(2, 2, 2, 17);
    i32 u2 = call(&e);
    sockaddr_lo(IN0, 0);
    request(13, (u32)u1, 0, 0);
    in(IN0, 16);
    CHECK(call(&e) == 0);
    request(16, (u32)u1, 0, 0);
    out(OUT1, 16); /* keep the destination separate from the payload */
    CHECK(call(&e) == 0);
    memcpy(GP(IN1), "udp!", 4);
    request(11, (u32)u2, 0, 0); /* SendTo */
    in(IN1, 4);
    in(OUT1, 16);
    CHECK(call(&e) == 4);
    request(9, (u32)u1, 0, 0); /* RecvFrom */
    out(OUT0, 64);
    out(IN0, 16);
    CHECK(call(&e) == 4 && word(2) == 16 && GP(IN0)[1] == 2 && GP(IN0)[4] == 127);

    /* Zero-size RecvFrom must not expose an uninitialized sender or consume the datagram. */
    request(11, (u32)u2, 0, 0);
    in(IN1, 4);
    in(OUT1, 16);
    CHECK(call(&e) == 4);
    memset(GP(IN0), 0xA5, 16);
    request(9, (u32)u1, 0, 0);
    out(OUT0, 0);
    out(IN0, 16);
    CHECK(call(&e) == 0 && word(2) == 0 && GP(IN0)[0] == 0xA5);
    request(9, (u32)u1, 0x80, 0);
    out(OUT0, 64);
    out(IN0, 16);
    CHECK(call(&e) == 4 && word(2) == 16);

    /* Every error response has exactly the command's ABI size, including the address length. */
    const u32 bad_fd_commands[] = {8, 9, 10, 11, 12, 15, 16, 17, 24, 25};
    for (unsigned i = 0; i < sizeof bad_fd_commands / sizeof *bad_fd_commands; i++) {
        request(bad_fd_commands[i], BSD_MAX_FD, 0, 0);
        in(IN1, 4);
        out(OUT0, 64);
        out(IN0, 16);
        CHECK(call(&e) == -1 && e == SNX_EBADF);
        if (ctx.out_size == 12) CHECK(word(2) == 0);
    }

    /* EventFd, Close, bad descriptors, offline */
    raw[0] = 0;
    request(31, 2, 0, 0x4); /* EventFd(2, EFD_NONBLOCK) */
    raw[2] = 0x4;
    i32 ev = call(&e);
    CHECK(ev > 0);
    request(25, (u32)ev, 0, 0);
    out(OUT0, 8);
    CHECK(call(&e) == 8 && gr32(OUT0) == 2);
    request(26, (u32)c, 0, 0);
    CHECK(call(&e) == 0);
    request(26, (u32)c, 0, 0);
    CHECK(call(&e) == -1 && e == SNX_EBADF);
    request(2, 2, 3, 6); /* bsd:u has no raw TCP sockets */
    CHECK(call(&e) == -1 && e == SNX_EINVAL);
    u8 google[16] = {16, 2, 0, 53, 8, 8, 8, 8};
    memcpy(GP(IN0), google, 16);
    request(14, (u32)u2, 0, 0); /* offline: only loopback is reachable */
    in(IN0, 16);
    CHECK(call(&e) == -1 && e == SNX_ENETUNREACH);
    CHECK(!network_online());

    svcobj_unref(bsd);
    free(g_mem);
    if (failures) {
        fprintf(stderr, "bsd_service: %u checks failed\n", failures);
        return 1;
    }
    puts("bsd_service: ok");
    return 0;
}
