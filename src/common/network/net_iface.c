/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden's src/core/internal_network/network_interface.cpp and emu_net_state.cpp (branch
   lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
   The host's interfaces and DNS servers, for what nifm reports and the GNU ABI's /etc/resolv.conf. Eden takes the
   first interface getifaddrs lists, or the one its settings name. Here the interface the host routes the internet
   through comes first: a UDP socket connected to a public address (connecting sends nothing) is bound to its
   address. */
#include "network/net_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#endif
#if defined(__ANDROID__) && __ANDROID_API__ < 24
#include <dlfcn.h>
/* getifaddrs comes with Android 7: looked up where the app may run on an older one, which lists no interface */
static int getifaddrs(struct ifaddrs** list) {
    int (*get)(struct ifaddrs**) = (int (*)(struct ifaddrs**))dlsym(RTLD_DEFAULT, "getifaddrs");
    return get ? get(list) : -1;
}
static void freeifaddrs(struct ifaddrs* list) {
    void (*release)(struct ifaddrs*) = (void (*)(struct ifaddrs*))dlsym(RTLD_DEFAULT, "freeifaddrs");
    if (release) release(list);
}
#endif

/* the local address of the default route, 0 when the host has none */
static int routed_address(uint8_t out[4]) {
    SnxSocket* s;
    if (snx_socket_open(&s, SNX_AF_INET, SNX_SOCK_DGRAM, SNX_IPPROTO_UDP)) return 0;
    SnxSockAddr to = {SNX_AF_INET, 0x3500 /* port 53, network order */, 0, {8, 8, 8, 8}, 0}, me;
    int ok = !snx_socket_connect(s, &to) && !snx_socket_name(s, &me) && memcmp(me.addr, "\0\0\0\0", 4);
    if (ok) memcpy(out, me.addr, 4);
    snx_socket_unref(s);
    return ok;
}

#ifdef _WIN32
static void mask_of(unsigned prefix, uint8_t out[4]) {
    uint32_t m = prefix >= 32 ? 0xFFFFFFFFu : prefix ? ~((1u << (32 - prefix)) - 1) : 0;
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(m >> (24 - i * 8));
}
static IP_ADAPTER_ADDRESSES* adapters(ULONG flags) {
    ULONG size = 16 * 1024;
    for (int tries = 0; tries < 4; tries++) {
        IP_ADAPTER_ADDRESSES* a = malloc(size);
        if (!a) return NULL;
        ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, a, &size);
        if (r == NO_ERROR) return a;
        free(a);
        if (r != ERROR_BUFFER_OVERFLOW) return NULL;
    }
    return NULL;
}
static int list_interfaces(SnxNetInterface* out, int max) {
    IP_ADAPTER_ADDRESSES* list =
        adapters(GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS);
    int n = 0;
    for (IP_ADAPTER_ADDRESSES* a = list; a && n < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            SnxNetInterface* i = &out[n++];
            memset(i, 0, sizeof *i);
            memcpy(i->ip, &((struct sockaddr_in*)u->Address.lpSockaddr)->sin_addr, 4);
            mask_of(u->OnLinkPrefixLength, i->mask);
            for (IP_ADAPTER_GATEWAY_ADDRESS* g = a->FirstGatewayAddress; g; g = g->Next)
                if (g->Address.lpSockaddr->sa_family == AF_INET) {
                    memcpy(i->gateway, &((struct sockaddr_in*)g->Address.lpSockaddr)->sin_addr, 4);
                    break;
                }
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, i->name, sizeof i->name - 1, NULL, NULL);
            i->wireless = a->IfType == IF_TYPE_IEEE80211;
            break;
        }
    }
    free(list);
    return n;
}
int snx_net_dns_servers(uint8_t (*ipv4)[4], int max, char* text, size_t text_size) {
    if (text && text_size) text[0] = 0;
    IP_ADAPTER_ADDRESSES* list = adapters(GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST);
    int n = 0;
    for (IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_DNS_SERVER_ADDRESS* d = a->FirstDnsServerAddress; d; d = d->Next) {
            char name[64];
            DWORD len = sizeof name;
            if (WSAAddressToStringA(d->Address.lpSockaddr, d->Address.iSockaddrLength, NULL, name, &len)) continue;
            if (d->Address.lpSockaddr->sa_family == AF_INET && ipv4 && n < max)
                memcpy(ipv4[n++], &((struct sockaddr_in*)d->Address.lpSockaddr)->sin_addr, 4);
            if (text && strlen(text) + strlen(name) + 13 < text_size && !strstr(text, name)) {
                strcat(text, "nameserver ");
                strcat(text, name);
                strcat(text, "\n");
            }
        }
    }
    free(list);
    return n;
}
#else
#ifdef __linux__
/* the gateway of interface name's default route, from the routing table */
static void linux_gateway(const char* name, uint8_t out[4]) {
    FILE* f = fopen("/proc/net/route", "r");
    if (!f) return;
    char line[256], ifname[64];
    unsigned dest, gw, flags;
    if (!fgets(line, sizeof line, f)) line[0] = 0; /* the header */
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%63s %x %x %x", ifname, &dest, &gw, &flags) == 4 && !strcmp(ifname, name) && !dest &&
            (flags & 2)) {       /* RTF_GATEWAY */
            memcpy(out, &gw, 4); /* the table prints the address as a host-order number of its network-order bytes */
            break;
        }
    fclose(f);
}
static int linux_wireless(const char* name) {
    char path[160];
    snprintf(path, sizeof path, "/sys/class/net/%s/wireless", name);
    return access(path, F_OK) == 0;
}
#endif
static int list_interfaces(SnxNetInterface* out, int max) {
    struct ifaddrs* list;
    if (getifaddrs(&list)) return 0;
    int n = 0;
    for (struct ifaddrs* a = list; a && n < max; a = a->ifa_next) {
        if (!a->ifa_addr || !a->ifa_netmask || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) ||
            (a->ifa_flags & IFF_LOOPBACK))
            continue;
        SnxNetInterface* i = &out[n++];
        memset(i, 0, sizeof *i);
        snprintf(i->name, sizeof i->name, "%s", a->ifa_name);
        memcpy(i->ip, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, 4);
        memcpy(i->mask, &((struct sockaddr_in*)a->ifa_netmask)->sin_addr, 4);
#ifdef __linux__
        linux_gateway(i->name, i->gateway);
        i->wireless = linux_wireless(i->name);
#endif
    }
    freeifaddrs(list);
    return n;
}
int snx_net_dns_servers(uint8_t (*ipv4)[4], int max, char* text, size_t text_size) {
    if (text && text_size) text[0] = 0;
    FILE* f = fopen("/etc/resolv.conf", "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char key[32], value[256];
        if (sscanf(line, "%31s %255s", key, value) != 2) continue;
        int ns = !strcmp(key, "nameserver");
        if (!ns && strcmp(key, "search") && strcmp(key, "domain") && strcmp(key, "options")) continue;
        if (ns && ipv4 && n < max && snx_net_parse_ipv4(value, ipv4[n])) n++;
        size_t len = strlen(line);
        if (text && len && line[len - 1] == '\n' && strlen(text) + len < text_size) strcat(text, line);
    }
    fclose(f);
    return n;
}
#endif

int snx_net_interfaces(SnxNetInterface* out, int max) {
    if (max <= 0 || snx_net_init()) return 0;
    int n = list_interfaces(out, max);
    uint8_t routed[4];
    if (n > 1 && routed_address(routed))
        for (int i = 1; i < n; i++)
            if (!memcmp(out[i].ip, routed, 4)) {
                SnxNetInterface t = out[0];
                out[0] = out[i];
                out[i] = t;
                break;
            }
    return n;
}
int snx_net_primary_interface(SnxNetInterface* out) {
    SnxNetInterface list[16];
    if (snx_net_interfaces(list, 16) <= 0) return -SNX_ENETDOWN;
    *out = list[0];
    return 0;
}
