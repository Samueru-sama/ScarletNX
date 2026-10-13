/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden's GetAddressInfo (src/core/internal_network/network.cpp) and Translate(GetAddrInfoError)
   (src/core/hle/service/sockets/sockets_translate.cpp), branch lizzie/refactor-networking-12:
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
     SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project, GPL-2.0-or-later
   Names through the host's resolver. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* EAI_NODATA, EAI_ADDRFAMILY on glibc */
#endif
#include "network/net_internal.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <netdb.h>
#endif

/* the host codes a host lacks (abi/gai.inc, abi/aiflag.inc), as values no resolver returns */
#ifndef EAI_ADDRFAMILY
#define EAI_ADDRFAMILY (-100001)
#endif
#ifndef EAI_NODATA
#define EAI_NODATA (-100002)
#endif
#ifndef EAI_SYSTEM
#define EAI_SYSTEM (-100003)
#endif
#ifndef EAI_BADHINTS
#define EAI_BADHINTS (-100004)
#endif
#ifndef EAI_PROTOCOL
#define EAI_PROTOCOL (-100005)
#endif
#ifndef EAI_OVERFLOW
#define EAI_OVERFLOW (-100006)
#endif
#ifndef AI_ALL
#define AI_ALL 0
#endif
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 0
#endif
#ifndef AI_V4MAPPED
#define AI_V4MAPPED 0
#endif

static int gai_from_host(int e) {
    if (!e) return 0;
#define SNX_NET_GAI(name, value, host, text)                                                                           \
    if (e == (host)) return value;
#include "network/abi/gai.inc"
    return SNX_EAI_FAIL;
}
const char* snx_net_gai_strerror(int e) {
    if (!e) return "Success";
#define SNX_NET_GAI(name, value, host, text)                                                                           \
    if (e == value) return text;
#include "network/abi/gai.inc"
    return "Unknown error";
}
static int aiflags_to_host(int f) {
    int h = 0;
#define SNX_NET_AIFLAG(name, value, host)                                                                              \
    if (f & value) h |= host;
#include "network/abi/aiflag.inc"
    return h;
}
static int niflags_to_host(int f) {
    int h = 0;
#define SNX_NET_NIFLAG(name, value, host)                                                                              \
    if (f & value) h |= host;
#include "network/abi/niflag.inc"
    return h;
}

int snx_net_parse_ipv4(const char* text, uint8_t out[4]) { return inet_pton(AF_INET, text, out) == 1; }

/* appends the host's entries that keep() picks */
static SnxAddrInfo** append(SnxAddrInfo** tail, const struct addrinfo* list, int udp, int* first) {
    for (const struct addrinfo* a = list; a; a = a->ai_next) {
        if ((a->ai_protocol == IPPROTO_UDP) != udp) continue;
        SnxAddrInfo* e = calloc(1, sizeof *e);
        if (!e) break;
        if (snx_net_addr_from_host(a->ai_addr, (snx_socklen)a->ai_addrlen, &e->addr) ||
            (e->addr.family != SNX_AF_INET && e->addr.family != SNX_AF_INET6)) {
            free(e);
            continue;
        }
        e->family = e->addr.family;
        e->socktype = snx_net_socktype_from_host(a->ai_socktype);
        e->protocol = a->ai_protocol;
        if (*first && a->ai_canonname) {
            size_t n = strlen(a->ai_canonname) + 1;
            if ((e->canonname = malloc(n))) memcpy(e->canonname, a->ai_canonname, n);
        }
        *first = 0;
        *tail = e;
        tail = &e->next;
    }
    return tail;
}
int snx_net_getaddrinfo(const char* host, const char* service, const SnxAddrInfo* hints, SnxAddrInfo** out) {
    *out = NULL;
    if (snx_net_init()) return SNX_EAI_FAIL;
    struct addrinfo h, *res = NULL;
    memset(&h, 0, sizeof h);
    if (hints) {
        h.ai_flags = aiflags_to_host(hints->flags);
        h.ai_family = snx_net_domain_to_host(hints->family);
        if (h.ai_family < 0) return SNX_EAI_FAMILY;
        if (hints->socktype && (h.ai_socktype = snx_net_socktype_to_host(hints->socktype)) < 0) return SNX_EAI_SOCKTYPE;
        h.ai_protocol = hints->protocol;
    }
    int r = getaddrinfo(host && *host ? host : NULL, service && *service ? service : NULL, hints ? &h : NULL, &res);
    if (r) return gai_from_host(r);
    /* Eden: the HB App Store's cURL takes the first entry, and a bug in its DNS cache breaks when that's UDP
       (https://github.com/curl/curl/issues/9274), so UDP entries go last */
    int first = 1;
    SnxAddrInfo** tail = append(out, res, 0, &first);
    append(tail, res, 1, &first);
    freeaddrinfo(res);
    return *out ? 0 : SNX_EAI_NODATA;
}
void snx_net_freeaddrinfo(SnxAddrInfo* a) {
    while (a) {
        SnxAddrInfo* n = a->next;
        free(a->canonname);
        free(a);
        a = n;
    }
}
int snx_net_getnameinfo(const SnxSockAddr* addr, char* host, size_t host_size, char* service, size_t service_size,
                        int flags) {
    if (snx_net_init()) return SNX_EAI_FAIL;
    struct sockaddr_storage ss;
    snx_socklen n;
    if (snx_net_addr_to_host(addr, &ss, &n) || addr->family == SNX_AF_UNSPEC) return SNX_EAI_FAMILY;
    return gai_from_host(getnameinfo((struct sockaddr*)&ss, n, host, host ? (snx_socklen)host_size : 0, service,
                                     service ? (snx_socklen)service_size : 0, niflags_to_host(flags)));
}
