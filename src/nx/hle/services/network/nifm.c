/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The online half ported from Eden's src/core/hle/service/nifm/nifm.cpp (branch lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
     SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project, GPL-2.0-or-later
   nifm, which tells the game whether the console is connected. Offline (the network option off, or a host without a
   connection) it answers what misc.c answered before the network stack existed: no request is accepted, and the
   queries about the connection fail like GetCurrentIpAddress does on a console without one. Online it reports the
   host's interface, the one with the default route. */
#include "hle/services/network/network.h"

enum { REQ_INVALID = 1, REQ_ON_HOLD = 2, REQ_ACCEPTED = 3 }; /* nn::nifm::RequestState (1 is also NotSubmitted) */
#define NIFM_NOT_CONNECTED RC(110, 300)

/* IRequest: u[0] the state, data the two events */
typedef struct {
    KEvent* ev[2];
} NifmRequest;
static void nifm_req_destroy(SvcObj* o) {
    NifmRequest* r = o->data;
    for (int i = 0; i < 2; i++) kobj_unref(&r->ev[i]->o);
}
static int nifm_req(SvcObj* o, IpcCtx* c) {
    NifmRequest* r = o->data;
    int online = network_online();
    switch (c->cmd) {
        case 0: out32(c, online ? (u32)o->u[0] : REQ_INVALID); return 0;          /* GetRequestState */
        case 1: return online && o->u[0] != REQ_INVALID ? 0 : NIFM_NOT_CONNECTED; /* GetResult */
        case 2:                                                                   /* GetSystemEventReadableHandles */
            out_copy_h(c, ev_handle(r->ev[0]));
            out_copy_h(c, ev_handle(r->ev[1]));
            return 0;
        case 3: o->u[0] = REQ_INVALID; return 0; /* Cancel */
        case 4:                                  /* Submit: the host is connected already, so it is accepted at once */
            if (online) {
                o->u[0] = REQ_ACCEPTED;
                kevent_signal(r->ev[0]);
            }
            return 0;
        case 5:
        case 6:
        case 11:
        case 21: return 0;
    }
    return stub_ok(o, c);
}
static SvcObj* nifm_req_new(void) {
    SvcObj* o = svcobj_new("IRequest", nifm_req, sizeof(NifmRequest));
    NifmRequest* r = o->data;
    r->ev[0] = owned_event("nifm0");
    r->ev[1] = owned_event("nifm1");
    /* offline the state event starts signaled: the SDK reads the state, which says the request was refused */
    if (!network_online()) r->ev[0]->signaled = 1;
    o->u[0] = REQ_INVALID;
    o->destroy = nifm_req_destroy;
    return o;
}
/* IScanRequest: a scan that is over at once and found nothing */
static int nifm_scan(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: return 0;             /* Submit */
        case 1: out8(c, 0); return 0; /* IsProcessing */
        case 2: return 0;             /* GetResult */
        case 3: {                     /* GetSystemEventReadableHandle */
            KEvent* e = kevent_new("nifm scan");
            e->signaled = 1;
            out_copy_h(c, ev_handle(e));
            return 0;
        }
    }
    return stub_ok(o, c);
}

/* nn::nifm::IpAddressSetting (0xD) then DnsSetting (0x9), the start of IpSettingData and GetCurrentIpConfigInfo's
   reply */
static void ip_config(const SnxNetInterface* i, u8 out[0x16]) {
    uint8_t dns[2][4] = {{1, 1, 1, 1}, {1, 0, 0, 1}};
    uint8_t found[4][4];
    int n = snx_net_dns_servers(found, 4, NULL, 0), k = 0;
    /* a resolver on the host itself (systemd-resolved) means nothing on the console's side: Eden's public ones then */
    for (int j = 0; j < n && k < 2; j++)
        if (found[j][0] != 127) memcpy(dns[k++], found[j], 4);
    memset(out, 0, 0x16);
    out[0] = 1; /* is_automatic */
    memcpy(out + 1, i->ip, 4);
    memcpy(out + 5, i->mask, 4);
    memcpy(out + 9, i->gateway, 4);
    out[0xD] = 1;
    memcpy(out + 0xE, dns[0], 4);
    memcpy(out + 0x12, dns[1], 4);
}
/* nn::nifm::detail::sf::NetworkProfileData (0x17C) for the host's interface */
static void network_profile(const SnxNetInterface* i, u8* p) {
    memset(p, 0, 0x17C);
    ip_config(i, p);
    p[0xC0] = 1500 & 0xFF; /* IpSettingData's MTU, after the 0xAA bytes of proxy settings, all off */
    p[0xC1] = 1500 >> 8;
    /* the profile's id: two FNV-1a hashes of the interface's name, as Eden makes it from the SSID */
    uint64_t h[2] = {0xcbf29ce484222325ull, 0xcbf29ce484222325ull ^ 0x9e3779b97f4a7c15ull};
    for (const char* s = i->name; *s; s++) {
        h[0] = (h[0] ^ (u8)*s) * 0x100000001b3ull;
        h[1] = (h[1] ^ (u8)(*s ^ 0x5A)) * 0x100000001b3ull;
    }
    memcpy(p + 0xC2, h, 16);
    snprintf((char*)p + 0xD2, 0x40, "%s", i->name);
    p[0x112] = 0;                   /* profile type: user */
    p[0x113] = i->wireless ? 1 : 2; /* interface type: Wi-Fi or Ethernet */
    p[0x114] = 1;                   /* is_auto_connect */
    if (i->wireless) {              /* the SSID isn't known: the interface's name stands for it */
        size_t n = strlen(i->name) < 0x20 ? strlen(i->name) : 0x20;
        p[0x116] = (u8)n;
        memcpy(p + 0x117, i->name, n);
        p[0x139] = 1; /* is_secured */
    }
}

static int nifm_gen(SvcObj* o, IpcCtx* c) {
    SnxNetInterface i;
    int online = !network_interface(&i);
    switch (c->cmd) {
        case 1: { /* GetClientId: a u32 in a C buffer */
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (a && s >= 4) gw32(a, 1);
            return 0;
        }
        case 2: out_obj(c, svcobj_new("IScanRequest", nifm_scan, 0)); return 0;
        case 4: out_obj(c, nifm_req_new()); return 0;
        case 5: { /* GetCurrentNetworkProfile */
            if (!online) return NIFM_NOT_CONNECTED;
            u64 a, s;
            u8 p[0x17C];
            buf_out(c, 0, &a, &s);
            network_profile(&i, p);
            if (a) memcpy(GP(a), p, s < sizeof p ? s : sizeof p);
            return 0;
        }
        case 12: /* GetCurrentIpAddress */
            if (!online) return NIFM_NOT_CONNECTED;
            outbytes(c, i.ip, 4);
            return 0;
        case 15: { /* GetCurrentIpConfigInfo */
            if (!online) return NIFM_NOT_CONNECTED;
            u8 cfg[0x16];
            ip_config(&i, cfg);
            outbytes(c, cfg, sizeof cfg);
            return 0;
        }
        case 17: out8(c, (u8)online); return 0; /* IsWirelessCommunicationEnabled */
        case 18:                                /* GetInternetConnectionStatus: type, signal bars, state */
            if (!online) return NIFM_NOT_CONNECTED;
            out8(c, i.wireless ? 1 : 2);
            out8(c, 3);
            out8(c, 4); /* connected */
            return 0;
        case 20:                                /* IsEthernetCommunicationEnabled */
        case 21:                                /* IsAnyInternetRequestAccepted */
        case 22: out8(c, (u8)online); return 0; /* IsAnyForegroundRequestAccepted */
        case 16:                                /* SetWirelessCommunicationEnabled */
        case 19:                                /* SetEthernetCommunicationEnabled */
        case 33:                                /* ConfirmSystemAvailability */
        case 34: return 0;                      /* SetBackgroundRequestEnabled */
    }
    return stub_ok(o, c);
}
static int nifm_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 4 || c->cmd == 5) {
        out_obj(c, svcobj_new("IGeneralService", nifm_gen, 0));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_nifm(void) { return svcobj_new("nifm", nifm_disp, 0); }
