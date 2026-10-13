/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SNX_NET_LEVEL(NAME, value, host): socket option levels (SNX_SOL_##NAME) */
SNX_NET_LEVEL(IP, 0, IPPROTO_IP)
SNX_NET_LEVEL(SOCKET, 1, SOL_SOCKET)
SNX_NET_LEVEL(TCP, 6, IPPROTO_TCP)
SNX_NET_LEVEL(UDP, 17, IPPROTO_UDP)
SNX_NET_LEVEL(IPV6, 41, IPPROTO_IPV6)
#undef SNX_NET_LEVEL
