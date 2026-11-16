// The SDK's outgoing data stream as a receiver sees it. A camera still is sent
// as one byte stream whose header attributes bind it to one execution, to the
// one participant that asked for it; the Channel drops a frame whose attributes
// it cannot match. Every packet is encoded here exactly as the peer would put it
// on the data channel and decoded back with nanopb.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "data_stream_writer.h"
#include "protocol.h"
#include "utils.h"

// utils.c needs esp_random and the wall clock; the writer only needs these two.
int64_t get_unix_time_ms(void) { return 1763280000000LL; }
void generate_uuid(char out[37])
{
    static int next = 0;
    snprintf(out, 37, "00000000-0000-4000-8000-%012d", ++next);
}

enum { kMaxPackets = 16, kMaxBytes = 20000 };
static uint8_t sent[kMaxPackets][kMaxBytes];
static size_t sent_size[kMaxPackets];
static int sent_count = 0;
static bool fail_sends = false;

static bool capture(const livekit_pb_data_packet_t* packet, void* ctx)
{
    (void)ctx;
    if (fail_sends) return false;
    assert(sent_count < kMaxPackets);
    pb_ostream_t stream = pb_ostream_from_buffer(sent[sent_count], kMaxBytes);
    assert(pb_encode(&stream, LIVEKIT_PB_DATA_PACKET_FIELDS, packet));
    sent_size[sent_count++] = stream.bytes_written;
    return true;
}

typedef struct {
    char keys[8][64];
    char values[8][128];
    int count;
} attributes_t;

static bool decode_entry(pb_istream_t* stream, const pb_field_t* field, void** arg)
{
    (void)field;
    attributes_t* out = (attributes_t*)*arg;
    livekit_pb_data_stream_header_attributes_entry_t entry = LIVEKIT_PB_DATA_STREAM_HEADER_ATTRIBUTES_ENTRY_INIT_ZERO;
    if (!pb_decode(stream, LIVEKIT_PB_DATA_STREAM_HEADER_ATTRIBUTES_ENTRY_FIELDS, &entry)) return false;
    assert(out->count < 8 && entry.key && entry.value);
    snprintf(out->keys[out->count], 64, "%s", entry.key);
    snprintf(out->values[out->count], 128, "%s", entry.value);
    out->count++;
    pb_release(LIVEKIT_PB_DATA_STREAM_HEADER_ATTRIBUTES_ENTRY_FIELDS, &entry);
    return true;
}

// The header's attributes, read the way a full receiver would: the stream
// header field of the DataPacket, decoded with an attributes callback set.
static attributes_t header_attributes(int index)
{
    attributes_t out;
    memset(&out, 0, sizeof(out));
    pb_istream_t stream = pb_istream_from_buffer(sent[index], sent_size[index]);
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof = false;
    bool found = false;
    while (pb_decode_tag(&stream, &wire, &tag, &eof)) {
        if (tag != LIVEKIT_PB_DATA_PACKET_STREAM_HEADER_TAG) {
            assert(pb_skip_field(&stream, wire));
            continue;
        }
        pb_istream_t sub;
        assert(pb_make_string_substream(&stream, &sub));
        livekit_pb_data_stream_header_t header = LIVEKIT_PB_DATA_STREAM_HEADER_INIT_ZERO;
        header.attributes.funcs.decode = decode_entry;
        header.attributes.arg = &out;
        assert(pb_decode(&sub, LIVEKIT_PB_DATA_STREAM_HEADER_FIELDS, &header));
        assert(pb_close_string_substream(&stream, &sub));
        pb_release(LIVEKIT_PB_DATA_STREAM_HEADER_FIELDS, &header);
        found = true;
    }
    assert(eof && found);
    return out;
}

// The packet decoded as the SDK's own receive path decodes it: no callbacks,
// so the header's attributes are skipped and everything else still reads.
static livekit_pb_data_packet_t decoded(int index)
{
    livekit_pb_data_packet_t packet = LIVEKIT_PB_DATA_PACKET_INIT_ZERO;
    pb_istream_t stream = pb_istream_from_buffer(sent[index], sent_size[index]);
    assert(pb_decode(&stream, LIVEKIT_PB_DATA_PACKET_FIELDS, &packet));
    return packet;
}

static const char* attribute(const attributes_t* attributes, const char* key)
{
    for (int i = 0; i < attributes->count; i++) {
        if (strcmp(attributes->keys[i], key) == 0) return attributes->values[i];
    }
    return NULL;
}

static void addressed_to_one(const livekit_pb_data_packet_t* packet, const char* identity)
{
    assert(packet->destination_identities_count == 1);
    assert(strcmp(packet->destination_identities[0], identity) == 0);
}

static void test_byte_stream_with_attributes_reaches_one_participant(void)
{
    data_stream_writer_handle_t writer = NULL;
    data_stream_writer_options_t writer_options = {.send_packet = capture};
    assert(data_stream_writer_create(&writer, &writer_options) == DATA_STREAM_WRITER_ERR_NONE);

    const livekit_data_stream_attribute_t attributes[] = {
        {"execution_id", "exec:0123456789abcdef0123456789abcdef"},
        {"session_id", "conv-42"},
        {"policy_revision", "7"},
    };
    char identity[] = "agent-channel";
    const char* destinations[] = {identity};
    livekit_data_stream_options_t options = {
        .topic = "eidolon.camera.frame.v1",
        .is_text = false,
        .total_length = 32000,
        .has_total_length = true,
        .attributes = attributes,
        .attributes_count = 3,
        .destination_identities = destinations,
        .destination_identities_count = 1,
    };
    livekit_data_stream_handle_t stream = NULL;
    sent_count = 0;
    assert(data_stream_writer_open(writer, &options, &stream) == DATA_STREAM_WRITER_ERR_NONE);
    // The caller's identity buffer may go away once the stream is open.
    memset(identity, 'x', sizeof(identity) - 1);

    static uint8_t still[32000];
    for (size_t i = 0; i < sizeof(still); i++) still[i] = (uint8_t)(i * 7);
    assert(data_stream_writer_write(writer, stream, still, sizeof(still)) == DATA_STREAM_WRITER_ERR_NONE);
    assert(data_stream_writer_close(writer, stream) == DATA_STREAM_WRITER_ERR_NONE);

    // Header, three chunks of at most 15000 bytes, trailer.
    assert(sent_count == 5);
    const attributes_t read = header_attributes(0);
    assert(read.count == 3);
    assert(strcmp(attribute(&read, "execution_id"), "exec:0123456789abcdef0123456789abcdef") == 0);
    assert(strcmp(attribute(&read, "session_id"), "conv-42") == 0);
    assert(strcmp(attribute(&read, "policy_revision"), "7") == 0);

    size_t received = 0;
    char stream_id[37] = "";
    for (int i = 0; i < sent_count; i++) {
        livekit_pb_data_packet_t packet = decoded(i);
        addressed_to_one(&packet, "agent-channel");
        if (i == 0) {
            assert(packet.which_value == LIVEKIT_PB_DATA_PACKET_STREAM_HEADER_TAG);
            const livekit_pb_data_stream_header_t* header = packet.value.stream_header;
            assert(strcmp(header->topic, "eidolon.camera.frame.v1") == 0);
            assert(header->has_total_length && header->total_length == 32000);
            assert(header->which_content_header == LIVEKIT_PB_DATA_STREAM_HEADER_BYTE_HEADER_TAG);
            snprintf(stream_id, sizeof(stream_id), "%s", header->stream_id);
        } else if (i < sent_count - 1) {
            assert(packet.which_value == LIVEKIT_PB_DATA_PACKET_STREAM_CHUNK_TAG);
            const livekit_pb_data_stream_chunk_t* chunk = packet.value.stream_chunk;
            assert(strcmp(chunk->stream_id, stream_id) == 0);
            assert(chunk->chunk_index == (uint64_t)(i - 1));
            assert(chunk->content->size <= LIVEKIT_DATA_STREAM_CHUNK_SIZE);
            assert(memcmp(chunk->content->bytes, still + received, chunk->content->size) == 0);
            received += chunk->content->size;
        } else {
            assert(packet.which_value == LIVEKIT_PB_DATA_PACKET_STREAM_TRAILER_TAG);
            assert(strcmp(packet.value.stream_trailer->stream_id, stream_id) == 0);
        }
        pb_release(LIVEKIT_PB_DATA_PACKET_FIELDS, &packet);
    }
    assert(received == sizeof(still));
    assert(data_stream_writer_destroy(writer) == DATA_STREAM_WRITER_ERR_NONE);
}

static void test_stream_without_options_is_unchanged(void)
{
    data_stream_writer_handle_t writer = NULL;
    data_stream_writer_options_t writer_options = {.send_packet = capture};
    assert(data_stream_writer_create(&writer, &writer_options) == DATA_STREAM_WRITER_ERR_NONE);
    livekit_data_stream_options_t options = {.topic = "lk.chat", .is_text = true};
    livekit_data_stream_handle_t stream = NULL;
    sent_count = 0;
    assert(data_stream_writer_open(writer, &options, &stream) == DATA_STREAM_WRITER_ERR_NONE);
    assert(data_stream_writer_write(writer, stream, (const uint8_t*)"hello", 5) == DATA_STREAM_WRITER_ERR_NONE);
    assert(data_stream_writer_close(writer, stream) == DATA_STREAM_WRITER_ERR_NONE);
    assert(sent_count == 3);
    assert(header_attributes(0).count == 0);
    for (int i = 0; i < sent_count; i++) {
        livekit_pb_data_packet_t packet = decoded(i);
        assert(packet.destination_identities_count == 0);  // the whole room, as before
        pb_release(LIVEKIT_PB_DATA_PACKET_FIELDS, &packet);
    }
    assert(data_stream_writer_destroy(writer) == DATA_STREAM_WRITER_ERR_NONE);
}

static void test_invalid_options_and_failed_header_release_the_slot(void)
{
    data_stream_writer_handle_t writer = NULL;
    data_stream_writer_options_t writer_options = {.send_packet = capture};
    assert(data_stream_writer_create(&writer, &writer_options) == DATA_STREAM_WRITER_ERR_NONE);
    livekit_data_stream_handle_t stream = NULL;
    const livekit_data_stream_attribute_t missing_value[] = {{"execution_id", NULL}};
    livekit_data_stream_options_t options = {
        .topic = "eidolon.camera.frame.v1", .attributes = missing_value, .attributes_count = 1};
    assert(data_stream_writer_open(writer, &options, &stream) == DATA_STREAM_WRITER_ERR_INVALID_ARG);
    options = (livekit_data_stream_options_t){.topic = "t", .destination_identities_count = 1};
    assert(data_stream_writer_open(writer, &options, &stream) == DATA_STREAM_WRITER_ERR_INVALID_ARG);

    // A header that could not be sent leaves no slot and no copied identities
    // behind (the address sanitizer reports a leak otherwise).
    const char* destinations[] = {"agent-channel"};
    options = (livekit_data_stream_options_t){
        .topic = "t", .destination_identities = destinations, .destination_identities_count = 1};
    fail_sends = true;
    for (int i = 0; i < 4; i++) {
        assert(data_stream_writer_open(writer, &options, &stream) == DATA_STREAM_WRITER_ERR_SEND);
    }
    fail_sends = false;
    sent_count = 0;
    assert(data_stream_writer_open(writer, &options, &stream) == DATA_STREAM_WRITER_ERR_NONE);
    // Destroying a writer with a stream still open frees its identities too.
    assert(data_stream_writer_destroy(writer) == DATA_STREAM_WRITER_ERR_NONE);
}

int main(void)
{
    test_byte_stream_with_attributes_reaches_one_participant();
    test_stream_without_options_is_unchanged();
    test_invalid_options_and_failed_header_release_the_slot();
    puts("livekit data stream writer tests passed");
    return 0;
}
