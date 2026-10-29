/*
 * rndis.h -- the RNDIS wire protocol.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * Written from [MS-RNDIS]: Remote Network Driver Interface Specification
 * (RNDIS) Protocol, and from Microsoft's "Remote NDIS To USB Mapping" for
 * the parts specific to carrying RNDIS over USB.
 *
 * Deliberately free of platform headers -- no IOKit, no DriverKit, no
 * kernel. Everything here is wire format and arithmetic, so it compiles and
 * is tested on any host. That matters more here than anywhere else in the
 * project: every field in a received message is chosen by the device at the
 * other end of the cable.
 */

#ifndef RNDIS_H
#define RNDIS_H

#include <stdint.h>

/*
 * RNDIS is little-endian on the wire. Every platform this targets is
 * little-endian, so the conversions are the identity; they exist to mark
 * which values are wire-order, and the check below keeps that honest.
 */
#if defined(__BIG_ENDIAN__)
#error "the RNDIS byte-order helpers assume a little-endian host"
#endif
#define rndis_cpu_to_le32(x) ((uint32_t)(x))
#define rndis_le32_to_cpu(x) ((uint32_t)(x))

/* ---------------------------------------------------------------------
 * Message types. [MS-RNDIS] 2.2.
 *
 * A response carries the request's type with the high bit set, which is
 * what RNDIS_MSG_COMPLETION expresses.
 * ------------------------------------------------------------------- */

#define RNDIS_MSG_COMPLETION        0x80000000u

#define RNDIS_MSG_PACKET            0x00000001u
#define RNDIS_MSG_INITIALIZE        0x00000002u
#define RNDIS_MSG_HALT              0x00000003u
#define RNDIS_MSG_QUERY             0x00000004u
#define RNDIS_MSG_SET               0x00000005u
#define RNDIS_MSG_RESET             0x00000006u
#define RNDIS_MSG_INDICATE_STATUS   0x00000007u
#define RNDIS_MSG_KEEPALIVE         0x00000008u

#define RNDIS_MSG_INITIALIZE_CMPLT  (RNDIS_MSG_INITIALIZE | RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_QUERY_CMPLT       (RNDIS_MSG_QUERY      | RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_SET_CMPLT         (RNDIS_MSG_SET        | RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_RESET_CMPLT       (RNDIS_MSG_RESET      | RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_KEEPALIVE_CMPLT   (RNDIS_MSG_KEEPALIVE  | RNDIS_MSG_COMPLETION)

/* ---------------------------------------------------------------------
 * Status codes. [MS-RNDIS] 2.2.1.
 * ------------------------------------------------------------------- */

#define RNDIS_STATUS_SUCCESS        0x00000000u
#define RNDIS_STATUS_FAILURE        0xC0000001u
#define RNDIS_STATUS_INVALID_DATA   0xC0010015u
#define RNDIS_STATUS_NOT_SUPPORTED  0xC00000BBu
#define RNDIS_STATUS_MEDIA_CONNECT  0x4001000Bu
#define RNDIS_STATUS_MEDIA_DISCONNECT 0x4001000Cu

/* ---------------------------------------------------------------------
 * OIDs used by this driver. NDIS object identifiers, as referenced by
 * [MS-RNDIS] for REMOTE_NDIS_QUERY_MSG and REMOTE_NDIS_SET_MSG.
 * ------------------------------------------------------------------- */

#define OID_GEN_MAXIMUM_FRAME_SIZE      0x00010106u
#define OID_GEN_CURRENT_PACKET_FILTER   0x0001010Eu
#define OID_GEN_PHYSICAL_MEDIUM         0x00010202u
#define OID_802_3_PERMANENT_ADDRESS     0x01010101u
#define OID_802_3_CURRENT_ADDRESS       0x01010102u

/* Packet-filter bits for OID_GEN_CURRENT_PACKET_FILTER. */
#define RNDIS_PACKET_TYPE_DIRECTED      0x00000001u
#define RNDIS_PACKET_TYPE_MULTICAST     0x00000002u
#define RNDIS_PACKET_TYPE_ALL_MULTICAST 0x00000004u
#define RNDIS_PACKET_TYPE_BROADCAST     0x00000008u
#define RNDIS_PACKET_TYPE_PROMISCUOUS   0x00000020u

/*
 * What we ask a device to deliver. Directed and broadcast are the minimum
 * for IP to work; all-multicast covers neighbour discovery and mDNS without
 * maintaining a filter list the device is free to ignore anyway.
 */
#define RNDIS_FILTER_DEFAULT ( \
        RNDIS_PACKET_TYPE_DIRECTED      | \
        RNDIS_PACKET_TYPE_BROADCAST     | \
        RNDIS_PACKET_TYPE_ALL_MULTICAST)

/* ---------------------------------------------------------------------
 * Control channel. "Remote NDIS To USB Mapping": control messages travel
 * as class requests on endpoint 0, addressed to the communication
 * interface.
 * ------------------------------------------------------------------- */

#define RNDIS_SEND_ENCAPSULATED_COMMAND 0x00
#define RNDIS_GET_ENCAPSULATED_RESPONSE 0x01

/* The notification a device sends on the interrupt endpoint to say a
 * response is waiting to be collected. */
#define RNDIS_NOTIFY_RESPONSE_AVAILABLE 0x00000001u

/*
 * The control buffer size. "Remote NDIS To USB Mapping" gives 1024 bytes as
 * the minimum a host must be able to receive, and no message this driver
 * sends comes close to it.
 */
#define RNDIS_CONTROL_BUFFER_SIZE   1024u

/*
 * The largest transfer we invite a device to make in one go. [MS-RNDIS]
 * says MaxTransferSize SHOULD be 0x4000 for REMOTE_NDIS_INITIALIZE_MSG, and
 * some devices use that figure regardless of what the host asks for, so the
 * receive buffer has to be at least this big either way.
 */
#define RNDIS_MAX_TRANSFER_SIZE     0x4000u

/* Largest payload in a standard Ethernet frame, and the largest frame
 * including its header. */
#define RNDIS_ETHERNET_MTU          1500u
#define RNDIS_ETHERNET_MAX_FRAME    1514u
#define RNDIS_ETHERNET_ADDR_LEN     6u
#define RNDIS_ETHERNET_HEADER_LEN   14u

/* ---------------------------------------------------------------------
 * Message layouts. [MS-RNDIS] 2.2. Every field is a 32-bit little-endian
 * integer, and the structures are packed because they describe bytes on a
 * wire rather than a convenient layout in memory.
 * ------------------------------------------------------------------- */

/* Common prefix of every control message and of every completion. */
struct rndis_header {
    uint32_t msg_type;
    uint32_t msg_len;
} __attribute__((packed));

/* REMOTE_NDIS_PACKET_MSG, 2.2.1. Wraps one Ethernet frame. */
struct rndis_packet_msg {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t data_offset;
    uint32_t data_len;
    uint32_t oob_data_offset;
    uint32_t oob_data_len;
    uint32_t num_oob_data_elements;
    uint32_t per_packet_info_offset;
    uint32_t per_packet_info_len;
    uint32_t reserved_1;
    uint32_t reserved_2;
} __attribute__((packed));

/* REMOTE_NDIS_INITIALIZE_MSG, 2.2.2. */
struct rndis_initialize_msg {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t max_transfer_size;
} __attribute__((packed));

/* REMOTE_NDIS_INITIALIZE_CMPLT, 2.2.3. */
struct rndis_initialize_cmplt {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t status;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t device_flags;
    uint32_t medium;
    uint32_t max_packets_per_transfer;
    uint32_t max_transfer_size;
    uint32_t packet_alignment_factor;
    uint32_t af_list_offset;
    uint32_t af_list_size;
} __attribute__((packed));

/* REMOTE_NDIS_QUERY_MSG, 2.2.9. */
struct rndis_query_msg {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t oid;
    uint32_t info_buffer_length;
    uint32_t info_buffer_offset;
    uint32_t device_vc_handle;
} __attribute__((packed));

/* REMOTE_NDIS_QUERY_CMPLT, 2.2.10. */
struct rndis_query_cmplt {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t status;
    uint32_t info_buffer_length;
    uint32_t info_buffer_offset;
} __attribute__((packed));

/* REMOTE_NDIS_SET_MSG, 2.2.11. */
struct rndis_set_msg {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t oid;
    uint32_t info_buffer_length;
    uint32_t info_buffer_offset;
    uint32_t device_vc_handle;
} __attribute__((packed));

/* REMOTE_NDIS_SET_CMPLT, 2.2.12. */
struct rndis_set_cmplt {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t status;
} __attribute__((packed));

/* REMOTE_NDIS_HALT_MSG, 2.2.13. */
struct rndis_halt_msg {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
} __attribute__((packed));

/*
 * The info buffer offsets above are measured from the start of the
 * request_id field, which is 8 bytes into the message -- not from the start
 * of the message. Getting this wrong by 8 is the classic RNDIS mistake: it
 * shifts every payload and fails in ways that look like a device bug.
 */
#define RNDIS_INFO_OFFSET_BASE 8u

#endif /* RNDIS_H */
