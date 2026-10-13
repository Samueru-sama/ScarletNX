/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BSD_DOMAIN(guest, NAME): Horizon's address families (FreeBSD's values) as the stack's SNX_AF_##NAME. bsd opens
   IPv4 and IPv6 sockets only. A sockaddr with family 0 is read as AF_INET, as FreeBSD's bind() does for old code */
BSD_DOMAIN(2, INET)
BSD_DOMAIN(28, INET6)
#undef BSD_DOMAIN
