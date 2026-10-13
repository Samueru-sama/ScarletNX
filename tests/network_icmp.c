/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Deterministic ICMP backend tests: inject replies, not fake network success. Including the backend lets this
   exercise its helper queue even on hosts where raw or unprivileged ping sockets are available. */
#include "network/net_icmp.c"
#include <stdio.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static void quiet(const char* message) { (void)message; }

int main(void) {
    CHECK(!snx_net_init());
    snx_net_set_log(quiet);
    Icmp* m = calloc(1, sizeof *m);
    CHECK(m);
    snx_lock_init(&m->lock);
    m->refs = 1;
    m->backend = BACKEND_HELPER;
    CHECK(!snx_notifier_open(m->notify));
    SnxSocket* s = snx_net_socket_new(&g_icmp_ops, m->notify[0], SNX_AF_INET, SNX_SOCK_RAW, SNX_IPPROTO_ICMP);
    CHECK(s);
    s->impl = m;
    snx_socket_set_nonblocking(s, 1);
    SnxSockAddr peer = {.family = SNX_AF_INET, .addr = {127, 0, 0, 1}}, from;
    uint8_t echo[11] = {0, 0, 0, 0, 0x12, 0x34, 0, 1, 'o', 'd', 'd'};
    uint16_t sum = snx_net_checksum(echo, sizeof echo, 0);
    echo[2] = (uint8_t)(sum >> 8);
    echo[3] = (uint8_t)sum;
    CHECK(!snx_net_checksum(echo, sizeof echo, 0)); /* odd-length payload */
    uint8_t buf[64];
    CHECK(snx_socket_recv(s, buf, sizeof buf, 0, NULL, NULL) == -SNX_EAGAIN);
    CHECK(snx_socket_send(s, echo, 7, 0, &peer) == -SNX_EINVAL);
    CHECK(snx_socket_send(s, echo, 65508, 0, &peer) == -SNX_EMSGSIZE);
    CHECK(snx_socket_send(s, echo, sizeof echo, 0, NULL) == -SNX_EDESTADDRREQ);

    queue_reply(m, &peer, echo, sizeof echo);
    SnxPollEntry p = {s, -1, SNX_POLLIN, 0};
    CHECK(snx_net_poll(&p, 1, 0) == 1 && (p.revents & SNX_POLLIN));
    int bytes = 0;
    CHECK(!snx_socket_readable_bytes(s, &bytes) && bytes == 31);
    CHECK(snx_socket_recv(s, buf, sizeof buf, SNX_MSG_PEEK, &from, NULL) == 31);
    CHECK(buf[0] == 0x45 && buf[9] == SNX_IPPROTO_ICMP && !snx_net_checksum(buf, 20, 0));
    CHECK(!memcmp(buf + 20, echo, sizeof echo) && from.addr[0] == 127);
    CHECK(m->queued == 1);

    /* A peeking receiver owns a snapshot even when another receiver consumes the original. */
    Reply *peek, *taken;
    CHECK(!pop_reply(m, 1, &peek) && peek);
    CHECK(!pop_reply(m, 0, &taken) && taken && taken != peek);
    free(taken);
    CHECK(!memcmp(peek->data, echo, sizeof echo));
    free(peek);
    CHECK(snx_net_poll(&p, 1, 0) == 0);

    queue_reply(m, &peer, echo, sizeof echo);
    int flags = 0;
    CHECK(snx_socket_recv(s, buf, 23, 0, NULL, &flags) == 23 && (flags & SNX_MSG_TRUNC));
    CHECK(m->queued == 0 && snx_net_poll(&p, 1, 0) == 0);
    for (int i = 0; i < MAX_QUEUED + 1; i++) queue_reply(m, &peer, echo, sizeof echo);
    CHECK(m->queued == MAX_QUEUED);
    for (int i = 0; i < MAX_QUEUED; i++) CHECK(snx_socket_recv(s, buf, sizeof buf, 0, NULL, NULL) == 31);
    CHECK(snx_socket_recv(s, buf, sizeof buf, 0, NULL, NULL) == -SNX_EAGAIN);

#ifndef _WIN32
    /* A missing helper must release its reserved slot on every request, not just decrement pending. */
    const char* path = getenv("PATH");
    char* saved = path ? strdup(path) : NULL;
    CHECK(!path || saved);
    CHECK(!setenv("PATH", "/nonexistent-scarletnx-test-bin", 1));
    for (int i = 0; i < MAX_PENDING + 1; i++) {
        Request* q = calloc(1, sizeof *q + sizeof echo);
        CHECK(q);
        q->m = m;
        q->to = peer;
        q->size = sizeof echo;
        q->slot = i % MAX_PENDING;
        q->wait_us = 1000;
        memcpy(q->msg, echo, sizeof echo);
        m->pids[q->slot] = -1;
        m->pending++;
        m->refs++;
        echo_worker(q);
        CHECK(m->pids[i % MAX_PENDING] == 0 && m->pending == 0 && m->refs == 1);
    }
    CHECK(!m->queued); /* no helper success, no fabricated reply */
    CHECK(saved ? !setenv("PATH", saved, 1) : !unsetenv("PATH"));
    free(saved);
#endif
    snx_socket_unref(s);
    puts("network_icmp: ok (synthetic queue and helper-failure tests, not a live ping)");
    return 0;
}
