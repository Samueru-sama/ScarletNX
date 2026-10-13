/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SNX_NET_GAI(NAME, value, host, text): getaddrinfo's errors (SNX_EAI_##NAME). The values are FreeBSD's, which is
   what Horizon's sfdnsres reports. The texts are bionic's (Eden's sockets_translate.cpp). net.c defines the host
   codes a host lacks as values no host uses, and translates in this order: Windows' EAI_NODATA is EAI_NONAME. */
SNX_NET_GAI(ADDRFAMILY, 1, EAI_ADDRFAMILY, "Address family for hostname not supported")
SNX_NET_GAI(AGAIN, 2, EAI_AGAIN, "Temporary failure in name resolution")
SNX_NET_GAI(BADFLAGS, 3, EAI_BADFLAGS, "Invalid value for ai_flags")
SNX_NET_GAI(FAIL, 4, EAI_FAIL, "Non-recoverable failure in name resolution")
SNX_NET_GAI(FAMILY, 5, EAI_FAMILY, "ai_family not supported")
SNX_NET_GAI(MEMORY, 6, EAI_MEMORY, "Memory allocation failure")
SNX_NET_GAI(NONAME, 8, EAI_NONAME, "hostname nor servname provided, or not known")
SNX_NET_GAI(NODATA, 7, EAI_NODATA, "No address associated with hostname")
SNX_NET_GAI(SERVICE, 9, EAI_SERVICE, "servname not supported for ai_socktype")
SNX_NET_GAI(SOCKTYPE, 10, EAI_SOCKTYPE, "ai_socktype not supported")
SNX_NET_GAI(SYSTEM, 11, EAI_SYSTEM, "System error returned in errno")
SNX_NET_GAI(BADHINTS, 12, EAI_BADHINTS, "Invalid value for hints")
SNX_NET_GAI(PROTOCOL, 13, EAI_PROTOCOL, "Resolved protocol is unknown")
SNX_NET_GAI(OVERFLOW, 14, EAI_OVERFLOW, "Argument buffer overflow")
#undef SNX_NET_GAI
