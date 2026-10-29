/*
 * test_control.c -- the handshake, exercised without a phone.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ExorTek
 *
 * The control channel decides whether an interface ever appears. Keeping
 * it in pure C means the whole handshake can be checked here, including
 * the replies a cooperative device would never send.
 */

#include <stddef.h>
#include <string.h>

#include "rndis_control.h"
#include "test_util.h"

/* Build a completion of `sent_type` in buf and return its length. */
static uint32_t
put_completion(uint8_t *buf, size_t buf_len, uint32_t sent_type,
               uint32_t request_id, uint32_t status, uint32_t msg_len)
{
    const uint32_t fields[4] = {
        rndis_cpu_to_le32(sent_type | RNDIS_MSG_COMPLETION),
        rndis_cpu_to_le32(msg_len),
        rndis_cpu_to_le32(request_id),
        rndis_cpu_to_le32(status),
    };
    memset(buf, 0, buf_len);
    memcpy(buf, fields, sizeof(fields));
    return msg_len;
}

static uint32_t
read_u32(const uint8_t *buf, size_t offset)
{
    uint32_t value;
    memcpy(&value, buf + offset, sizeof(value));
    return rndis_le32_to_cpu(value);
}

/* ---- builders ---- */

static void
test_initialize_is_well_formed(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];

    const size_t len = rndis_build_initialize(buf, sizeof(buf), 7,
                                              RNDIS_MAX_TRANSFER_SIZE);

    CHECK(len == sizeof(struct rndis_initialize_msg),
          "built %zu bytes", len);
    CHECK(read_u32(buf, offsetof(struct rndis_initialize_msg, msg_type))
          == RNDIS_MSG_INITIALIZE, "wrong msg_type");
    /* msg_len must describe the bytes actually sent, or a conformant
     * device decides the host is broken. */
    CHECK(read_u32(buf, offsetof(struct rndis_initialize_msg, msg_len))
          == (uint32_t)len, "msg_len disagrees with the length returned");
    CHECK(read_u32(buf, offsetof(struct rndis_initialize_msg, request_id)) == 7,
          "request id not carried");
    CHECK(read_u32(buf, offsetof(struct rndis_initialize_msg, major_version)) == 1
          && read_u32(buf, offsetof(struct rndis_initialize_msg, minor_version)) == 0,
          "version is not 1.0");
    CHECK(read_u32(buf, offsetof(struct rndis_initialize_msg, max_transfer_size))
          == RNDIS_MAX_TRANSFER_SIZE, "max transfer size not carried");
}

static void
test_query_is_well_formed(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];

    const size_t len = rndis_build_query(buf, sizeof(buf), 9,
                                         OID_802_3_PERMANENT_ADDRESS);

    CHECK(len == sizeof(struct rndis_query_msg), "built %zu bytes", len);
    CHECK(read_u32(buf, offsetof(struct rndis_query_msg, msg_type))
          == RNDIS_MSG_QUERY, "wrong msg_type");
    CHECK(read_u32(buf, offsetof(struct rndis_query_msg, oid))
          == OID_802_3_PERMANENT_ADDRESS, "wrong OID");
    CHECK(read_u32(buf, offsetof(struct rndis_query_msg, info_buffer_length)) == 0,
          "a query with no input should declare a zero-length info buffer");
}

static void
test_set_u32_places_its_value(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];

    const size_t len = rndis_build_set_u32(buf, sizeof(buf), 3,
                                           OID_GEN_CURRENT_PACKET_FILTER,
                                           RNDIS_FILTER_DEFAULT);

    CHECK(len == sizeof(struct rndis_set_msg) + 4, "built %zu bytes", len);
    CHECK(read_u32(buf, offsetof(struct rndis_set_msg, info_buffer_length)) == 4,
          "info buffer length is not 4");

    /*
     * The important one: info_buffer_offset is measured from request_id,
     * 8 bytes in, not from the start of the message. Off by 8 and the
     * device reads the wrong four bytes as the filter -- which looks like
     * a device that ignores the filter rather than a host bug.
     */
    const uint32_t offset =
        read_u32(buf, offsetof(struct rndis_set_msg, info_buffer_offset));
    CHECK(offset == sizeof(struct rndis_set_msg) - RNDIS_INFO_OFFSET_BASE,
          "info_buffer_offset is %u, expected %zu", offset,
          sizeof(struct rndis_set_msg) - RNDIS_INFO_OFFSET_BASE);
    CHECK(read_u32(buf, RNDIS_INFO_OFFSET_BASE + offset) == RNDIS_FILTER_DEFAULT,
          "the value is not where the offset says it is");
}

static void
test_builders_refuse_small_buffers(void)
{
    uint8_t buf[8];

    CHECK(rndis_build_initialize(buf, sizeof(buf), 1, 0x4000) == 0,
          "built an INITIALIZE into 8 bytes");
    CHECK(rndis_build_query(buf, sizeof(buf), 1, OID_GEN_PHYSICAL_MEDIUM) == 0,
          "built a QUERY into 8 bytes");
    CHECK(rndis_build_set_u32(buf, sizeof(buf), 1, OID_GEN_CURRENT_PACKET_FILTER, 0) == 0,
          "built a SET into 8 bytes");
    CHECK(rndis_build_halt(buf, sizeof(buf), 1) == 0, "built a HALT into 8 bytes");
    CHECK(rndis_build_initialize(NULL, 1024, 1, 0x4000) == 0,
          "wrote through a NULL destination");
}

/* ---- reply checking ---- */

static void
test_accepts_a_good_completion(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    const uint32_t len = put_completion(buf, sizeof(buf), RNDIS_MSG_SET, 5,
                                        RNDIS_STATUS_SUCCESS,
                                        (uint32_t)sizeof(struct rndis_set_cmplt));

    const rndis_reply_result_t r =
        rndis_check_completion(buf, len, RNDIS_MSG_SET, 5);
    CHECK(r == RNDIS_REPLY_OK, "result %s", rndis_reply_result_name(r));
}

static void
test_rejects_the_wrong_completion(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    /* An unsolicited INDICATE_STATUS can arrive on the control channel at
     * any time; mistaking one for our answer would make us act on fields
     * that mean something else entirely. */
    const uint32_t len = put_completion(buf, sizeof(buf), RNDIS_MSG_QUERY, 5,
                                        RNDIS_STATUS_SUCCESS, 24);

    const rndis_reply_result_t r =
        rndis_check_completion(buf, len, RNDIS_MSG_SET, 5);
    CHECK(r == RNDIS_REPLY_WRONG_TYPE, "result %s", rndis_reply_result_name(r));
}

static void
test_rejects_a_stale_request_id(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    /* The completion of a request that timed out earlier. Accepting it
     * pairs an answer with the wrong question for the rest of the session. */
    const uint32_t len = put_completion(buf, sizeof(buf), RNDIS_MSG_SET, 4,
                                        RNDIS_STATUS_SUCCESS, 16);

    const rndis_reply_result_t r =
        rndis_check_completion(buf, len, RNDIS_MSG_SET, 5);
    CHECK(r == RNDIS_REPLY_WRONG_ID, "result %s", rndis_reply_result_name(r));
}

static void
test_reports_a_device_failure(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    const uint32_t len = put_completion(buf, sizeof(buf), RNDIS_MSG_SET, 5,
                                        RNDIS_STATUS_FAILURE, 16);

    const rndis_reply_result_t r =
        rndis_check_completion(buf, len, RNDIS_MSG_SET, 5);
    CHECK(r == RNDIS_REPLY_DEVICE_ERROR, "result %s", rndis_reply_result_name(r));
}

static void
test_rejects_a_short_reply(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    put_completion(buf, sizeof(buf), RNDIS_MSG_SET, 5, RNDIS_STATUS_SUCCESS, 16);

    const rndis_reply_result_t r = rndis_check_completion(buf, 7, RNDIS_MSG_SET, 5);
    CHECK(r == RNDIS_REPLY_TOO_SHORT, "result %s", rndis_reply_result_name(r));
}

static void
test_rejects_msg_len_longer_than_the_reply(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    /* A device claiming a longer message than it sent is the shape of an
     * overread: trusting the larger figure is how you read past the end of
     * the control buffer. */
    put_completion(buf, sizeof(buf), RNDIS_MSG_SET, 5, RNDIS_STATUS_SUCCESS, 4096);

    const rndis_reply_result_t r = rndis_check_completion(buf, 16, RNDIS_MSG_SET, 5);
    CHECK(r == RNDIS_REPLY_BAD_MSG_LEN, "result %s", rndis_reply_result_name(r));
}

static void
test_initialize_reply_is_read(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    struct rndis_initialize_cmplt reply;
    rndis_init_reply_t out;

    memset(&reply, 0, sizeof(reply));
    reply.msg_type = rndis_cpu_to_le32(RNDIS_MSG_INITIALIZE_CMPLT);
    reply.msg_len  = rndis_cpu_to_le32((uint32_t)sizeof(reply));
    reply.request_id = rndis_cpu_to_le32(11);
    reply.status = rndis_cpu_to_le32(RNDIS_STATUS_SUCCESS);
    reply.major_version = rndis_cpu_to_le32(1);
    reply.minor_version = rndis_cpu_to_le32(0);
    reply.max_transfer_size = rndis_cpu_to_le32(0x4000);
    reply.max_packets_per_transfer = rndis_cpu_to_le32(3);
    reply.packet_alignment_factor = rndis_cpu_to_le32(2);
    memcpy(buf, &reply, sizeof(reply));

    const rndis_reply_result_t r =
        rndis_parse_initialize_reply(buf, (uint32_t)sizeof(reply), 11, &out);

    CHECK(r == RNDIS_REPLY_OK, "result %s", rndis_reply_result_name(r));
    CHECK(out.max_transfer_size == 0x4000, "max transfer size %u",
          out.max_transfer_size);
    CHECK(out.max_packets_per_transfer == 3, "max packets %u",
          out.max_packets_per_transfer);
    CHECK(out.packet_alignment_factor == 2, "alignment %u",
          out.packet_alignment_factor);
}

static void
test_initialize_reply_must_be_complete(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    rndis_init_reply_t out;

    /* A well-formed prefix is not enough: the fields we read live past it,
     * and a truncated reply would have us act on uninitialised bytes. */
    const uint32_t len = put_completion(buf, sizeof(buf), RNDIS_MSG_INITIALIZE,
                                        11, RNDIS_STATUS_SUCCESS, 16);

    const rndis_reply_result_t r =
        rndis_parse_initialize_reply(buf, len, 11, &out);
    CHECK(r == RNDIS_REPLY_TOO_SHORT, "result %s", rndis_reply_result_name(r));
}

static void
test_query_reply_returns_its_payload(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    struct rndis_query_cmplt reply;
    const uint8_t mac[RNDIS_ETHERNET_ADDR_LEN] = {2, 0, 0, 0, 0, 1};
    const uint32_t info_offset =
        (uint32_t)(sizeof(reply) - RNDIS_INFO_OFFSET_BASE);

    memset(buf, 0, sizeof(buf));
    memset(&reply, 0, sizeof(reply));
    reply.msg_type = rndis_cpu_to_le32(RNDIS_MSG_QUERY_CMPLT);
    reply.msg_len  = rndis_cpu_to_le32((uint32_t)(sizeof(reply) + sizeof(mac)));
    reply.request_id = rndis_cpu_to_le32(2);
    reply.status = rndis_cpu_to_le32(RNDIS_STATUS_SUCCESS);
    reply.info_buffer_length = rndis_cpu_to_le32((uint32_t)sizeof(mac));
    reply.info_buffer_offset = rndis_cpu_to_le32(info_offset);
    memcpy(buf, &reply, sizeof(reply));
    memcpy(buf + sizeof(reply), mac, sizeof(mac));

    const uint8_t *info = NULL;
    uint32_t info_len = 0;
    const rndis_reply_result_t r =
        rndis_parse_query_reply(buf, (uint32_t)(sizeof(reply) + sizeof(mac)),
                                2, &info, &info_len);

    CHECK(r == RNDIS_REPLY_OK, "result %s", rndis_reply_result_name(r));
    CHECK(info_len == sizeof(mac), "info length %u", info_len);
    CHECK(info != NULL && memcmp(info, mac, sizeof(mac)) == 0,
          "the payload is not where the offset says it is");
}

static void
test_query_reply_payload_must_be_inside_the_message(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    struct rndis_query_cmplt reply;

    /* An offset past the end of what arrived. This is the one that hands
     * the caller a pointer outside the buffer if it is not checked against
     * BOTH the claimed msg_len and the bytes actually received. */
    memset(buf, 0, sizeof(buf));
    memset(&reply, 0, sizeof(reply));
    reply.msg_type = rndis_cpu_to_le32(RNDIS_MSG_QUERY_CMPLT);
    reply.msg_len  = rndis_cpu_to_le32((uint32_t)sizeof(reply));
    reply.request_id = rndis_cpu_to_le32(2);
    reply.status = rndis_cpu_to_le32(RNDIS_STATUS_SUCCESS);
    reply.info_buffer_length = rndis_cpu_to_le32(64);
    reply.info_buffer_offset = rndis_cpu_to_le32(0xFFFFFF00u);
    memcpy(buf, &reply, sizeof(reply));

    const uint8_t *info = NULL;
    uint32_t info_len = 0;
    const rndis_reply_result_t r =
        rndis_parse_query_reply(buf, (uint32_t)sizeof(reply), 2, &info, &info_len);

    CHECK(r == RNDIS_REPLY_BAD_PAYLOAD, "result %s", rndis_reply_result_name(r));
}

static void
test_query_reply_payload_must_follow_the_header(void)
{
    uint8_t buf[RNDIS_CONTROL_BUFFER_SIZE];
    struct rndis_query_cmplt reply;

    /* An offset pointing back into the header. Not an overread, but the
     * bytes it returns are the message's own fields, not an answer. */
    memset(buf, 0, sizeof(buf));
    memset(&reply, 0, sizeof(reply));
    reply.msg_type = rndis_cpu_to_le32(RNDIS_MSG_QUERY_CMPLT);
    reply.msg_len  = rndis_cpu_to_le32((uint32_t)sizeof(reply));
    reply.request_id = rndis_cpu_to_le32(2);
    reply.status = rndis_cpu_to_le32(RNDIS_STATUS_SUCCESS);
    reply.info_buffer_length = rndis_cpu_to_le32(4);
    reply.info_buffer_offset = rndis_cpu_to_le32(0);
    memcpy(buf, &reply, sizeof(reply));

    const uint8_t *info = NULL;
    uint32_t info_len = 0;
    const rndis_reply_result_t r =
        rndis_parse_query_reply(buf, (uint32_t)sizeof(reply), 2, &info, &info_len);

    CHECK(r == RNDIS_REPLY_BAD_PAYLOAD, "result %s", rndis_reply_result_name(r));
}

/* ---------------------------------------------------------------- */

void
run_control_tests(void)
{
    RUN(test_initialize_is_well_formed,       "INITIALIZE is well formed");
    RUN(test_query_is_well_formed,            "QUERY is well formed");
    RUN(test_set_u32_places_its_value,        "SET places its value at its offset");
    RUN(test_builders_refuse_small_buffers,   "builders refuse small buffers");
    RUN(test_accepts_a_good_completion,       "a good completion is accepted");
    RUN(test_rejects_the_wrong_completion,    "the wrong completion is rejected");
    RUN(test_rejects_a_stale_request_id,      "a stale request id is rejected");
    RUN(test_reports_a_device_failure,        "a device failure is reported");
    RUN(test_rejects_a_short_reply,           "a short reply is rejected");
    RUN(test_rejects_msg_len_longer_than_the_reply,
                                              "msg_len longer than the reply");
    RUN(test_initialize_reply_is_read,        "INITIALIZE reply is read");
    RUN(test_initialize_reply_must_be_complete,
                                              "INITIALIZE reply must be complete");
    RUN(test_query_reply_returns_its_payload, "QUERY reply returns its payload");
    RUN(test_query_reply_payload_must_be_inside_the_message,
                                              "QUERY payload inside the message");
    RUN(test_query_reply_payload_must_follow_the_header,
                                              "QUERY payload after the header");
}
