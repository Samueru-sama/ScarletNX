/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SNX_NET_MSG(NAME, value, host): send and receive flags (SNX_MSG_##NAME), Linux's values. host 0 means the stack
   handles the flag itself: host sockets never block, so DONTWAIT and WAITALL are how long the stack waits, and
   SIGPIPE is never raised. net.c defines the host flags a host lacks as 0. */
SNX_NET_MSG(OOB, 0x1, MSG_OOB)
SNX_NET_MSG(PEEK, 0x2, MSG_PEEK)
SNX_NET_MSG(DONTROUTE, 0x4, MSG_DONTROUTE)
SNX_NET_MSG(CTRUNC, 0x8, 0)
SNX_NET_MSG(TRUNC, 0x20, MSG_TRUNC)
SNX_NET_MSG(DONTWAIT, 0x40, 0)
SNX_NET_MSG(EOR, 0x80, MSG_EOR)
SNX_NET_MSG(WAITALL, 0x100, 0)
SNX_NET_MSG(NOSIGNAL, 0x4000, 0)
#undef SNX_NET_MSG
