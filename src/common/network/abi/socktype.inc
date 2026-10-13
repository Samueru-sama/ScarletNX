/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SNX_NET_SOCKTYPE(NAME, value, host): socket types (SNX_SOCK_##NAME). Linux and FreeBSD (Horizon) agree on these */
SNX_NET_SOCKTYPE(STREAM, 1, SOCK_STREAM)
SNX_NET_SOCKTYPE(DGRAM, 2, SOCK_DGRAM)
SNX_NET_SOCKTYPE(RAW, 3, SOCK_RAW)
SNX_NET_SOCKTYPE(RDM, 4, SOCK_RDM)
SNX_NET_SOCKTYPE(SEQPACKET, 5, SOCK_SEQPACKET)
#undef SNX_NET_SOCKTYPE
