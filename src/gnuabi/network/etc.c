/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The files a guest's libc reads to resolve names. The guest's filesystem never shows the host's, so the runtime
   writes them: resolv.conf with the host's DNS servers (a resolver on the host's loopback, like systemd-resolved,
   works too, since the guest's sockets are the host's), hosts with the runtime's hostname (uname's), and the
   service and protocol names getaddrinfo() and getservbyname() look up. Without resolv.conf, glibc and musl would
   ask 127.0.0.1. */
#include "network/network.h"
#include <stdio.h>
#include <string.h>

static const char g_hosts[] = "127.0.0.1\tlocalhost\n"
                              "127.0.1.1\tscarletnx\n"
                              "::1\tlocalhost ip6-localhost ip6-loopback\n";
static const char g_nsswitch[] = "hosts: files dns\n"
                                 "services: files\n"
                                 "protocols: files\n";
static const char g_protocols[] = "ip\t0\tIP\n"
                                  "icmp\t1\tICMP\n"
                                  "tcp\t6\tTCP\n"
                                  "udp\t17\tUDP\n"
                                  "ipv6\t41\tIPv6\n"
                                  "ipv6-icmp\t58\tIPv6-ICMP\n";
static const char g_services[] = "ftp\t\t21/tcp\n"
                                 "ssh\t\t22/tcp\n"
                                 "telnet\t\t23/tcp\n"
                                 "smtp\t\t25/tcp\n"
                                 "domain\t\t53/tcp\n"
                                 "domain\t\t53/udp\n"
                                 "http\t\t80/tcp\t\twww\n"
                                 "ntp\t\t123/udp\n"
                                 "imap\t\t143/tcp\n"
                                 "https\t\t443/tcp\n"
                                 "https\t\t443/udp\n"
                                 "submission\t587/tcp\n"
                                 "imaps\t\t993/tcp\n"
                                 "pop3s\t\t995/tcp\n";

int gnu_net_process_init(GnuProcess* p) {
    char resolv[4096];
    snx_net_dns_servers(NULL, 0, resolv, sizeof resolv);
    int r = gnu_vfs_add_file(&p->vfs, "/etc/resolv.conf", resolv, strlen(resolv));
    if (!r) r = gnu_vfs_add_file(&p->vfs, "/etc/hosts", g_hosts, sizeof g_hosts - 1);
    if (!r) r = gnu_vfs_add_file(&p->vfs, "/etc/nsswitch.conf", g_nsswitch, sizeof g_nsswitch - 1);
    if (!r) r = gnu_vfs_add_file(&p->vfs, "/etc/protocols", g_protocols, sizeof g_protocols - 1);
    if (!r) r = gnu_vfs_add_file(&p->vfs, "/etc/services", g_services, sizeof g_services - 1);
    return r;
}
