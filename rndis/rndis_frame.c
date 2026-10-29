/*
 * rndis_frame.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * The arithmetic below is done in uint64_t although every field is 32 bits,
 * and that is the point. msg_len, data_offset and data_len come from the
 * device, and the obvious 32-bit `offset + msg_len > size` wraps for values
 * near UINT32_MAX and passes a check it should fail. Widening first means
 * the comparison cannot wrap.
 */

#include "rndis_frame.h"

#include <string.h>

rndis_parse_result_t
rndis_parse_transfer(const uint8_t *buf, uint32_t size,
                     rndis_frame_t *out, uint32_t max_out,
                     uint32_t max_frame_len, uint32_t *found)
{
    const uint64_t header_len = sizeof(struct rndis_packet_msg);
    uint64_t offset = 0;

    *found = 0;

    if (buf == NULL || out == NULL || max_out == 0) {
        return RNDIS_PARSE_OUT_OF_ROOM;
    }

    while (offset + header_len <= (uint64_t)size) {
        struct rndis_packet_msg header;

        /*
         * memcpy rather than a cast: a transfer buffer carries no alignment
         * guarantee, and a misaligned load is undefined even where the
         * hardware would tolerate it.
         */
        memcpy(&header, buf + offset, sizeof(header));

        if (rndis_le32_to_cpu(header.msg_type) != RNDIS_MSG_PACKET) {
            return RNDIS_PARSE_NOT_PACKET;
        }

        const uint64_t msg_len     = rndis_le32_to_cpu(header.msg_len);
        const uint64_t data_offset = rndis_le32_to_cpu(header.data_offset);
        const uint64_t data_len    = rndis_le32_to_cpu(header.data_len);

        /* The record has to fit in what is left of the transfer. */
        if (msg_len < header_len || offset + msg_len > (uint64_t)size) {
            return RNDIS_PARSE_BAD_MSG_LEN;
        }

        /* An empty record is not a frame. Treat it as corruption rather
         * than handing a zero-length packet to the network stack. */
        if (data_len == 0) {
            return RNDIS_PARSE_BAD_PAYLOAD;
        }

        /*
         * The frame must lie inside its own record, after the header. The
         * offset is measured from the request_id field; see
         * RNDIS_INFO_OFFSET_BASE.
         */
        const uint64_t frame_start = offset + RNDIS_INFO_OFFSET_BASE + data_offset;
        if (frame_start < offset + header_len
                || frame_start + data_len > offset + msg_len) {
            return RNDIS_PARSE_BAD_PAYLOAD;
        }

        if (data_len > (uint64_t)max_frame_len) {
            return RNDIS_PARSE_FRAME_TOO_BIG;
        }

        if (*found >= max_out) {
            return RNDIS_PARSE_OUT_OF_ROOM;
        }
        out[*found].offset = (uint32_t)frame_start;
        out[*found].length = (uint32_t)data_len;
        (*found)++;

        offset += msg_len;
    }

    /*
     * Trailing bytes too short to hold a record. Devices pad a transfer up
     * to the endpoint's packet size, so a short tail is expected rather
     * than alarming; the caller decides whether to care.
     */
    return (offset == (uint64_t)size) ? RNDIS_PARSE_OK : RNDIS_PARSE_SHORT_TAIL;
}

size_t
rndis_build_packet_header(uint8_t *dst, size_t dst_len, uint32_t frame_len)
{
    struct rndis_packet_msg header;

    if (dst == NULL || frame_len == 0) {
        return 0;
    }
    /* Header and frame both have to fit. Checked without adding them first,
     * so a huge frame_len cannot wrap the sum. */
    if (dst_len < sizeof(header)
            || (uint64_t)(dst_len - sizeof(header)) < (uint64_t)frame_len) {
        return 0;
    }

    memset(&header, 0, sizeof(header));
    header.msg_type    = rndis_cpu_to_le32(RNDIS_MSG_PACKET);
    header.msg_len     = rndis_cpu_to_le32((uint32_t)(sizeof(header) + frame_len));
    header.data_offset = rndis_cpu_to_le32((uint32_t)(sizeof(header)
                                                      - RNDIS_INFO_OFFSET_BASE));
    header.data_len    = rndis_cpu_to_le32(frame_len);

    memcpy(dst, &header, sizeof(header));
    return sizeof(header);
}

const char *
rndis_parse_result_name(rndis_parse_result_t result)
{
    switch (result) {
    case RNDIS_PARSE_OK:            return "ok";
    case RNDIS_PARSE_SHORT_TAIL:    return "short tail";
    case RNDIS_PARSE_NOT_PACKET:    return "not a data packet";
    case RNDIS_PARSE_BAD_MSG_LEN:   return "bad msg_len";
    case RNDIS_PARSE_BAD_PAYLOAD:   return "bad data_offset or data_len";
    case RNDIS_PARSE_FRAME_TOO_BIG: return "frame too big";
    case RNDIS_PARSE_OUT_OF_ROOM:   return "no room for more frames";
    }
    return "unknown";
}
