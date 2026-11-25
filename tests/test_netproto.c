/*
 * test_netproto.c -- DHCP, ARP and the IPv4 length, without a phone.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * The bridge in tools/android-tether trusts these parsers with whatever
 * the phone sends. As with the RNDIS tests, most of what is here is the
 * shapes a cooperative phone would never produce.
 */

#include <string.h>

#include "netproto.h"
#include "test_util.h"

static const uint8_t kOurMac[6]   = { 0x7e, 0x08, 0xb6, 0x10, 0xe3, 0xf2 };
static const uint8_t kPhoneMac[6] = { 0xda, 0x43, 0xa3, 0x63, 0xd1, 0x8b };
static const uint8_t kOurIp[4]    = { 10, 177, 117, 33 };
static const uint8_t kPhoneIp[4]  = { 10, 177, 117, 42 };
static const uint8_t kBcastMac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t kBcastIp[4]  = { 255, 255, 255, 255 };

#define XID 0x1b5aa416u

/*
 * Build a server reply the way a phone would: an Ethernet/IPv4/UDP
 * envelope around a BOOTREPLY carrying `opts`. Returns the frame length.
 * The envelope's lengths are computed from what is actually there, so a
 * test can then corrupt exactly one thing.
 */
static uint32_t
make_reply(uint8_t *f, size_t cap, uint32_t xid, const uint8_t *opts, uint32_t opts_len)
{
    const uint32_t bootp = 236 + 4 + opts_len;
    const uint32_t udp = 8 + bootp;
    const uint32_t ip = 20 + udp;
    const uint32_t total = 14 + ip;

    memset(f, 0, cap);
    net_put_eth(f, kBcastMac, kPhoneMac, ETHERTYPE_IPV4);
    uint8_t *h = f + 14;
    h[0] = 0x45;
    h[2] = (uint8_t)(ip >> 8); h[3] = (uint8_t)ip;
    h[8] = 64; h[9] = 17;
    memcpy(h + 12, kPhoneIp, 4);
    memcpy(h + 16, kBcastIp, 4);
    uint8_t *u = h + 20;
    u[1] = 67; u[3] = 68;
    u[4] = (uint8_t)(udp >> 8); u[5] = (uint8_t)udp;
    uint8_t *b = u + 8;
    b[0] = 2; b[1] = 1; b[2] = 6;
    b[4] = (uint8_t)(xid >> 24); b[5] = (uint8_t)(xid >> 16);
    b[6] = (uint8_t)(xid >> 8);  b[7] = (uint8_t)xid;
    memcpy(b + 16, kOurIp, 4);
    memcpy(b + 28, kOurMac, 6);
    b[236] = 99; b[237] = 130; b[238] = 83; b[239] = 99;
    memcpy(b + 240, opts, opts_len);
    return total;
}

static const uint8_t kOfferOpts[] = {
    53, 1, DHCP_OFFER,
    54, 4, 10, 177, 117, 42,
    51, 4, 0x00, 0x00, 0x0e, 0x10,              /* 3600 s */
    1, 4, 255, 255, 255, 0,
    3, 4, 10, 177, 117, 42,
    6, 8, 10, 177, 117, 42, 8, 8, 8, 8,
    255
};

/* ---- IPv4 length ---- */

static void
test_ipv4_len_ignores_padding(void)
{
    uint8_t f[64];
    memset(f, 0, sizeof(f));
    net_put_eth(f, kOurMac, kPhoneMac, ETHERTYPE_IPV4);
    f[14] = 0x45;
    f[17] = 28;                                 /* a 28-byte ping, padded to 60 */

    CHECK(net_ipv4_len(f, 60) == 28, "padding counted as payload");
}

static void
test_ipv4_len_refuses_what_did_not_arrive(void)
{
    uint8_t f[64];
    memset(f, 0, sizeof(f));
    net_put_eth(f, kOurMac, kPhoneMac, ETHERTYPE_IPV4);
    f[14] = 0x45;
    f[16] = 0x05; f[17] = 0xdc;                 /* claims 1500 */

    CHECK(net_ipv4_len(f, 60) == 0, "a length longer than the frame was believed");
    f[16] = 0; f[17] = 10;                      /* shorter than its own header */
    CHECK(net_ipv4_len(f, 60) == 0, "a length shorter than the header was believed");
    f[17] = 28; f[14] = 0x65;                   /* version 6 in an IPv4 frame */
    CHECK(net_ipv4_len(f, 60) == 0, "a non-IPv4 header was believed");
    CHECK(net_ipv4_len(f, 20) == 0, "a truncated frame was read");
}

/* ---- DHCP ---- */

static void
test_discover_is_well_formed(void)
{
    uint8_t f[600];
    dhcp_request_t req;
    memset(&req, 0, sizeof(req));
    req.type = DHCP_DISCOVER;
    req.xid = XID;
    memcpy(req.client_mac, kOurMac, 6);
    memcpy(req.dst_mac, kBcastMac, 6);
    memcpy(req.dst_ip, kBcastIp, 4);

    const size_t len = dhcp_build(f, sizeof(f), &req);
    CHECK(len == 14 + 20 + 8 + 300, "length %zu", len);
    CHECK(net_ipv4_len(f, (uint32_t)len) == len - 14, "IP length disagrees");
    CHECK(net_checksum(f + 14, 20) == 0, "IPv4 header checksum does not verify");
    CHECK(f[14 + 20 + 8 + 10] == 0x80, "broadcast flag not set without an address");
    CHECK(memcmp(f + 6, kOurMac, 6) == 0, "wrong source MAC");
    CHECK(f[14 + 20 + 8 + 240] == 53 && f[14 + 20 + 8 + 242] == DHCP_DISCOVER,
          "message type is not the first option");
    CHECK(dhcp_build(f, len - 1, &req) == 0, "built into a buffer one byte short");
}

static void
test_request_carries_ip_and_server(void)
{
    uint8_t f[600];
    dhcp_request_t req;
    memset(&req, 0, sizeof(req));
    req.type = DHCP_REQUEST;
    req.xid = XID;
    memcpy(req.client_mac, kOurMac, 6);
    memcpy(req.requested_ip, kOurIp, 4);
    memcpy(req.server_id, kPhoneIp, 4);
    memcpy(req.dst_mac, kBcastMac, 6);
    memcpy(req.dst_ip, kBcastIp, 4);
    dhcp_build(f, sizeof(f), &req);

    const uint8_t *o = f + 14 + 20 + 8 + 240;
    CHECK(o[3] == 50 && o[4] == 4 && memcmp(o + 5, kOurIp, 4) == 0, "no option 50");
    CHECK(o[9] == 54 && o[10] == 4 && memcmp(o + 11, kPhoneIp, 4) == 0, "no option 54");
}

static void
test_renew_has_ciaddr_and_no_broadcast_flag(void)
{
    uint8_t f[600];
    dhcp_request_t req;
    memset(&req, 0, sizeof(req));
    req.type = DHCP_REQUEST;
    req.xid = XID;
    memcpy(req.client_mac, kOurMac, 6);
    memcpy(req.ciaddr, kOurIp, 4);
    memcpy(req.src_ip, kOurIp, 4);
    memcpy(req.dst_mac, kPhoneMac, 6);
    memcpy(req.dst_ip, kPhoneIp, 4);
    dhcp_build(f, sizeof(f), &req);

    const uint8_t *b = f + 14 + 20 + 8;
    CHECK(memcmp(b + 12, kOurIp, 4) == 0, "ciaddr not set");
    CHECK(b[10] == 0, "broadcast flag set with an address in hand");
    CHECK(memcmp(f, kPhoneMac, 6) == 0, "renewal not unicast to the server");
}

static void
test_offer_parses(void)
{
    uint8_t f[600];
    const uint32_t len = make_reply(f, sizeof(f), XID, kOfferOpts, sizeof(kOfferOpts));
    dhcp_reply_t r;

    CHECK(dhcp_parse(f, len, XID, &r) == DHCP_PARSE_OK, "a good offer was refused");
    CHECK(r.type == DHCP_OFFER, "type %u", r.type);
    CHECK(memcmp(r.your_ip, kOurIp, 4) == 0, "yiaddr");
    CHECK(memcmp(r.server_id, kPhoneIp, 4) == 0, "server id");
    CHECK(memcmp(r.router, kPhoneIp, 4) == 0, "router");
    CHECK(r.subnet_mask[2] == 255 && r.subnet_mask[3] == 0, "mask");
    CHECK(r.lease_seconds == 3600, "lease %u", r.lease_seconds);
    CHECK(r.dns_count == 2 && r.dns[1][0] == 8, "dns count %u", r.dns_count);
    CHECK(memcmp(r.src_mac, kPhoneMac, 6) == 0, "source MAC");
}

static void
test_parse_ignores_what_is_not_ours(void)
{
    uint8_t f[600];
    uint32_t len = make_reply(f, sizeof(f), XID, kOfferOpts, sizeof(kOfferOpts));
    dhcp_reply_t r;

    CHECK(dhcp_parse(f, len, XID + 1, &r) == DHCP_PARSE_WRONG_XID, "another client's reply accepted");

    f[14 + 20 + 1] = 53;                        /* DNS, not DHCP */
    CHECK(dhcp_parse(f, len, XID, &r) == DHCP_PARSE_NOT_DHCP, "a non-DHCP port was parsed");

    len = make_reply(f, sizeof(f), XID, kOfferOpts, sizeof(kOfferOpts));
    f[14 + 9] = 6;                              /* TCP */
    CHECK(dhcp_parse(f, len, XID, &r) == DHCP_PARSE_NOT_DHCP, "TCP was parsed as DHCP");
}

static void
test_option_running_off_the_end(void)
{
    /* The last option claims 200 bytes; the message ends after 4. */
    const uint8_t opts[] = { 53, 1, DHCP_OFFER, 6, 200, 8, 8, 8, 8 };
    uint8_t f[600];
    const uint32_t len = make_reply(f, sizeof(f), XID, opts, sizeof(opts));

    CHECK(dhcp_parse(f, len, XID, NULL) == DHCP_PARSE_MALFORMED,
          "an option past the end of the message was read");
}

static void
test_option_header_split_by_the_end(void)
{
    /* A tag with no room left for its length byte. */
    const uint8_t opts[] = { 53, 1, DHCP_OFFER, 3 };
    uint8_t f[600];
    const uint32_t len = make_reply(f, sizeof(f), XID, opts, sizeof(opts));

    CHECK(dhcp_parse(f, len, XID, NULL) == DHCP_PARSE_MALFORMED,
          "a truncated option header was read");
}

static void
test_udp_length_past_the_ip_packet(void)
{
    uint8_t f[600];
    const uint32_t len = make_reply(f, sizeof(f), XID, kOfferOpts, sizeof(kOfferOpts));
    f[14 + 20 + 4] = 0x40;                      /* UDP length 16k inside a ~300-byte packet */

    CHECK(dhcp_parse(f, len, XID, NULL) == DHCP_PARSE_MALFORMED,
          "a UDP length longer than its packet was believed");
}

static void
test_reply_without_type_is_refused(void)
{
    const uint8_t opts[] = { 54, 4, 10, 177, 117, 42, 255 };
    uint8_t f[600];
    const uint32_t len = make_reply(f, sizeof(f), XID, opts, sizeof(opts));

    CHECK(dhcp_parse(f, len, XID, NULL) == DHCP_PARSE_MALFORMED,
          "a reply with no message type was accepted");
}

static void
test_missing_server_id_falls_back_to_source(void)
{
    const uint8_t opts[] = { 53, 1, DHCP_ACK, 255 };
    uint8_t f[600];
    const uint32_t len = make_reply(f, sizeof(f), XID, opts, sizeof(opts));
    dhcp_reply_t r;

    CHECK(dhcp_parse(f, len, XID, &r) == DHCP_PARSE_OK, "refused");
    CHECK(memcmp(r.server_id, kPhoneIp, 4) == 0, "server id not taken from the IP source");
}

static void
test_dns_list_is_capped(void)
{
    const uint8_t opts[] = {
        53, 1, DHCP_ACK,
        6, 20, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
        255
    };
    uint8_t f[600];
    const uint32_t len = make_reply(f, sizeof(f), XID, opts, sizeof(opts));
    dhcp_reply_t r;

    CHECK(dhcp_parse(f, len, XID, &r) == DHCP_PARSE_OK, "refused");
    CHECK(r.dns_count == DHCP_MAX_DNS, "dns count %u overflowed the array", r.dns_count);
}

/* ---- ARP ---- */

static void
test_arp_reply_to_a_request_for_us(void)
{
    uint8_t req[64], rep[64];
    arp_build_request(req, sizeof(req), kPhoneMac, kPhoneIp, kOurIp);

    const size_t len = arp_build_reply(rep, sizeof(rep), req, 60, kOurMac, kOurIp);
    CHECK(len == ETH_MIN_FRAME, "no reply, or the wrong length (%zu)", len);
    CHECK(memcmp(rep, kPhoneMac, 6) == 0, "reply not addressed to the asker");
    CHECK(rep[14 + 7] == 2, "not an ARP reply");
    CHECK(memcmp(rep + 14 + 8, kOurMac, 6) == 0, "reply does not carry our MAC");
    CHECK(memcmp(rep + 14 + 24, kPhoneIp, 4) == 0, "reply target is not the asker");

    uint8_t learned[6];
    CHECK(arp_sender_mac(rep, (uint32_t)len, kOurIp, learned) &&
          memcmp(learned, kOurMac, 6) == 0, "could not learn a MAC from our own reply");
}

static void
test_arp_ignores_requests_for_others(void)
{
    const uint8_t other[4] = { 10, 177, 117, 99 };
    uint8_t req[64], rep[64];
    arp_build_request(req, sizeof(req), kPhoneMac, kPhoneIp, other);

    CHECK(arp_build_reply(rep, sizeof(rep), req, 60, kOurMac, kOurIp) == 0,
          "answered for an address that is not ours");
}

static void
test_arp_refuses_short_and_foreign_packets(void)
{
    uint8_t req[64], rep[64], mac[6];
    arp_build_request(req, sizeof(req), kPhoneMac, kPhoneIp, kOurIp);

    CHECK(arp_build_reply(rep, sizeof(rep), req, 14 + 27, kOurMac, kOurIp) == 0,
          "a truncated ARP packet was read");
    req[14 + 4] = 8;                            /* a hardware address that is not 6 bytes */
    CHECK(arp_build_reply(rep, sizeof(rep), req, 60, kOurMac, kOurIp) == 0,
          "non-Ethernet ARP was answered");
    CHECK(!arp_sender_mac(req, 60, kPhoneIp, mac), "learned from non-Ethernet ARP");
}

void
run_netproto_tests(void)
{
    RUN(test_ipv4_len_ignores_padding,              "IPv4 length excludes Ethernet padding");
    RUN(test_ipv4_len_refuses_what_did_not_arrive,  "IPv4 length checked against the frame");
    RUN(test_discover_is_well_formed,               "DISCOVER is well formed");
    RUN(test_request_carries_ip_and_server,         "REQUEST carries options 50 and 54");
    RUN(test_renew_has_ciaddr_and_no_broadcast_flag, "renewal is unicast with ciaddr");
    RUN(test_offer_parses,                          "an OFFER parses");
    RUN(test_parse_ignores_what_is_not_ours,        "other replies and protocols ignored");
    RUN(test_option_running_off_the_end,            "option longer than the message");
    RUN(test_option_header_split_by_the_end,        "option header cut off by the end");
    RUN(test_udp_length_past_the_ip_packet,         "UDP length past the IP packet");
    RUN(test_reply_without_type_is_refused,         "reply with no message type");
    RUN(test_missing_server_id_falls_back_to_source, "missing server id uses the source");
    RUN(test_dns_list_is_capped,                    "DNS list capped at the array");
    RUN(test_arp_reply_to_a_request_for_us,         "ARP request for us is answered");
    RUN(test_arp_ignores_requests_for_others,       "ARP requests for others ignored");
    RUN(test_arp_refuses_short_and_foreign_packets, "short and non-Ethernet ARP refused");
}
