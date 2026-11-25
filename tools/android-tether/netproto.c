/*
 * netproto.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * As in rndis/, every offset derived from a received packet is checked
 * against the bytes that actually arrived before it is read, with the
 * arithmetic done in a type wide enough not to wrap.
 */

#include "netproto.h"

#include <string.h>

static const uint8_t kBroadcastMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t kDhcpCookie[4] = { 99, 130, 83, 99 };

#define IPV4_HLEN       20u
#define UDP_HLEN        8u
#define BOOTP_FIXED     236u        /* up to and including the file field */
#define BOOTP_MIN       300u        /* RFC 951: pad the message to this */
#define DHCP_PORT_SERVER 67u
#define DHCP_PORT_CLIENT 68u
#define ARP_LEN         28u

static uint16_t
get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void
put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int
is_zero4(const uint8_t a[4])
{
    return (a[0] | a[1] | a[2] | a[3]) == 0;
}

/* ---------------------------------------------------------------- */

uint16_t
net_ethertype(const uint8_t *frame, uint32_t len)
{
    return len < ETH_HLEN ? 0 : get16(frame + 12);
}

uint32_t
net_ipv4_len(const uint8_t *frame, uint32_t len)
{
    if (net_ethertype(frame, len) != ETHERTYPE_IPV4 || len < ETH_HLEN + IPV4_HLEN)
        return 0;

    const uint8_t *ip = frame + ETH_HLEN;
    const uint32_t ihl = (ip[0] & 0x0Fu) * 4u;
    const uint32_t total = get16(ip + 2);

    if ((ip[0] >> 4) != 4 || ihl < IPV4_HLEN || total < ihl ||
        (uint64_t)ETH_HLEN + total > len)
        return 0;
    return total;
}

size_t
net_put_eth(uint8_t *frame, const uint8_t dst[6], const uint8_t src[6],
            uint16_t ethertype)
{
    memcpy(frame, dst, ETH_ALEN);
    memcpy(frame + 6, src, ETH_ALEN);
    put16(frame + 12, ethertype);
    return ETH_HLEN;
}

uint16_t
net_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2)
        sum += get16(data + i);
    if (i < len)
        sum += (uint32_t)data[i] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---------------------------------------------------------------- */
/* DHCP                                                              */
/* ---------------------------------------------------------------- */

size_t
dhcp_build(uint8_t *f, size_t cap, const dhcp_request_t *req)
{
    const size_t udp_len = UDP_HLEN + BOOTP_MIN;
    const size_t ip_len = IPV4_HLEN + udp_len;
    const size_t total = ETH_HLEN + ip_len;

    if (f == NULL || req == NULL || cap < total)
        return 0;
    memset(f, 0, total);

    net_put_eth(f, req->dst_mac, req->client_mac, ETHERTYPE_IPV4);

    uint8_t *ip = f + ETH_HLEN;
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)ip_len);
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, req->src_ip, 4);
    memcpy(ip + 16, req->dst_ip, 4);
    put16(ip + 10, net_checksum(ip, IPV4_HLEN));

    /* A UDP checksum of zero means "none", which IPv4 allows. */
    uint8_t *udp = ip + IPV4_HLEN;
    put16(udp, DHCP_PORT_CLIENT);
    put16(udp + 2, DHCP_PORT_SERVER);
    put16(udp + 4, (uint16_t)udp_len);

    uint8_t *b = udp + UDP_HLEN;
    b[0] = 1;                                   /* BOOTREQUEST */
    b[1] = 1;                                   /* Ethernet */
    b[2] = ETH_ALEN;
    b[4] = (uint8_t)(req->xid >> 24);
    b[5] = (uint8_t)(req->xid >> 16);
    b[6] = (uint8_t)(req->xid >> 8);
    b[7] = (uint8_t)req->xid;
    memcpy(b + 12, req->ciaddr, 4);
    /*
     * Ask for a broadcast reply only while we have no address: until then
     * there is nothing a unicast reply could be addressed to that our side
     * would accept.
     */
    if (is_zero4(req->ciaddr))
        b[10] = 0x80;
    memcpy(b + 28, req->client_mac, ETH_ALEN);

    uint8_t *o = b + BOOTP_FIXED;
    memcpy(o, kDhcpCookie, 4);
    o += 4;
    *o++ = 53; *o++ = 1; *o++ = req->type;
    if (!is_zero4(req->requested_ip)) {
        *o++ = 50; *o++ = 4;
        memcpy(o, req->requested_ip, 4);
        o += 4;
    }
    if (!is_zero4(req->server_id)) {
        *o++ = 54; *o++ = 4;
        memcpy(o, req->server_id, 4);
        o += 4;
    }
    if (req->type != DHCP_RELEASE) {
        /* subnet mask, router, DNS, lease time */
        *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    }
    *o++ = 255;
    /* The rest of the 300 bytes is already zero: pad. */

    return total;
}

dhcp_parse_result_t
dhcp_parse(const uint8_t *f, uint32_t len, uint32_t xid, dhcp_reply_t *out)
{
    const uint32_t ip_total = net_ipv4_len(f, len);
    if (ip_total == 0)
        return DHCP_PARSE_NOT_DHCP;

    const uint8_t *ip = f + ETH_HLEN;
    const uint32_t ihl = (ip[0] & 0x0Fu) * 4u;
    if (ip[9] != 17 || (uint64_t)ihl + UDP_HLEN > ip_total)
        return DHCP_PARSE_NOT_DHCP;

    const uint8_t *udp = ip + ihl;
    if (get16(udp) != DHCP_PORT_SERVER || get16(udp + 2) != DHCP_PORT_CLIENT)
        return DHCP_PARSE_NOT_DHCP;

    /* From here on it is addressed to a DHCP client. */
    const uint32_t udp_len = get16(udp + 4);
    if (udp_len < UDP_HLEN || (uint64_t)ihl + udp_len > ip_total)
        return DHCP_PARSE_MALFORMED;

    const uint8_t *b = udp + UDP_HLEN;
    const uint32_t blen = udp_len - UDP_HLEN;
    if (blen < BOOTP_FIXED + 4 || b[0] != 2)
        return DHCP_PARSE_MALFORMED;

    const uint32_t got_xid = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 |
                             (uint32_t)b[6] << 8 | (uint32_t)b[7];
    if (got_xid != xid)
        return DHCP_PARSE_WRONG_XID;
    if (memcmp(b + BOOTP_FIXED, kDhcpCookie, 4) != 0)
        return DHCP_PARSE_MALFORMED;

    dhcp_reply_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.your_ip, b + 16, 4);
    memcpy(r.src_mac, f + 6, ETH_ALEN);
    memcpy(r.src_ip, ip + 12, 4);

    /* Options: tag, length, value, until 255 or the end of the message. */
    uint32_t i = BOOTP_FIXED + 4;
    while (i < blen) {
        const uint8_t tag = b[i];
        if (tag == 255)
            break;
        if (tag == 0) {
            i++;
            continue;
        }
        if ((uint64_t)i + 2 > blen)
            return DHCP_PARSE_MALFORMED;
        const uint32_t olen = b[i + 1];
        const uint8_t *v = b + i + 2;
        if ((uint64_t)i + 2 + olen > blen)
            return DHCP_PARSE_MALFORMED;

        switch (tag) {
        case 53:
            if (olen == 1)
                r.type = v[0];
            break;
        case 1:
            if (olen == 4)
                memcpy(r.subnet_mask, v, 4);
            break;
        case 3:
            if (olen >= 4)
                memcpy(r.router, v, 4);
            break;
        case 6:
            for (uint32_t k = 0; k + 4 <= olen && r.dns_count < DHCP_MAX_DNS; k += 4)
                memcpy(r.dns[r.dns_count++], v + k, 4);
            break;
        case 51:
            if (olen == 4)
                r.lease_seconds = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 |
                                  (uint32_t)v[2] << 8 | (uint32_t)v[3];
            break;
        case 54:
            if (olen == 4)
                memcpy(r.server_id, v, 4);
            break;
        default:
            break;
        }
        i += 2 + olen;
    }

    if (r.type == 0)
        return DHCP_PARSE_MALFORMED;
    if (is_zero4(r.server_id))
        memcpy(r.server_id, r.src_ip, 4);

    if (out != NULL)
        *out = r;
    return DHCP_PARSE_OK;
}

const char *
dhcp_type_name(uint8_t type)
{
    switch (type) {
    case DHCP_DISCOVER: return "DISCOVER";
    case DHCP_OFFER:    return "OFFER";
    case DHCP_REQUEST:  return "REQUEST";
    case DHCP_ACK:      return "ACK";
    case DHCP_NAK:      return "NAK";
    case DHCP_RELEASE:  return "RELEASE";
    default:            return "?";
    }
}

/* ---------------------------------------------------------------- */
/* ARP                                                               */
/* ---------------------------------------------------------------- */

/* Ethernet/IPv4 ARP: htype 1, ptype 0x0800, hlen 6, plen 4. */
static int
arp_is_ipv4(const uint8_t *frame, uint32_t len)
{
    if (net_ethertype(frame, len) != ETHERTYPE_ARP || len < ETH_HLEN + ARP_LEN)
        return 0;
    const uint8_t *a = frame + ETH_HLEN;
    return get16(a) == 1 && get16(a + 2) == ETHERTYPE_IPV4 && a[4] == 6 && a[5] == 4;
}

static size_t
arp_build(uint8_t *f, size_t cap, uint16_t op, const uint8_t eth_dst[6],
          const uint8_t sha[6], const uint8_t spa[4],
          const uint8_t tha[6], const uint8_t tpa[4])
{
    if (f == NULL || cap < ETH_MIN_FRAME)
        return 0;
    memset(f, 0, ETH_MIN_FRAME);

    net_put_eth(f, eth_dst, sha, ETHERTYPE_ARP);
    uint8_t *a = f + ETH_HLEN;
    put16(a, 1);
    put16(a + 2, ETHERTYPE_IPV4);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, sha, 6);
    memcpy(a + 14, spa, 4);
    memcpy(a + 18, tha, 6);
    memcpy(a + 24, tpa, 4);
    return ETH_MIN_FRAME;   /* padded to the Ethernet minimum */
}

size_t
arp_build_request(uint8_t *frame, size_t cap, const uint8_t our_mac[6],
                  const uint8_t our_ip[4], const uint8_t target_ip[4])
{
    static const uint8_t unknown[6] = { 0 };
    return arp_build(frame, cap, 1, kBroadcastMac, our_mac, our_ip, unknown, target_ip);
}

size_t
arp_build_reply(uint8_t *out, size_t cap, const uint8_t *in, uint32_t in_len,
                const uint8_t our_mac[6], const uint8_t our_ip[4])
{
    if (!arp_is_ipv4(in, in_len))
        return 0;
    const uint8_t *a = in + ETH_HLEN;
    if (get16(a + 6) != 1 || memcmp(a + 24, our_ip, 4) != 0)
        return 0;
    /* Reply to the asker, telling them our MAC is behind our IP. */
    return arp_build(out, cap, 2, a + 8, our_mac, our_ip, a + 8, a + 14);
}

int
arp_sender_mac(const uint8_t *frame, uint32_t len, const uint8_t ip[4], uint8_t mac[6])
{
    if (!arp_is_ipv4(frame, len))
        return 0;
    const uint8_t *a = frame + ETH_HLEN;
    if (memcmp(a + 14, ip, 4) != 0)
        return 0;
    memcpy(mac, a + 8, 6);
    return 1;
}
