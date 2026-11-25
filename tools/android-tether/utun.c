/*
 * utun.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 */

#include "utun.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if_utun.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <sys/uio.h>

int
utun_open(char *name, size_t name_len)
{
    const int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) {
        perror("utun: socket");
        return -1;
    }

    struct ctl_info info;
    memset(&info, 0, sizeof(info));
    strlcpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name));
    if (ioctl(fd, CTLIOCGINFO, &info) < 0) {
        perror("utun: CTLIOCGINFO");
        close(fd);
        return -1;
    }

    /* Unit 0 lets the kernel pick the next free utunN. */
    struct sockaddr_ctl addr;
    memset(&addr, 0, sizeof(addr));
    addr.sc_len = sizeof(addr);
    addr.sc_family = AF_SYSTEM;
    addr.ss_sysaddr = AF_SYS_CONTROL;
    addr.sc_id = info.ctl_id;
    addr.sc_unit = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror(errno == EPERM ? "utun: connect (run with sudo)" : "utun: connect");
        close(fd);
        return -1;
    }

    socklen_t len = (socklen_t)name_len;
    if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, name, &len) < 0) {
        perror("utun: UTUN_OPT_IFNAME");
        close(fd);
        return -1;
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

ssize_t
utun_read_ipv4(int fd, uint8_t *buf, size_t cap)
{
    uint32_t family;
    struct iovec iov[2] = {
        { .iov_base = &family, .iov_len = sizeof(family) },
        { .iov_base = buf, .iov_len = cap },
    };

    const ssize_t n = readv(fd, iov, 2);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if (n == 0)
        return 0;
    if ((size_t)n <= sizeof(family) || ntohl(family) != AF_INET)
        return UTUN_SKIPPED;
    return n - (ssize_t)sizeof(family);
}

int
utun_write_ipv4(int fd, const uint8_t *packet, size_t len)
{
    uint32_t family = htonl(AF_INET);
    struct iovec iov[2] = {
        { .iov_base = &family, .iov_len = sizeof(family) },
        { .iov_base = (void *)packet, .iov_len = len },
    };
    return writev(fd, iov, 2) < 0 ? -1 : 0;
}
