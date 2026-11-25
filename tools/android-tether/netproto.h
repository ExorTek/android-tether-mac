/*
 * netproto.h -- the little IPv4 the bridge has to speak itself.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * A utun interface carries bare IP; the phone speaks Ethernet. Between the
 * two, someone has to lease an address, answer ARP, and put an Ethernet
 * header on and take it off again. That someone is this file.
 *
 * Like rndis/, it has no platform in it and is tested on any host, for the
 * same reason: every DHCP option length and every ARP field below arrives
 * from the phone, and is trusted no further than it can be checked.
 *
 * Addresses are uint8_t[4] in network order throughout, so nothing here
 * depends on host byte order.
 */

#ifndef NETPROTO_H
#define NETPROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ETH_ALEN        6u
#define ETH_HLEN        14u
#define ETH_MIN_FRAME   60u         /* without the FCS, which RNDIS omits */
#define ETHERTYPE_IPV4  0x0800u
#define ETHERTYPE_ARP   0x0806u
#define ETHERTYPE_IPV6  0x86DDu

#define DHCP_DISCOVER   1u
#define DHCP_OFFER      2u
#define DHCP_REQUEST    3u
#define DHCP_ACK        5u
#define DHCP_NAK        6u
#define DHCP_RELEASE    7u

#define DHCP_MAX_DNS    3u

/* ---- Ethernet and IPv4 ---- */

uint16_t net_ethertype(const uint8_t *frame, uint32_t len);

/*
 * The length of the IPv4 packet inside an Ethernet frame, taken from the
 * IP header and checked against what arrived. 0 if the frame does not
 * hold a plausible IPv4 packet.
 *
 * Not simply len - 14: a short packet is padded to the Ethernet minimum,
 * and the padding is not part of it.
 */
uint32_t net_ipv4_len(const uint8_t *frame, uint32_t len);

/* Write an Ethernet header at frame. Returns ETH_HLEN. */
size_t net_put_eth(uint8_t *frame, const uint8_t dst[6], const uint8_t src[6],
                   uint16_t ethertype);

/* The RFC 1071 one's-complement checksum. */
uint16_t net_checksum(const uint8_t *data, size_t len);

/* ---- DHCP (client side only) ---- */

typedef struct {
    uint8_t  type;              /* DHCP_DISCOVER, DHCP_REQUEST, DHCP_RELEASE */
    uint32_t xid;
    uint8_t  client_mac[6];
    uint8_t  ciaddr[4];         /* ours when renewing or releasing, else 0 */
    uint8_t  requested_ip[4];   /* option 50; all zero to omit */
    uint8_t  server_id[4];      /* option 54; all zero to omit */
    uint8_t  dst_mac[6];        /* ff:ff:ff:ff:ff:ff to broadcast */
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];         /* 255.255.255.255 to broadcast */
} dhcp_request_t;

/* Build a whole Ethernet frame. Returns its length, or 0 if cap is short. */
size_t dhcp_build(uint8_t *frame, size_t cap, const dhcp_request_t *req);

typedef enum {
    DHCP_PARSE_OK = 0,
    DHCP_PARSE_NOT_DHCP,        /* not a server reply to a client: ignore */
    DHCP_PARSE_WRONG_XID,       /* a reply, but to someone else */
    DHCP_PARSE_MALFORMED        /* claims to be ours and does not add up */
} dhcp_parse_result_t;

typedef struct {
    uint8_t  type;              /* option 53 */
    uint8_t  your_ip[4];        /* yiaddr */
    uint8_t  server_id[4];      /* option 54, or the IP source if absent */
    uint8_t  subnet_mask[4];    /* option 1, zero if absent */
    uint8_t  router[4];         /* option 3, first entry, zero if absent */
    uint8_t  dns[DHCP_MAX_DNS][4];
    uint32_t dns_count;
    uint32_t lease_seconds;     /* option 51, 0 if absent */
    uint8_t  src_mac[6];        /* who sent it, at the Ethernet layer */
    uint8_t  src_ip[4];
} dhcp_reply_t;

dhcp_parse_result_t dhcp_parse(const uint8_t *frame, uint32_t len, uint32_t xid,
                               dhcp_reply_t *out);

const char *dhcp_type_name(uint8_t type);

/* ---- ARP (Ethernet / IPv4 only) ---- */

/* Build a who-has request for target_ip. Returns the frame length. */
size_t arp_build_request(uint8_t *frame, size_t cap, const uint8_t our_mac[6],
                         const uint8_t our_ip[4], const uint8_t target_ip[4]);

/*
 * If `in` is an ARP request for our_ip, build the reply into `out` and
 * return its length. Otherwise 0.
 */
size_t arp_build_reply(uint8_t *out, size_t cap, const uint8_t *in, uint32_t in_len,
                       const uint8_t our_mac[6], const uint8_t our_ip[4]);

/*
 * If `frame` is any well-formed ARP packet whose sender is `ip`, copy the
 * sender's hardware address to mac and return 1. Otherwise 0.
 */
int arp_sender_mac(const uint8_t *frame, uint32_t len, const uint8_t ip[4],
                   uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* NETPROTO_H */
