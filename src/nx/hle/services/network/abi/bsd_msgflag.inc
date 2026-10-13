/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BSD_MSG(guest, NAME): Horizon's send and receive flags (FreeBSD's sys/socket.h) as the stack's SNX_MSG_##NAME */
BSD_MSG(0x00001, OOB)
BSD_MSG(0x00002, PEEK)
BSD_MSG(0x00004, DONTROUTE)
BSD_MSG(0x00008, EOR)
BSD_MSG(0x00010, TRUNC)
BSD_MSG(0x00020, CTRUNC)
BSD_MSG(0x00040, WAITALL)
BSD_MSG(0x00080, DONTWAIT)
BSD_MSG(0x20000, NOSIGNAL)
#undef BSD_MSG
