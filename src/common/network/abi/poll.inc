/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SNX_NET_POLL(NAME, value, host, wsapoll): poll events (SNX_POLL##NAME), Linux's values. wsapoll is 1 for the
   events WSAPoll accepts as input; it rejects the call when any other is set (Eden's TranslatePollEvents). net.c
   defines the host events a host lacks as 0. */
SNX_NET_POLL(IN, 0x001, POLLIN, 1)
SNX_NET_POLL(PRI, 0x002, POLLPRI, 0)
SNX_NET_POLL(OUT, 0x004, POLLOUT, 1)
SNX_NET_POLL(ERR, 0x008, POLLERR, 0)
SNX_NET_POLL(HUP, 0x010, POLLHUP, 0)
SNX_NET_POLL(NVAL, 0x020, POLLNVAL, 0)
SNX_NET_POLL(RDNORM, 0x040, POLLRDNORM, 1)
SNX_NET_POLL(RDBAND, 0x080, POLLRDBAND, 1)
SNX_NET_POLL(WRNORM, 0x100, POLLWRNORM, 1)
SNX_NET_POLL(WRBAND, 0x200, POLLWRBAND, 0)
SNX_NET_POLL(RDHUP, 0x2000, POLLRDHUP, 0)
#undef SNX_NET_POLL
