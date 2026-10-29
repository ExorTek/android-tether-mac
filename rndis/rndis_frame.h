/*
 * rndis_frame.h -- carrying Ethernet frames inside RNDIS data records.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * REMOTE_NDIS_PACKET_MSG, [MS-RNDIS] 2.2.1.
 *
 * This is the part of the driver a device can attack. msg_len, data_offset
 * and data_len all arrive from the other end of the cable, and a mistake
 * here is a read past the end of a buffer rather than a slow interface. It
 * lives in plain C so the tests can feed it the shapes real hardware will
 * not produce on request.
 */

#ifndef RNDIS_FRAME_H
#define RNDIS_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "rndis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where one Ethernet frame sits inside a received transfer. */
typedef struct {
    uint32_t offset;        /* from the start of the transfer buffer */
    uint32_t length;
} rndis_frame_t;

/* Why a walk over a transfer stopped. Distinct values so a log can tell a
 * malformed device apart from a full output array. */
typedef enum {
    RNDIS_PARSE_OK = 0,         /* the transfer was consumed exactly */
    RNDIS_PARSE_SHORT_TAIL,     /* trailing bytes too short for a record */
    RNDIS_PARSE_NOT_PACKET,     /* a record that is not a data packet */
    RNDIS_PARSE_BAD_MSG_LEN,    /* msg_len disagrees with the transfer */
    RNDIS_PARSE_BAD_PAYLOAD,    /* the frame is not inside its own record */
    RNDIS_PARSE_FRAME_TOO_BIG,  /* the frame exceeds the receive buffer */
    RNDIS_PARSE_OUT_OF_ROOM     /* more records than the caller can hold */
} rndis_parse_result_t;

/*
 * Walk one bulk IN transfer and record where each Ethernet frame starts.
 *
 * Nothing is copied: the caller gets offsets into its own buffer. A
 * transfer may carry several records back to back, so this loops rather
 * than assuming one frame per transfer -- a driver that assumes one drops
 * the rest silently, which looks like packet loss.
 *
 * Parsing stops at the first record that does not make sense, and the
 * frames found before it are still reported: a device that corrupts its
 * fifth record has still delivered four good frames.
 *
 *   buf, size       the transfer exactly as it came off the pipe
 *   out, max_out    where to record what was found
 *   max_frame_len   the receive buffer size; a larger frame is refused
 *   found           number of entries written to out; never NULL
 */
rndis_parse_result_t rndis_parse_transfer(const uint8_t *buf, uint32_t size,
                                          rndis_frame_t *out, uint32_t max_out,
                                          uint32_t max_frame_len,
                                          uint32_t *found);

/*
 * Write the data-record header for one outgoing frame at the start of dst.
 *
 * Returns the header length, or 0 if the frame length or the buffer will
 * not do. It refuses rather than truncating, because a truncated RNDIS
 * record is not a short frame -- it is a different message.
 */
size_t rndis_build_packet_header(uint8_t *dst, size_t dst_len,
                                 uint32_t frame_len);

/* A short name for a parse result, for log lines. */
const char *rndis_parse_result_name(rndis_parse_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* RNDIS_FRAME_H */
