/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SNX_NET_DOMAIN(NAME, value, host): address families (SNX_AF_##NAME). The values are Linux's. AF_UNIX only serves
   socketpair(): a socket bound to a path would reach the host's filesystem, which the guests never see. */
SNX_NET_DOMAIN(UNSPEC, 0, AF_UNSPEC)
SNX_NET_DOMAIN(UNIX, 1, AF_UNIX)
SNX_NET_DOMAIN(INET, 2, AF_INET)
SNX_NET_DOMAIN(INET6, 10, AF_INET6)
#undef SNX_NET_DOMAIN
