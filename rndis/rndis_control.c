/*
 * rndis_control.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * As in rndis_frame.c, bounds arithmetic is widened to 64 bits before any
 * comparison: the lengths and offsets being checked were chosen by the
 * device, and 32-bit sums of attacker-chosen values wrap.
 */

#include "rndis_control.h"

#include <string.h>

/* ---------------------------------------------------------------- */
/* builders                                                          */
/* ---------------------------------------------------------------- */

size_t
rndis_build_initialize(uint8_t *dst, size_t dst_len,
                       uint32_t request_id, uint32_t max_transfer_size)
{
    struct rndis_initialize_msg msg;

    if (dst == NULL || dst_len < sizeof(msg)) {
        return 0;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = rndis_cpu_to_le32(RNDIS_MSG_INITIALIZE);
    msg.msg_len  = rndis_cpu_to_le32((uint32_t)sizeof(msg));
    msg.request_id = rndis_cpu_to_le32(request_id);
    /* RNDIS 1.0: the only version any of this targets. */
    msg.major_version = rndis_cpu_to_le32(1);
    msg.minor_version = rndis_cpu_to_le32(0);
    msg.max_transfer_size = rndis_cpu_to_le32(max_transfer_size);

    memcpy(dst, &msg, sizeof(msg));
    return sizeof(msg);
}

size_t
rndis_build_query(uint8_t *dst, size_t dst_len, uint32_t request_id, uint32_t oid)
{
    struct rndis_query_msg msg;

    if (dst == NULL || dst_len < sizeof(msg)) {
        return 0;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_type   = rndis_cpu_to_le32(RNDIS_MSG_QUERY);
    msg.msg_len    = rndis_cpu_to_le32((uint32_t)sizeof(msg));
    msg.request_id = rndis_cpu_to_le32(request_id);
    msg.oid        = rndis_cpu_to_le32(oid);
    /* No input buffer. [MS-RNDIS] has the offset as reserved when the
     * length is zero; send zero rather than a stale value. */
    msg.info_buffer_length = rndis_cpu_to_le32(0);
    msg.info_buffer_offset = rndis_cpu_to_le32(0);

    memcpy(dst, &msg, sizeof(msg));
    return sizeof(msg);
}

size_t
rndis_build_set_u32(uint8_t *dst, size_t dst_len,
                    uint32_t request_id, uint32_t oid, uint32_t value)
{
    struct rndis_set_msg msg;
    const size_t total = sizeof(msg) + sizeof(uint32_t);

    if (dst == NULL || dst_len < total) {
        return 0;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_type   = rndis_cpu_to_le32(RNDIS_MSG_SET);
    msg.msg_len    = rndis_cpu_to_le32((uint32_t)total);
    msg.request_id = rndis_cpu_to_le32(request_id);
    msg.oid        = rndis_cpu_to_le32(oid);
    msg.info_buffer_length = rndis_cpu_to_le32((uint32_t)sizeof(uint32_t));
    /* The value sits immediately after the header, and the offset is
     * measured from request_id rather than from the start of the message. */
    msg.info_buffer_offset = rndis_cpu_to_le32((uint32_t)(sizeof(msg)
                                                          - RNDIS_INFO_OFFSET_BASE));

    memcpy(dst, &msg, sizeof(msg));
    const uint32_t wire_value = rndis_cpu_to_le32(value);
    memcpy(dst + sizeof(msg), &wire_value, sizeof(wire_value));
    return total;
}

size_t
rndis_build_halt(uint8_t *dst, size_t dst_len, uint32_t request_id)
{
    struct rndis_halt_msg msg;

    if (dst == NULL || dst_len < sizeof(msg)) {
        return 0;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_type   = rndis_cpu_to_le32(RNDIS_MSG_HALT);
    msg.msg_len    = rndis_cpu_to_le32((uint32_t)sizeof(msg));
    msg.request_id = rndis_cpu_to_le32(request_id);

    memcpy(dst, &msg, sizeof(msg));
    return sizeof(msg);
}

/* ---------------------------------------------------------------- */
/* reply checking                                                    */
/* ---------------------------------------------------------------- */

/* Every completion begins with type, length, request id and status. */
struct rndis_completion_prefix {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t status;
} __attribute__((packed));

rndis_reply_result_t
rndis_check_completion(const uint8_t *buf, uint32_t len,
                       uint32_t sent_type, uint32_t request_id)
{
    struct rndis_completion_prefix prefix;

    if (buf == NULL || len < sizeof(prefix)) {
        return RNDIS_REPLY_TOO_SHORT;
    }
    memcpy(&prefix, buf, sizeof(prefix));

    if (rndis_le32_to_cpu(prefix.msg_type) != (sent_type | RNDIS_MSG_COMPLETION)) {
        /* Could be an unsolicited INDICATE_STATUS, or the tail of an
         * earlier exchange. Either way it is not our answer. */
        return RNDIS_REPLY_WRONG_TYPE;
    }

    const uint64_t msg_len = rndis_le32_to_cpu(prefix.msg_len);
    if (msg_len < sizeof(prefix) || msg_len > (uint64_t)len) {
        /* A device claiming a longer message than it sent is the shape of
         * an overread, so refuse rather than trusting the larger figure. */
        return RNDIS_REPLY_BAD_MSG_LEN;
    }

    if (request_id != 0
            && rndis_le32_to_cpu(prefix.request_id) != request_id) {
        return RNDIS_REPLY_WRONG_ID;
    }

    if (rndis_le32_to_cpu(prefix.status) != RNDIS_STATUS_SUCCESS) {
        return RNDIS_REPLY_DEVICE_ERROR;
    }
    return RNDIS_REPLY_OK;
}

rndis_reply_result_t
rndis_parse_initialize_reply(const uint8_t *buf, uint32_t len,
                             uint32_t request_id, rndis_init_reply_t *out)
{
    struct rndis_initialize_cmplt reply;

    const rndis_reply_result_t common =
        rndis_check_completion(buf, len, RNDIS_MSG_INITIALIZE, request_id);
    if (common != RNDIS_REPLY_OK) {
        return common;
    }
    if (len < sizeof(reply)) {
        return RNDIS_REPLY_TOO_SHORT;
    }
    memcpy(&reply, buf, sizeof(reply));

    if (out != NULL) {
        out->major_version = rndis_le32_to_cpu(reply.major_version);
        out->minor_version = rndis_le32_to_cpu(reply.minor_version);
        out->max_transfer_size = rndis_le32_to_cpu(reply.max_transfer_size);
        out->max_packets_per_transfer =
            rndis_le32_to_cpu(reply.max_packets_per_transfer);
        out->packet_alignment_factor =
            rndis_le32_to_cpu(reply.packet_alignment_factor);
    }
    return RNDIS_REPLY_OK;
}

rndis_reply_result_t
rndis_parse_query_reply(const uint8_t *buf, uint32_t len, uint32_t request_id,
                        const uint8_t **info, uint32_t *info_len)
{
    struct rndis_query_cmplt reply;

    const rndis_reply_result_t common =
        rndis_check_completion(buf, len, RNDIS_MSG_QUERY, request_id);
    if (common != RNDIS_REPLY_OK) {
        return common;
    }
    if (len < sizeof(reply)) {
        return RNDIS_REPLY_TOO_SHORT;
    }
    memcpy(&reply, buf, sizeof(reply));

    const uint64_t msg_len = rndis_le32_to_cpu(reply.msg_len);
    const uint64_t offset  = rndis_le32_to_cpu(reply.info_buffer_offset);
    const uint64_t length  = rndis_le32_to_cpu(reply.info_buffer_length);
    const uint64_t start   = RNDIS_INFO_OFFSET_BASE + offset;

    /*
     * The information buffer has to lie inside the message the device
     * claims to have sent AND inside the bytes that actually arrived.
     * Checking only one of the two is how a reply that lies about its own
     * length hands the caller a pointer past the end of the buffer.
     */
    if (start < sizeof(reply)
            || start + length > msg_len
            || start + length > (uint64_t)len) {
        return RNDIS_REPLY_BAD_PAYLOAD;
    }

    if (info != NULL) {
        *info = buf + start;
    }
    if (info_len != NULL) {
        *info_len = (uint32_t)length;
    }
    return RNDIS_REPLY_OK;
}

const char *
rndis_reply_result_name(rndis_reply_result_t result)
{
    switch (result) {
    case RNDIS_REPLY_OK:           return "ok";
    case RNDIS_REPLY_TOO_SHORT:    return "reply too short";
    case RNDIS_REPLY_WRONG_TYPE:   return "not the completion we asked for";
    case RNDIS_REPLY_WRONG_ID:     return "completion for a different request";
    case RNDIS_REPLY_BAD_MSG_LEN:  return "msg_len disagrees with the reply";
    case RNDIS_REPLY_DEVICE_ERROR: return "device reported failure";
    case RNDIS_REPLY_BAD_PAYLOAD:  return "info buffer outside the message";
    }
    return "unknown";
}
