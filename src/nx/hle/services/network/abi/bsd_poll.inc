/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BSD_POLL(guest, NAME): Horizon's poll events (FreeBSD's sys/poll.h) as the stack's SNX_POLL##NAME. FreeBSD's
   POLLWRNORM is POLLOUT */
BSD_POLL(0x0001, IN)
BSD_POLL(0x0002, PRI)
BSD_POLL(0x0004, OUT)
BSD_POLL(0x0008, ERR)
BSD_POLL(0x0010, HUP)
BSD_POLL(0x0020, NVAL)
BSD_POLL(0x0040, RDNORM)
BSD_POLL(0x0080, RDBAND)
BSD_POLL(0x0100, WRBAND)
#undef BSD_POLL
