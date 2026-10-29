/*
 * test_frame.c -- the data path, fed what real hardware will not send.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * Every length and offset rndis_parse_transfer reads was chosen by the
 * device, so the cases that matter are the hostile ones -- and those are
 * exactly the cases nobody can produce by plugging in a phone.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "rndis_frame.h"
#include "test_util.h"

#define HDR_LEN ((uint32_t)sizeof(struct rndis_packet_msg))

/* A well-formed record at buf+at carrying frame_len bytes of `fill`. */
static uint32_t
put_record(uint8_t *buf, uint32_t at, uint32_t frame_len, uint8_t fill)
{
    struct rndis_packet_msg header;

    memset(&header, 0, sizeof(header));
    header.msg_type    = rndis_cpu_to_le32(RNDIS_MSG_PACKET);
    header.msg_len     = rndis_cpu_to_le32(HDR_LEN + frame_len);
    header.data_offset = rndis_cpu_to_le32(HDR_LEN - RNDIS_INFO_OFFSET_BASE);
    header.data_len    = rndis_cpu_to_le32(frame_len);
    memcpy(buf + at, &header, sizeof(header));
    memset(buf + at + HDR_LEN, fill, frame_len);
    return HDR_LEN + frame_len;
}

static void
poke(uint8_t *buf, uint32_t at, size_t field_offset, uint32_t value)
{
    const uint32_t wire = rndis_cpu_to_le32(value);
    memcpy(buf + at + field_offset, &wire, sizeof(wire));
}

#define OFF_TYPE   offsetof(struct rndis_packet_msg, msg_type)
#define OFF_LEN    offsetof(struct rndis_packet_msg, msg_len)
#define OFF_DOFF   offsetof(struct rndis_packet_msg, data_offset)
#define OFF_DLEN   offsetof(struct rndis_packet_msg, data_len)

/* ---------------------------------------------------------------- */

static void
test_header_layout(void)
{
    /* [MS-RNDIS] fixes this. If the struct ever gains padding, every
     * offset computed from it is wrong and nothing else here means
     * anything, so check it before checking behaviour. */
    CHECK(HDR_LEN == 44, "rndis_packet_msg is %u bytes, expected 44", HDR_LEN);
    CHECK(OFF_TYPE == 0,  "msg_type at %zu", (size_t)OFF_TYPE);
    CHECK(OFF_LEN == 4,   "msg_len at %zu", (size_t)OFF_LEN);
    CHECK(OFF_DOFF == 8,  "data_offset at %zu", (size_t)OFF_DOFF);
    CHECK(OFF_DLEN == 12, "data_len at %zu", (size_t)OFF_DLEN);
}

static void
test_single_frame(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    const uint32_t total = put_record(buf, 0, 100, 0xAB);
    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_OK, "result %s", rndis_parse_result_name(r));
    CHECK(found == 1, "found %u frames, expected 1", found);
    CHECK(frames[0].length == 100, "length %u", frames[0].length);
    CHECK(frames[0].offset == HDR_LEN, "offset %u, expected %u",
          frames[0].offset, HDR_LEN);
    CHECK(buf[frames[0].offset] == 0xAB, "offset does not point at the payload");
}

static void
test_several_frames(void)
{
    uint8_t buf[4096];
    rndis_frame_t frames[8];
    uint32_t found = 0;
    uint32_t at = 0;

    /* One transfer may carry several records. A driver that assumes one
     * drops the rest, which looks like packet loss rather than a bug. */
    at += put_record(buf, at, 64,   0x11);
    at += put_record(buf, at, 1514, 0x22);
    at += put_record(buf, at, 128,  0x33);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, at, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_OK, "result %s", rndis_parse_result_name(r));
    CHECK(found == 3, "found %u frames, expected 3", found);
    CHECK(frames[0].length == 64 && frames[1].length == 1514
          && frames[2].length == 128, "lengths %u/%u/%u",
          frames[0].length, frames[1].length, frames[2].length);
    CHECK(buf[frames[1].offset] == 0x22, "second frame points at the wrong bytes");
    CHECK(buf[frames[2].offset] == 0x33, "third frame points at the wrong bytes");
}

static void
test_padded_tail(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    /* Devices pad up to the endpoint packet size. The frame before the
     * padding must still be delivered. */
    const uint32_t total = put_record(buf, 0, 60, 0x44);
    memset(buf + total, 0, 7);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total + 7, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_SHORT_TAIL, "result %s", rndis_parse_result_name(r));
    CHECK(found == 1, "found %u frames, expected 1", found);
}

static void
test_good_frames_survive_a_bad_record(void)
{
    uint8_t buf[4096];
    rndis_frame_t frames[8];
    uint32_t found = 0;
    uint32_t at = 0;

    at += put_record(buf, at, 100, 0x55);
    at += put_record(buf, at, 100, 0x66);
    const uint32_t third = at;
    at += put_record(buf, at, 100, 0x77);
    poke(buf, third, OFF_DLEN, 0xFFFFFFFFu);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, at, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r != RNDIS_PARSE_OK, "a corrupt record was accepted");
    CHECK(found == 2, "found %u frames, expected the 2 good ones", found);
}

/* ---- hostile ---- */

static void
test_truncated_header(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    put_record(buf, 0, 100, 0x88);

    /* Fewer bytes than a header: reading one anyway is an overread on the
     * very first transfer. */
    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, HDR_LEN - 1, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_SHORT_TAIL, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "found %u frames in a truncated header", found);
}

static void
test_msg_len_past_the_transfer(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    const uint32_t total = put_record(buf, 0, 100, 0x99);
    poke(buf, 0, OFF_LEN, total + 1000);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_BAD_MSG_LEN, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "accepted a record longer than the transfer");
}

static void
test_msg_len_shorter_than_a_header(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    const uint32_t total = put_record(buf, 0, 100, 0xAA);
    poke(buf, 0, OFF_LEN, 4);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    /* Also the guard against advancing the walk by zero and spinning. */
    CHECK(r == RNDIS_PARSE_BAD_MSG_LEN, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "accepted an impossibly short record");
}

static void
test_data_offset_past_the_record(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    const uint32_t total = put_record(buf, 0, 100, 0xBB);
    poke(buf, 0, OFF_DOFF, 10000);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_BAD_PAYLOAD, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "accepted a frame starting outside its record");
}

static void
test_offset_length_overflow(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    /* Why the implementation widens to 64 bits: in 32-bit arithmetic
     * data_offset + data_len wraps to a small number, sails through a naive
     * bounds check, and points the copy far outside the buffer. */
    const uint32_t total = put_record(buf, 0, 100, 0xCC);
    poke(buf, 0, OFF_DOFF, 0xFFFFFFF0u);
    poke(buf, 0, OFF_DLEN, 0x20u);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_BAD_PAYLOAD, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "an integer overflow got past the bounds check");
}

static void
test_frame_overlapping_its_header(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    /* Not memory-unsafe, but handing header bytes to the network stack as
     * Ethernet is its own kind of wrong. */
    const uint32_t total = put_record(buf, 0, 100, 0xDD);
    poke(buf, 0, OFF_DOFF, 0);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_BAD_PAYLOAD, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "accepted a frame overlapping its own header");
}

static void
test_zero_length_frame(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    const uint32_t total = put_record(buf, 0, 100, 0xEE);
    poke(buf, 0, OFF_DLEN, 0);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_BAD_PAYLOAD, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "accepted an empty frame");
}

static void
test_frame_larger_than_the_buffer(void)
{
    uint8_t buf[8192];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    /* Internally consistent, just bigger than we can receive into. This is
     * what stops a jumbo frame overrunning a pool buffer. */
    const uint32_t total = put_record(buf, 0, 4000, 0x0F);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_FRAME_TOO_BIG, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "accepted a frame larger than the receive buffer");
}

static void
test_control_message_on_the_data_pipe(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[8];
    uint32_t found = 0;

    const uint32_t total = put_record(buf, 0, 100, 0x10);
    poke(buf, 0, OFF_TYPE, RNDIS_MSG_INITIALIZE);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, total, frames, 8, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_NOT_PACKET, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "accepted a control message as a data frame");
}

static void
test_output_array_is_respected(void)
{
    uint8_t buf[4096];
    rndis_frame_t frames[2];
    uint32_t found = 0;
    uint32_t at = 0;

    for (int i = 0; i < 5; i++) {
        at += put_record(buf, at, 64, (uint8_t)(0x20 + i));
    }

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, at, frames, 2, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_OUT_OF_ROOM, "result %s", rndis_parse_result_name(r));
    CHECK(found == 2, "wrote %u entries into an array of 2", found);
}

static void
test_empty_transfer(void)
{
    uint8_t buf[64];
    rndis_frame_t frames[4];
    uint32_t found = 0;

    memset(buf, 0, sizeof(buf));
    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, 0, frames, 4, RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_OK, "result %s", rndis_parse_result_name(r));
    CHECK(found == 0, "found %u frames in an empty transfer", found);
}

/* ---- transmit ---- */

static void
test_build_parse_round_trip(void)
{
    uint8_t buf[2048];
    rndis_frame_t frames[4];
    uint32_t found = 0;

    /* What we build must be what we can parse. If the two ever disagree,
     * every frame we transmit is malformed and nothing on this side would
     * say so. */
    const size_t hdr = rndis_build_packet_header(buf, sizeof(buf), 500);
    CHECK(hdr == HDR_LEN, "built a %zu-byte header", hdr);
    memset(buf + hdr, 0x5A, 500);

    const rndis_parse_result_t r =
        rndis_parse_transfer(buf, (uint32_t)(hdr + 500), frames, 4,
                             RNDIS_ETHERNET_MAX_FRAME, &found);

    CHECK(r == RNDIS_PARSE_OK, "result %s", rndis_parse_result_name(r));
    CHECK(found == 1, "found %u frames", found);
    CHECK(frames[0].length == 500, "length %u", frames[0].length);
    CHECK(frames[0].offset == hdr, "offset %u", frames[0].offset);
}

static void
test_build_refuses_bad_sizes(void)
{
    uint8_t buf[128];

    CHECK(rndis_build_packet_header(buf, sizeof(buf), 0) == 0,
          "built a header for an empty frame");
    CHECK(rndis_build_packet_header(buf, HDR_LEN - 1, 10) == 0,
          "built a header into a buffer too small to hold it");
    CHECK(rndis_build_packet_header(buf, sizeof(buf), 1000) == 0,
          "built a header for a frame that does not fit");
    CHECK(rndis_build_packet_header(buf, sizeof(buf), 0xFFFFFFFFu) == 0,
          "a huge frame length got past the fit check");
    CHECK(rndis_build_packet_header(NULL, sizeof(buf), 10) == 0,
          "wrote through a NULL destination");
}

/* ---------------------------------------------------------------- */

void
run_frame_tests(void)
{
    RUN(test_header_layout,                  "header layout");
    RUN(test_single_frame,                   "single frame");
    RUN(test_several_frames,                 "several frames in one transfer");
    RUN(test_padded_tail,                    "padded tail");
    RUN(test_good_frames_survive_a_bad_record, "good frames kept before a bad one");
    RUN(test_truncated_header,               "truncated header");
    RUN(test_msg_len_past_the_transfer,      "msg_len past the transfer");
    RUN(test_msg_len_shorter_than_a_header,  "msg_len shorter than a header");
    RUN(test_data_offset_past_the_record,    "data_offset past the record");
    RUN(test_offset_length_overflow,         "offset+length overflow");
    RUN(test_frame_overlapping_its_header,   "frame overlapping its header");
    RUN(test_zero_length_frame,              "zero-length frame");
    RUN(test_frame_larger_than_the_buffer,   "frame larger than the buffer");
    RUN(test_control_message_on_the_data_pipe, "control message on the data pipe");
    RUN(test_output_array_is_respected,      "output array respected");
    RUN(test_empty_transfer,                 "empty transfer");
    RUN(test_build_parse_round_trip,         "build/parse round trip");
    RUN(test_build_refuses_bad_sizes,        "build refuses bad sizes");
}
