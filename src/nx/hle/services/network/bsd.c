/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden's src/core/hle/service/sockets/bsd.cpp (branch lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
     SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project, GPL-2.0-or-later
   bsd:u and bsd:s, Horizon's BSD sockets, on the emulated network stack (src/common/network). Requests are served on
   the guest thread that sends them, without its core (ipc_handle), so a blocking call blocks that thread only, as
   on the console where each of the SDK's sessions has a server thread. Every reply is the call's result and its
   errno (Linux's numbering, the stack's too); the constants are FreeBSD's, translated with the tables in abi/.

   Eden stubs Read, Select and EventFd, answers GetSockOpt for SO_ERROR only, and shuts down the read side of a
   SocketExempt socket; those are real here. */
#include "hle/services/network/network.h"
#include <time.h>

#define BSD_MAX_FD 128
enum {
    BSD_F_GETFD = 1,
    BSD_F_SETFD = 2,
    BSD_F_GETFL = 3,
    BSD_F_SETFL = 4,
    BSD_O_NONBLOCK = 0x4,      /* FreeBSD's */
    BSD_O_NONBLOCK_NX = 0x800, /* what the SDK and libnx send (Eden's NONBLOCK_NX) */
    BSD_SOCK_NONBLOCK = 0x20000000,
    BSD_SOCK_CLOEXEC = 0x10000000,
    BSD_EFD_SEMAPHORE = 0x1,
    BSD_EFD_NONBLOCK = 0x4,
    BSD_FIONBIO = 0x8004667E,
    BSD_FIONREAD = 0x4004667F,
    BSD_SOCKADDR_IN = 16,
    BSD_SOCKADDR_IN6 = 28,
};

typedef struct BsdFd {
    SnxSocket* s;
    int flags;  /* F_SETFL's as the guest sent them */
    int exempt; /* SocketExempt: ShutdownAllSockets leaves it alone */
} BsdFd;
/* one per session (and its clones): Horizon's descriptors belong to the client that registered */
typedef struct Bsd {
    PMutex lk;
    BsdFd fd[BSD_MAX_FD];
    int system; /* bsd:s, which may open raw sockets and duplicate descriptors */
} Bsd;

/* ============================================================ the network option */
/* nifm asks often (some games every frame): the host's state is read again after two seconds */
int network_interface(SnxNetInterface* out) {
    static PMutex lk = PMUTEX_INIT;
    static time_t when;
    static int have;
    static SnxNetInterface cached;
    if (!g_opt_network) return -SNX_ENETDOWN;
    pm_lock(&lk);
    time_t now = time(NULL);
    if (!when || now - when >= 2 || now < when) {
        have = !snx_net_primary_interface(&cached);
        when = now;
    }
    if (have && out) *out = cached;
    int r = have ? 0 : -SNX_ENETDOWN;
    pm_unlock(&lk);
    return r;
}
int network_online(void) { return !network_interface(NULL); }
int network_reachable(const SnxSockAddr* a) {
    if (g_opt_network || a->family == SNX_AF_UNSPEC) return 1;
    if (a->family == SNX_AF_INET) return a->addr[0] == 127;
    static const uint8_t lo6[16] = {[15] = 1};
    return a->family == SNX_AF_INET6 && !memcmp(a->addr, lo6, 16);
}
static void net_log(const char* m) { LOG("%s", m); }
void network_init(void) {
    snx_net_set_log(net_log);
    if (snx_net_init()) WARN("bsd: the host's network stack didn't start, sockets will fail\n");
}

/* ============================================================ translations (abi/) */
static int domain_in(u32 d) {
#define BSD_DOMAIN(guest, name)                                                                                        \
    if (d == guest) return SNX_AF_##name;
#include "hle/services/network/abi/bsd_domain.inc"
    return -1;
}
static u32 domain_out(int d) {
#define BSD_DOMAIN(guest, name)                                                                                        \
    if (d == SNX_AF_##name) return guest;
#include "hle/services/network/abi/bsd_domain.inc"
    return 0;
}
static int msg_in(u32 f) {
    int r = 0;
#define BSD_MSG(guest, name)                                                                                           \
    if (f & guest) r |= SNX_MSG_##name;
#include "hle/services/network/abi/bsd_msgflag.inc"
    return r;
}
static uint16_t poll_in(u16 e) {
    uint16_t r = 0;
#define BSD_POLL(guest, name)                                                                                          \
    if (e & guest) r |= SNX_POLL##name;
#include "hle/services/network/abi/bsd_poll.inc"
    return r;
}
static u16 poll_out(uint16_t e) {
    u16 r = 0;
#define BSD_POLL(guest, name)                                                                                          \
    if (e & SNX_POLL##name) r |= guest;
#include "hle/services/network/abi/bsd_poll.inc"
    if (e & SNX_POLLWRNORM) r |= 0x0004;
    return r;
}
/* 1 and the stack's level and name, 0 for an option to accept and ignore, -1 for one Horizon doesn't have */
static int sockopt_in(u32 level, u32 name, int* l, int* n) {
    int sl = -1;
#define BSD_LEVEL(guest, nm)                                                                                           \
    if (level == guest) sl = SNX_SOL_##nm;
#include "hle/services/network/abi/bsd_sockopt.inc"
    if (sl < 0) return -1;
    *l = sl;
#define BSD_SOCKOPT(lv, guest, nm)                                                                                     \
    if (sl == SNX_SOL_##lv && name == guest) {                                                                         \
        *n = SNX_##nm;                                                                                                 \
        return 1;                                                                                                      \
    }
#define BSD_SOCKOPT_IGNORED(lv, guest, nm)                                                                             \
    if (sl == SNX_SOL_##lv && name == guest) return 0;
#include "hle/services/network/abi/bsd_sockopt.inc"
    return -1;
}

/* guest sockaddr_in {u8 len, u8 family, be16 port, in_addr, zero[8]}, sockaddr_in6 {len, family, port, u32
   flowinfo, in6_addr, u32 scope} */
static int addr_in(u64 a, u64 n, SnxSockAddr* out) {
    memset(out, 0, sizeof *out);
    if (!a || n < 8) return -SNX_EINVAL;
    const u8* p = GP(a);
    int fam = p[1] ? domain_in(p[1]) : SNX_AF_INET;
    if (fam == SNX_AF_INET) {
        out->family = SNX_AF_INET;
        memcpy(&out->port, p + 2, 2);
        memcpy(out->addr, p + 4, 4);
        return 0;
    }
    if (fam == SNX_AF_INET6 && n >= 24) {
        out->family = SNX_AF_INET6;
        memcpy(&out->port, p + 2, 2);
        memcpy(&out->flowinfo, p + 4, 4);
        memcpy(out->addr, p + 8, 16);
        if (n >= 28) memcpy(&out->scope_id, p + 24, 4);
        return 0;
    }
    return -SNX_EAFNOSUPPORT;
}
/* writes what fits of the address, and returns its whole size (the addrlen the guest gets back) */
static u32 addr_out(const SnxSockAddr* a, u64 dst, u64 cap) {
    u8 p[BSD_SOCKADDR_IN6];
    u32 n = a->family == SNX_AF_INET6 ? BSD_SOCKADDR_IN6 : BSD_SOCKADDR_IN;
    memset(p, 0, sizeof p);
    p[0] = (u8)n;
    p[1] = (u8)domain_out(a->family ? a->family : SNX_AF_INET);
    memcpy(p + 2, &a->port, 2);
    if (n == BSD_SOCKADDR_IN6) {
        memcpy(p + 4, &a->flowinfo, 4);
        memcpy(p + 8, a->addr, 16);
        memcpy(p + 24, &a->scope_id, 4);
    } else
        memcpy(p + 4, a->addr, 4);
    if (dst) memcpy(GP(dst), p, cap < n ? cap : n);
    return n;
}

/* ============================================================ descriptors */
static SnxSocket* fd_get(Bsd* b, i32 fd, BsdFd* copy) {
    SnxSocket* s = NULL;
    if (fd < 0 || fd >= BSD_MAX_FD) return NULL;
    pm_lock(&b->lk);
    if (b->fd[fd].s) {
        s = snx_socket_ref(b->fd[fd].s);
        if (copy) *copy = b->fd[fd];
    }
    pm_unlock(&b->lk);
    return s;
}
/* takes the reference s carries. -SNX_EMFILE when the table is full */
static int fd_add(Bsd* b, SnxSocket* s, int flags, int exempt) {
    pm_lock(&b->lk);
    for (int i = 0; i < BSD_MAX_FD; i++)
        if (!b->fd[i].s) {
            b->fd[i].s = s;
            b->fd[i].flags = flags;
            b->fd[i].exempt = exempt;
            pm_unlock(&b->lk);
            return i;
        }
    pm_unlock(&b->lk);
    snx_socket_unref(s);
    WARN_LIMIT(4, "bsd: no free descriptor\n");
    return -SNX_EMFILE;
}
static void bsd_destroy(SvcObj* o) {
    Bsd* b = o->data;
    for (int i = 0; i < BSD_MAX_FD; i++) snx_socket_unref(b->fd[i].s);
}

/* ============================================================ replies */
static int reply(IpcCtx* c, int64_t r) {
    out32(c, r < 0 ? (u32)-1 : (u32)r);
    out32(c, r < 0 ? (u32)-r : 0);
    return 0;
}
static int reply_len(IpcCtx* c, int64_t r, u32 len) {
    reply(c, r);
    out32(c, r < 0 ? 0 : len);
    return 0;
}
#define WITH_SOCKET(fdarg)                                                                                             \
    BsdFd d;                                                                                                           \
    SnxSocket* s = fd_get(b, (i32)(fdarg), &d);                                                                        \
    if (!s) return reply(c, -SNX_EBADF)

/* ============================================================ commands */
static int bsd_socket(Bsd* b, IpcCtx* c, int exempt) {
    u32 gdom = in32(c, 0), gtype = in32(c, 4), proto = in32(c, 8);
    int dom = domain_in(gdom), type = (int)(gtype & ~(u32)(BSD_SOCK_NONBLOCK | BSD_SOCK_CLOEXEC));
    if (dom < 0) return reply(c, -SNX_EAFNOSUPPORT);
    /* bsd:u: no raw or seqpacket sockets, except raw ICMP (Eden) */
    if (!b->system && (type == SNX_SOCK_SEQPACKET || type == SNX_SOCK_RAW) &&
        !(type == SNX_SOCK_RAW && dom == SNX_AF_INET && proto == SNX_IPPROTO_ICMP))
        return reply(c, -SNX_EINVAL);
    SnxSocket* s;
    int r = snx_socket_open(&s, dom, type, (int)proto);
    if (r) {
        LOG("bsd: Socket(%u, %u, %u) failed: errno %d\n", gdom, gtype, proto, -r);
        return reply(c, r);
    }
    if (gtype & BSD_SOCK_NONBLOCK) snx_socket_set_nonblocking(s, 1);
    r = fd_add(b, s, (gtype & BSD_SOCK_NONBLOCK) ? BSD_O_NONBLOCK_NX : 0, exempt);
    LOGV("bsd: Socket(%u, %u, %u) = %d\n", gdom, gtype, proto, r);
    return reply(c, r);
}
static int bsd_select(Bsd* b, IpcCtx* c) {
    /* {i32 nfds, timeval {u64 sec, u64 usec}, bool null} and the read, write and error fd_sets in and out */
    i32 nfds = (i32)in32(c, 0);
    u64 sec = in64(c, 8), usec = in64(c, 16);
    int null_timeout = in8(c, 24);
    u64 ia[3], is[3], oa[3], os[3];
    for (int k = 0; k < 3; k++) {
        buf_in(c, k, &ia[k], &is[k]);
        buf_out(c, k, &oa[k], &os[k]);
    }
    if (nfds < 0 || nfds > BSD_MAX_FD) return reply(c, -SNX_EINVAL);
    SnxPollEntry e[BSD_MAX_FD];
    int which[BSD_MAX_FD], n = 0;
    int64_t r = 0;
    for (i32 fd = 0; fd < nfds; fd++) {
        uint16_t ev = 0;
        for (int k = 0; k < 3; k++)
            if (ia[k] && (u64)fd / 8 < is[k] && (((u8*)GP(ia[k]))[fd / 8] >> (fd % 8) & 1))
                ev |= k == 0 ? SNX_POLLIN : k == 1 ? SNX_POLLOUT : SNX_POLLPRI;
        if (!ev) continue;
        SnxSocket* s = fd_get(b, fd, NULL);
        if (!s) {
            r = -SNX_EBADF;
            break;
        }
        e[n].socket = s;
        e[n].host_fd = -1;
        e[n].events = ev;
        which[n++] = fd;
    }
    if (!r) {
        int ms = -1;
        if (!null_timeout) {
            u64 t = sec * 1000 + (usec + 999) / 1000;
            ms = t > 0x7FFFFFFF ? 0x7FFFFFFF : (int)t;
        }
        r = snx_net_poll(e, (size_t)n, ms);
    }
    if (r >= 0) {
        u8 sets[3][BSD_MAX_FD / 8];
        memset(sets, 0, sizeof sets);
        r = 0;
        for (int i = 0; i < n; i++) {
            uint16_t ev = e[i].revents;
            int fd = which[i];
            int hit[3] = {(e[i].events & SNX_POLLIN) && (ev & (SNX_POLLIN | SNX_POLLHUP | SNX_POLLERR)),
                          (e[i].events & SNX_POLLOUT) && (ev & (SNX_POLLOUT | SNX_POLLHUP | SNX_POLLERR)),
                          (e[i].events & SNX_POLLPRI) && (ev & SNX_POLLPRI)};
            for (int k = 0; k < 3; k++)
                if (hit[k]) {
                    sets[k][fd / 8] |= (u8)(1 << (fd % 8));
                    r++;
                }
        }
        for (int k = 0; k < 3; k++)
            if (oa[k]) {
                memset(GP(oa[k]), 0, os[k]);
                memcpy(GP(oa[k]), sets[k], os[k] < sizeof sets[k] ? os[k] : sizeof sets[k]);
            }
    }
    for (int i = 0; i < n; i++) snx_socket_unref(e[i].socket);
    return reply(c, r);
}
static int bsd_poll(Bsd* b, IpcCtx* c) {
    i32 nfds = (i32)in32(c, 0), timeout = (i32)in32(c, 4);
    u64 ia, is, oa, os;
    buf_in(c, 0, &ia, &is);
    buf_out(c, 0, &oa, &os);
    /* Eden: no entries is -1 with errno 0 */
    if (!nfds) {
        out32(c, (u32)-1);
        out32(c, 0);
        return 0;
    }
    if (nfds < 0) return reply(c, -SNX_EINVAL);
    if (nfds > 4096 || !ia || is < (u64)nfds * 8 || (oa && os < (u64)nfds * 8)) return reply(c, -SNX_EINVAL);
    SnxPollEntry* e = calloc((size_t)nfds, sizeof *e);
    if (!e) return reply(c, -SNX_ENOMEM);
    const u8* in = GP(ia);
    int bad = 0;
    for (i32 i = 0; i < nfds; i++) {
        i32 fd;
        u16 ev;
        memcpy(&fd, in + i * 8, 4);
        memcpy(&ev, in + i * 8 + 4, 2);
        e[i].host_fd = -1;
        e[i].events = poll_in(ev);
        if (fd < 0) continue; /* ignored, as POSIX says */
        e[i].socket = fd_get(b, fd, NULL);
        if (!e[i].socket) bad++;
    }
    int64_t r = snx_net_poll(e, (size_t)nfds, bad ? 0 : timeout);
    u8* out = oa ? GP(oa) : NULL;
    if (r >= 0) r += bad;
    for (i32 i = 0; i < nfds; i++) {
        i32 fd;
        memcpy(&fd, in + i * 8, 4);
        u16 rev = r >= 0 ? poll_out(e[i].revents) : 0;
        if (r >= 0 && fd >= 0 && !e[i].socket) rev = 0x0020; /* POLLNVAL */
        if (out) {
            memmove(out + i * 8, in + i * 8, 6);
            memcpy(out + i * 8 + 6, &rev, 2);
        }
        snx_socket_unref(e[i].socket);
    }
    free(e);
    return reply(c, r);
}
static int64_t do_recv(Bsd* b, IpcCtx* c, i32 fd, u32 gflags, u64 addr_buf, u64 addr_cap, u32* addrlen) {
    u64 a, n;
    buf_out(c, 0, &a, &n);
    SnxSocket* s = fd_get(b, fd, NULL);
    if (!s) return -SNX_EBADF;
    SnxSockAddr from = {0};
    int64_t r;
    if (!n)
        r = 0; /* Eden: nothing to receive into, and a datagram stays queued */
    else if (!a)
        r = -SNX_EFAULT;
    else
        r = snx_socket_recv(s, a ? GP(a) : NULL, n, msg_in(gflags), addrlen ? &from : NULL, NULL);
    /* a connected stream socket names no sender (Eden clears the address for connection based sockets) */
    if (r >= 0 && addrlen) {
        if (from.family && snx_socket_type(s) != SNX_SOCK_STREAM)
            *addrlen = addr_out(&from, addr_buf, addr_cap);
        else
            *addrlen = 0;
    }
    snx_socket_unref(s);
    return r;
}
static int64_t do_send(Bsd* b, IpcCtx* c, i32 fd, u32 gflags, const SnxSockAddr* to) {
    u64 a, n;
    buf_in(c, 0, &a, &n);
    SnxSocket* s = fd_get(b, fd, NULL);
    if (!s) return -SNX_EBADF;
    int64_t r;
    if (to && !network_reachable(to))
        r = -SNX_ENETUNREACH;
    else if (!a && n)
        r = -SNX_EFAULT;
    else
        r = snx_socket_send(s, a ? GP(a) : NULL, n, msg_in(gflags), to);
    snx_socket_unref(s);
    return r;
}
static int bsd_name(Bsd* b, IpcCtx* c, int peer) {
    u64 a, n;
    buf_out(c, 0, &a, &n);
    SnxSocket* s = fd_get(b, (i32)in32(c, 0), NULL);
    if (!s) return reply_len(c, -SNX_EBADF, 0);
    SnxSockAddr sa;
    int r = peer ? snx_socket_peer(s, &sa) : snx_socket_name(s, &sa);
    snx_socket_unref(s);
    return reply_len(c, r, r ? 0 : addr_out(&sa, a, n));
}
static int bsd_getsockopt(Bsd* b, IpcCtx* c) {
    u32 glevel = in32(c, 4), gname = in32(c, 8);
    u64 a, n;
    buf_out(c, 0, &a, &n);
    SnxSocket* s = fd_get(b, (i32)in32(c, 0), NULL);
    if (!s) return reply_len(c, -SNX_EBADF, 0);
    int level, name, k = sockopt_in(glevel, gname, &level, &name), r = 0;
    u8 v[32];
    size_t len = sizeof v;
    memset(v, 0, sizeof v);
    if (k < 0)
        r = -SNX_ENOPROTOOPT;
    else if (k == 0)
        len = 4; /* an option the stack ignores reads as 0 */
    else
        r = snx_socket_getsockopt(s, level, name, v, &len);
    snx_socket_unref(s);
    if (k < 0) WARN_LIMIT(8, "bsd: GetSockOpt of unknown option level %#x name %#x\n", glevel, gname);
    if (!r && a) memcpy(GP(a), v, n < len ? n : len);
    return reply_len(c, r, (u32)len);
}
static int bsd_setsockopt(Bsd* b, IpcCtx* c) {
    u32 glevel = in32(c, 4), gname = in32(c, 8);
    u64 a, n;
    buf_in(c, 0, &a, &n);
    WITH_SOCKET(in32(c, 0));
    (void)d;
    int level, name, k = sockopt_in(glevel, gname, &level, &name), r = 0;
    if (k > 0) {
        u8 v[32];
        memset(v, 0, sizeof v);
        if (a) memcpy(v, GP(a), n < sizeof v ? n : sizeof v);
        r = snx_socket_setsockopt(s, level, name, v, n);
        /* the host refusing an option the console has (a buffer size out of its range, SO_DEBUG without
           privileges) isn't the game's fault: it gets what the console would answer */
        if (r == -SNX_ENOPROTOOPT || r == -SNX_EPERM || r == -SNX_EACCES) {
            LOG_LIMIT(8, "bsd: the host refused option level %#x name %#x (errno %d), ignored\n", glevel, gname, -r);
            r = 0;
        }
    } else if (k < 0)
        WARN_LIMIT(8, "bsd: SetSockOpt of unknown option level %#x name %#x, ignored\n", glevel, gname);
    snx_socket_unref(s);
    return reply(c, r);
}
static int bsd_fcntl(Bsd* b, IpcCtx* c) {
    i32 fd = (i32)in32(c, 0), cmd = (i32)in32(c, 4), arg = (i32)in32(c, 8);
    WITH_SOCKET(fd);
    int64_t r;
    switch (cmd) {
        case BSD_F_GETFL:
            r = d.flags |
                (snx_socket_nonblocking(s) && !(d.flags & (BSD_O_NONBLOCK | BSD_O_NONBLOCK_NX)) ? BSD_O_NONBLOCK_NX
                                                                                                : 0);
            break;
        case BSD_F_SETFL:
            snx_socket_set_nonblocking(s, (arg & (BSD_O_NONBLOCK | BSD_O_NONBLOCK_NX)) != 0);
            pm_lock(&b->lk);
            if (b->fd[fd].s == s) b->fd[fd].flags = arg;
            pm_unlock(&b->lk);
            r = 0;
            break;
        case BSD_F_GETFD:
        case BSD_F_SETFD: r = 0; break;
        default: r = -SNX_EINVAL; break;
    }
    snx_socket_unref(s);
    return reply(c, r);
}
static int bsd_ioctl(Bsd* b, IpcCtx* c) {
    u32 req = in32(c, 4);
    u64 ia, is, oa, os;
    buf_in(c, 0, &ia, &is);
    buf_out(c, 0, &oa, &os);
    WITH_SOCKET(in32(c, 0));
    (void)d;
    int64_t r = 0;
    if (req == BSD_FIONBIO) {
        u64 src = ia ? ia : oa, size = ia ? is : os;
        r = src && size >= 4 ? snx_socket_set_nonblocking(s, gr32(src) != 0) : -SNX_EFAULT;
    } else if (req == BSD_FIONREAD) {
        int bytes = 0;
        r = snx_socket_readable_bytes(s, &bytes);
        if (!r && oa && os >= 4)
            gw32(oa, (u32)bytes);
        else if (!r && ia && is >= 4)
            gw32(ia, (u32)bytes); /* an inout buffer */
    } else {
        WARN_LIMIT(4, "bsd: Ioctl %#x is not implemented\n", req);
        r = -SNX_EINVAL;
    }
    snx_socket_unref(s);
    return reply(c, r);
}
static int bsd_close(Bsd* b, IpcCtx* c) {
    i32 fd = (i32)in32(c, 0);
    SnxSocket* s = NULL;
    if (fd >= 0 && fd < BSD_MAX_FD) {
        pm_lock(&b->lk);
        s = b->fd[fd].s;
        memset(&b->fd[fd], 0, sizeof b->fd[fd]);
        pm_unlock(&b->lk);
    }
    if (!s) return reply(c, -SNX_EBADF);
    snx_socket_unref(s);
    LOGV("bsd: Close(%d)\n", fd);
    return reply(c, 0);
}

static int bsd_disp(SvcObj* o, IpcCtx* c) {
    Bsd* b = o->data;
    i32 fd = (i32)in32(c, 0);
    switch (c->cmd) {
        case 0: /* RegisterClient */
        case 33: /* RegisterClientShared */ out32(c, 0); return 0;
        case 1: return 0; /* StartMonitoring */
        case 2: return bsd_socket(b, c, 0);
        case 3: return bsd_socket(b, c, 1);   /* SocketExempt */
        case 4: return reply(c, -SNX_ENOENT); /* Open: no devices */
        case 5: return bsd_select(b, c);
        case 6: return bsd_poll(b, c);
        case 8:
        case 25: return reply(c, do_recv(b, c, fd, c->cmd == 8 ? in32(c, 4) : 0, 0, 0, NULL)); /* Recv, Read */
        case 9: {                                                                              /* RecvFrom */
            u64 aa, an;
            u32 alen = 0;
            buf_out(c, 1, &aa, &an);
            int64_t r = do_recv(b, c, fd, in32(c, 4), aa, an, &alen);
            return reply_len(c, r, alen);
        }
        case 10:
        case 24: return reply(c, do_send(b, c, fd, c->cmd == 10 ? in32(c, 4) : 0, NULL)); /* Send, Write */
        case 11: {                                                                        /* SendTo */
            u64 aa, an;
            SnxSockAddr to;
            buf_in(c, 1, &aa, &an);
            if (!aa || !an) return reply(c, do_send(b, c, fd, in32(c, 4), NULL));
            int r = addr_in(aa, an, &to);
            return reply(c, r ? r : do_send(b, c, fd, in32(c, 4), &to));
        }
        case 12: { /* Accept */
            u64 aa, an;
            buf_out(c, 0, &aa, &an);
            BsdFd d;
            SnxSocket* s = fd_get(b, fd, &d);
            if (!s) return reply_len(c, -SNX_EBADF, 0);
            SnxSocket* n;
            SnxSockAddr peer;
            int r = snx_socket_accept(s, &n, &peer, snx_socket_nonblocking(s)); /* FreeBSD inherits O_NONBLOCK */
            snx_socket_unref(s);
            if (r) return reply_len(c, r, 0);
            r = fd_add(b, n, d.flags & (BSD_O_NONBLOCK | BSD_O_NONBLOCK_NX), 0);
            return reply_len(c, r, r < 0 ? 0 : addr_out(&peer, aa, an));
        }
        case 13:   /* Bind */
        case 14: { /* Connect */
            u64 aa, an;
            SnxSockAddr sa;
            buf_in(c, 0, &aa, &an);
            int r = addr_in(aa, an, &sa);
            if (r) return reply(c, r);
            WITH_SOCKET(fd);
            (void)d;
            if (c->cmd == 13)
                r = snx_socket_bind(s, &sa);
            else if (!network_reachable(&sa))
                r = -SNX_ENETUNREACH;
            else {
                r = snx_socket_connect(s, &sa);
                if (r == -SNX_EISCONN) r = 0; /* Eden: some games connect twice */
            }
            snx_socket_unref(s);
            return reply(c, r);
        }
        case 15:
        case 16: return bsd_name(b, c, c->cmd == 15); /* GetPeerName, GetSockName */
        case 17: return bsd_getsockopt(b, c);
        case 18: { /* Listen */
            WITH_SOCKET(fd);
            (void)d;
            int r = snx_socket_listen(s, (int)in32(c, 4));
            snx_socket_unref(s);
            return reply(c, r);
        }
        case 19: return bsd_ioctl(b, c);
        case 20: return bsd_fcntl(b, c);
        case 21: return bsd_setsockopt(b, c);
        case 22: { /* Shutdown */
            WITH_SOCKET(fd);
            (void)d;
            int r = snx_socket_shutdown(s, (int)in32(c, 4));
            snx_socket_unref(s);
            return reply(c, r);
        }
        case 23: { /* ShutdownAllSockets */
            int how = (int)in32(c, 0);
            for (int i = 0; i < BSD_MAX_FD; i++) {
                BsdFd d;
                SnxSocket* s = fd_get(b, i, &d);
                if (!s) continue;
                if (!d.exempt) snx_socket_shutdown(s, how);
                snx_socket_unref(s);
            }
            return reply(c, 0);
        }
        case 26: return bsd_close(b, c);
        case 27: { /* DuplicateSocket: bsd:s only (Eden) */
            if (!b->system) return reply(c, -SNX_EINVAL);
            WITH_SOCKET(fd);
            return reply(c, fd_add(b, s, d.flags, d.exempt));
        }
        case 31: { /* EventFd(u64 initval, u32 flags) */
            u32 flags = in32(c, 8);
            SnxSocket* s;
            int r = snx_socket_eventfd(&s, in64(c, 0), flags & BSD_EFD_SEMAPHORE);
            if (r) return reply(c, r);
            if (flags & BSD_EFD_NONBLOCK) snx_socket_set_nonblocking(s, 1);
            return reply(c, fd_add(b, s, (flags & BSD_EFD_NONBLOCK) ? BSD_O_NONBLOCK_NX : 0, 0));
        }
        case 32: return reply(c, 0); /* RegisterResourceStatisticsName */
    }
    if (ipc_warn_once(o->name, c->cmd)) WARN("  [not implemented] %s cmd %u -> EOPNOTSUPP\n", o->name, c->cmd);
    return reply(c, -SNX_EOPNOTSUPP);
}
static SvcObj* bsd_new(const char* name, int system) {
    SvcObj* o = svcobj_new(name, bsd_disp, sizeof(Bsd));
    Bsd* b = o->data;
    pm_init(&b->lk);
    b->system = system;
    o->destroy = bsd_destroy;
    return o;
}
SvcObj* new_bsd_u(void) { return bsd_new("bsd:u", 0); }
SvcObj* new_bsd_s(void) { return bsd_new("bsd:s", 1); }

/* bsdcfg: the interface configuration, which the system's own services use. Nothing a game changes */
static int bsdcfg_disp(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
SvcObj* new_bsdcfg(void) { return svcobj_new("bsdcfg", bsdcfg_disp, 0); }
