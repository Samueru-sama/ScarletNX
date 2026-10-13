/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The network services, on the stack in src/common/network: bsd (bsd.c), sfdnsres and nsd (dns.c), nifm (nifm.c) */
#ifndef SNX_HLE_NETWORK_H
#define SNX_HLE_NETWORK_H
#include "hle/services/services_int.h"
#include "network/net.h"

/* bsd.c */
void network_init(void); /* the stack, with its messages in the log */
SvcObj* new_bsd_u(void);
SvcObj* new_bsd_s(void);
SvcObj* new_bsdcfg(void);
/* the connection the game is told about: the host's, unless the network option is off */
int network_online(void);
int network_interface(SnxNetInterface* out); /* 0 and the host's interface when online */
/* whether a guest may reach addr: with the network option off, loopback only */
int network_reachable(const SnxSockAddr* addr);

/* dns.c */
SvcObj* new_sfdnsres(void);
SvcObj* new_nsd(void);

/* nifm.c */
SvcObj* new_nifm(void);

#endif
