/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden's src/core/hle/service/sockets/sfdnsres.cpp and nsd.cpp (branch lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
     SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project, GPL-2.0-or-later
   sfdnsres, the resolver behind the SDK's getaddrinfo and gethostbyname, on the host's resolver, and nsd, which
   turns Nintendo's placeholder FQDNs into names. The console only resolves IPv4. The serialized hostent and
   addrinfo are libnx's resolver.c layout, which Eden verified against the console: big-endian fields, except the
   address and port, which the console passes through htonl once more. */
#include "hle/services/network/network.h"
#ifndef _WIN32
#include <strings.h>
#endif

enum { NETDB_INTERNAL = -1, NETDB_SUCCESS = 0, NETDB_HOST_NOT_FOUND = 1, NETDB_TRY_AGAIN = 2, NETDB_NO_RECOVERY = 3 };
#define ADDRINFO_MAGIC 0xBEEFCAFEu

static int blocked(const char* host) {
    size_t n = strlen(host);
#define BLOCKED_HOST(d)                                                                                                \
    {                                                                                                                  \
        size_t k = sizeof(d) - 1;                                                                                      \
        if (n >= k && !strcasecmp(host + n - k, d) && (n == k || host[n - k - 1] == '.')) return 1;                    \
    }
#include "hle/services/network/abi/blocked_hosts.inc"
    return 0;
}
/* what the game may resolve: anything online; offline only numbers and localhost, as a console without a
   connection, whose resolver can't reach a server */
static int resolvable(const char* host) {
    uint8_t ip[4];
    if (network_online() || !host[0] || !strcasecmp(host, "localhost") || snx_net_parse_ipv4(host, ip)) return 1;
    return 0;
}
/* Eden's mappings, checked on the console though not exhaustively */
static int gai_netdb(int e) {
    return !e || e == SNX_EAI_SERVICE ? NETDB_SUCCESS : e == SNX_EAI_AGAIN ? NETDB_TRY_AGAIN : NETDB_HOST_NOT_FOUND;
}
static int gai_errno(int e) { return e == SNX_EAI_SERVICE ? SNX_EINVAL : 0; }

/* the buffer's text, NUL-terminated within cap */
static void buf_string(IpcCtx* c, int i, char* out, size_t cap) {
    u64 a, s;
    out[0] = 0;
    buf_in(c, i, &a, &s);
    if (!a || !s) return;
    size_t n = s < cap - 1 ? (size_t)s : cap - 1;
    memcpy(out, GP(a), n);
    out[n] = 0;
}

typedef struct {
    u8* p;
    size_t n, cap;
} Out;
static void put(Out* o, const void* v, size_t n) {
    if (o->n + n <= o->cap) memcpy(o->p + o->n, v, n);
    o->n += n;
}
static void put_be32(Out* o, u32 v) {
    u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
    put(o, b, 4);
}
static void put_be16(Out* o, u16 v) {
    u8 b[2] = {(u8)(v >> 8), (u8)v};
    put(o, b, 2);
}
static void put_reversed(Out* o, const uint8_t* v, size_t n) { /* a big-endian field passed through htonl again */
    for (size_t i = 0; i < n; i++) put(o, &v[n - 1 - i], 1);
}

/* the guest's name lookup, 0 or SNX_EAI_*. IPv4 only */
static int lookup(const char* host, const char* service, int socktype, int protocol, int flags, SnxAddrInfo** out) {
    *out = NULL;
    if (blocked(host)) {
        LOG("sfdnsres: %s is blocked, answering EAI_AGAIN\n", host);
        return SNX_EAI_AGAIN;
    }
    if (!resolvable(host)) return SNX_EAI_AGAIN;
    SnxAddrInfo hints = {flags & (SNX_AI_PASSIVE | SNX_AI_CANONNAME | SNX_AI_NUMERICHOST | SNX_AI_NUMERICSERV),
                         SNX_AF_INET,
                         socktype,
                         protocol,
                         {0},
                         NULL,
                         NULL};
    int r = snx_net_getaddrinfo(host[0] ? host : NULL, service && service[0] ? service : NULL, &hints, out);
    if (r) LOG("sfdnsres: %s: %s\n", host, snx_net_gai_strerror(r));
    return r;
}

/* GetHostByNameRequest(WithOptions): the hostent in output buffer 0. Returns the size, *gai the error */
static u32 host_by_name(IpcCtx* c, int* gai) {
    char host[256];
    buf_string(c, 0, host, sizeof host);
    SnxAddrInfo* list;
    *gai = lookup(host, NULL, SNX_SOCK_STREAM, 0, 0, &list);
    if (*gai) return 0;
    u64 a, s;
    buf_out(c, 0, &a, &s);
    Out o = {a ? GP(a) : NULL, 0, a ? s : 0};
    put(&o, host, strlen(host) + 1); /* h_name: the name asked for */
    put_be32(&o, 0);                 /* no aliases */
    put_be16(&o, 2);                 /* h_addrtype AF_INET */
    put_be16(&o, 4);                 /* h_length */
    u32 count = 0;
    for (SnxAddrInfo* e = list; e; e = e->next) count++;
    put_be32(&o, count);
    for (SnxAddrInfo* e = list; e; e = e->next) put_reversed(&o, e->addr.addr, 4);
    snx_net_freeaddrinfo(list);
    LOG("sfdnsres: %s resolved (%u addresses)\n", host, count);
    return (u32)(o.n <= o.cap ? o.n : o.cap);
}
/* GetAddrInfoRequest(WithOptions): buffers host, service, serialized hints; the list in output buffer 0 */
static u32 addr_info(IpcCtx* c, int* gai) {
    char host[256], service[64];
    buf_string(c, 0, host, sizeof host);
    buf_string(c, 1, service, sizeof service);
    int flags = 0, socktype = 0, protocol = 0;
    u64 ha, hs;
    buf_in(c, 2, &ha, &hs);
    if (ha && hs >= 24) {
        const u8* h = GP(ha);
        u32 f[6];
        for (int i = 0; i < 6; i++)
            f[i] = (u32)h[i * 4] << 24 | (u32)h[i * 4 + 1] << 16 | (u32)h[i * 4 + 2] << 8 | h[i * 4 + 3];
        if (f[0] == ADDRINFO_MAGIC) {
            flags = (int)f[1]; /* FreeBSD's AI_ flags, the stack's too */
            socktype = (int)f[3];
            protocol = (int)f[4];
        }
    }
    SnxAddrInfo* list;
    *gai = lookup(host, service, socktype, protocol, flags, &list);
    if (*gai) return 0;
    u64 a, s;
    buf_out(c, 0, &a, &s);
    Out o = {a ? GP(a) : NULL, 0, a ? s : 0};
    for (SnxAddrInfo* e = list; e; e = e->next) {
        put_be32(&o, ADDRINFO_MAGIC);
        put_be32(&o, 0); /* ai_flags */
        put_be32(&o, 2); /* AF_INET */
        put_be32(&o, (u32)e->socktype);
        put_be32(&o, (u32)e->protocol);
        put_be32(&o, 16); /* ai_addrlen */
        put_be16(&o, 2);  /* sin_family */
        put_reversed(&o, (const uint8_t*)&e->addr.port, 2);
        put_reversed(&o, e->addr.addr, 4);
        u8 zero[8] = {0};
        put(&o, zero, 8);
        if (e->canonname)
            put(&o, e->canonname, strlen(e->canonname) + 1);
        else
            put(&o, zero, 1);
    }
    u8 sentinel[4] = {0};
    put(&o, sentinel, 4);
    snx_net_freeaddrinfo(list);
    LOG("sfdnsres: %s resolved\n", host);
    return (u32)(o.n <= o.cap ? o.n : o.cap);
}
/* GetHostByAddrRequest: {in_addr} in buffer 0, the hostent with the name out */
static u32 host_by_addr(IpcCtx* c, int* netdb) {
    u64 ia, is, a, s;
    buf_in(c, 0, &ia, &is);
    buf_out(c, 0, &a, &s);
    *netdb = NETDB_HOST_NOT_FOUND;
    if (!ia || is < 4) return 0;
    SnxSockAddr sa = {SNX_AF_INET, 0, 0, {0}, 0};
    memcpy(sa.addr, GP(ia), 4);
    char name[256];
    if (!network_reachable(&sa) || snx_net_getnameinfo(&sa, name, sizeof name, NULL, 0, SNX_NI_NAMEREQD)) return 0;
    Out o = {a ? GP(a) : NULL, 0, a ? s : 0};
    put(&o, name, strlen(name) + 1);
    put_be32(&o, 0);
    put_be16(&o, 2);
    put_be16(&o, 4);
    put_be32(&o, 1);
    put_reversed(&o, sa.addr, 4);
    *netdb = NETDB_SUCCESS;
    return (u32)(o.n <= o.cap ? o.n : o.cap);
}
static void write_text(IpcCtx* c, const char* t) {
    u64 a, s;
    buf_out(c, 0, &a, &s);
    if (!a || !s) return;
    size_t n = strlen(t) + 1 < s ? strlen(t) + 1 : (size_t)s;
    memcpy(GP(a), t, n);
    ((char*)GP(a))[n - 1] = 0;
}
static const char* netdb_text(int e) {
    switch (e) {
        case NETDB_SUCCESS: return "Resolver Error 0 (no error)";
        case NETDB_HOST_NOT_FOUND: return "Unknown host";
        case NETDB_TRY_AGAIN: return "Host name lookup failure";
        case NETDB_NO_RECOVERY: return "Unknown server error";
        case 4: return "No address associated with name";
    }
    return "Unknown resolver error";
}

static int sfdnsres_disp(SvcObj* o, IpcCtx* c) {
    int gai = 0, netdb;
    u32 size;
    switch (c->cmd) {
        case 2: /* GetHostByNameRequest -> {netdb error, errno, size} */
            size = host_by_name(c, &gai);
            out32(c, (u32)gai_netdb(gai));
            out32(c, (u32)gai_errno(gai));
            out32(c, size);
            return 0;
        case 10: /* GetHostByNameRequestWithOptions -> {size, netdb error, errno} */
            size = host_by_name(c, &gai);
            out32(c, size);
            out32(c, (u32)gai_netdb(gai));
            out32(c, (u32)gai_errno(gai));
            return 0;
        case 3: /* GetHostByAddrRequest -> {netdb error, errno, size} */
            size = host_by_addr(c, &netdb);
            out32(c, (u32)netdb);
            out32(c, 0);
            out32(c, size);
            return 0;
        case 11: /* GetHostByAddrRequestWithOptions -> {size, netdb error, errno} */
            size = host_by_addr(c, &netdb);
            out32(c, size);
            out32(c, (u32)netdb);
            out32(c, 0);
            return 0;
        case 4: write_text(c, netdb_text((int)in32(c, 0))); return 0;           /* GetHostStringErrorRequest */
        case 5: write_text(c, snx_net_gai_strerror((int)in32(c, 0))); return 0; /* GetGaiStringErrorRequest */
        case 6: /* GetAddrInfoRequest -> {errno, gai error, size} */
            size = addr_info(c, &gai);
            out32(c, (u32)gai_errno(gai));
            out32(c, (u32)gai);
            out32(c, size);
            return 0;
        case 12: /* GetAddrInfoRequestWithOptions -> {size, gai error, netdb error, errno} */
            size = addr_info(c, &gai);
            out32(c, size);
            out32(c, (u32)gai);
            out32(c, (u32)gai_netdb(gai));
            out32(c, (u32)gai_errno(gai));
            return 0;
        case 8: out32(c, 1); return 0;  /* RequestCancelHandleRequest: requests finish before a cancel could come */
        case 9:                         /* CancelRequest */
        case 14: out32(c, 0); return 0; /* ResolverSetOptionRequest -> errno */
    }
    return stub_ok(o, c);
}
SvcObj* new_sfdnsres(void) { return svcobj_new("sfdnsres", sfdnsres_disp, 0); }

/* nsd: Resolve(Ex) returns the FQDN as it is, which serves every server but Nintendo's (Eden). The environment is
   the retail one, lp1 */
static int nsd_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 20:   /* Resolve */
        case 21: { /* ResolveEx */
            char fqdn[0x100];
            buf_string(c, 0, fqdn, sizeof fqdn);
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (a) {
                memset(GP(a), 0, s < sizeof fqdn ? s : sizeof fqdn);
                memcpy(GP(a), fqdn, strlen(fqdn) + 1 <= s ? strlen(fqdn) + 1 : s);
            }
            if (c->cmd == 21) out32(c, 0); /* the result of the resolution */
            return 0;
        }
        case 11: { /* GetEnvironmentIdentifier */
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (a && s >= 8) memcpy(GP(a), "lp1\0\0\0\0\0", 8);
            return 0;
        }
        case 15: return 0;               /* SetChangeEnvironmentIdentifierDisabled */
        case 100: out32(c, 1); return 0; /* GetApplicationServerEnvironmentType: Lp (nn::oe's Dd is 0) */
    }
    return stub_ok(o, c);
}
SvcObj* new_nsd(void) { return svcobj_new("nsd", nsd_disp, 0); }
