/*
 * utun.h -- a point-to-point IP interface owned by this process.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * utun is how VPN clients get an interface without a kernel extension:
 * a socket whose far end is a network interface. Packets written to it
 * appear on the interface; packets routed to the interface are read from
 * it. Each carries a 4-byte address-family header in front. The interface
 * disappears, routes and all, when the socket is closed -- including when
 * the process dies -- so nothing can be left behind.
 *
 * Creating one needs root.
 */

#ifndef UTUN_H
#define UTUN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Returns a non-blocking fd and fills name (e.g. "utun5"), or -1. */
int utun_open(char *name, size_t name_len);

#define UTUN_SKIPPED (-2)

/* One IPv4 packet in. Returns its length; 0 when there is nothing left to
 * read; UTUN_SKIPPED for a packet that was not IPv4 (it is consumed);
 * -1 on error. */
ssize_t utun_read_ipv4(int fd, uint8_t *buf, size_t cap);

/* One IPv4 packet out. Returns 0 on success, -1 on error. */
int utun_write_ipv4(int fd, const uint8_t *packet, size_t len);

#endif /* UTUN_H */
