// The vendored LiveKit SDK decoding a participant's attributes from the
// signalling channel. The Channel Provider announces the Owner policy revision
// as one (`eidolon.policy_revision`); before 2026-11-16 the SDK dropped every
// attribute. Decoded with the real nanopb and the SDK's generated bindings,
// exactly as protocol_signal_response_decode/_free do, from bytes the
// server-side protocol encoder produced.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>

#include "livekit_rtc.pb.h"

static long live_allocations = 0;

void* counting_realloc(void* ptr, size_t size)
{
    void* out = realloc(ptr, size);
    if (ptr == NULL && out != NULL) ++live_allocations;
    return out;
}

void counting_free(void* ptr)
{
    if (ptr != NULL) --live_allocations;
    free(ptr);
}

// livekit-protocol 1.1.26 (Python, the encoder the Provider uses):
// SignalResponse(update=ParticipantUpdate(participants=[
//   ParticipantInfo(sid="PA_device", identity="device-instance-1", state=ACTIVE,
//     metadata='{"kind":"device"}', kind=STANDARD,
//     attributes={"eidolon.policy_revision": "9", "lk.note": "x" * 40},
//     permission=ParticipantPermission(can_subscribe=True, can_publish=True,
//       can_publish_data=True, can_publish_sources=[MICROPHONE])),
//   ParticipantInfo(sid="PA_agent", identity="agent-1", state=ACTIVE, kind=AGENT,
//     permission=ParticipantPermission(can_subscribe=True, can_publish=True,
//       can_publish_data=True, agent=True))]))
static const uint8_t kParticipantUpdate[] = {
    0x2a, 0xb7, 0x01, 0x0a, 0x91, 0x01, 0x0a, 0x09, 0x50, 0x41, 0x5f, 0x64,
    0x65, 0x76, 0x69, 0x63, 0x65, 0x12, 0x11, 0x64, 0x65, 0x76, 0x69, 0x63,
    0x65, 0x2d, 0x69, 0x6e, 0x73, 0x74, 0x61, 0x6e, 0x63, 0x65, 0x2d, 0x31,
    0x18, 0x02, 0x2a, 0x11, 0x7b, 0x22, 0x6b, 0x69, 0x6e, 0x64, 0x22, 0x3a,
    0x22, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x22, 0x7d, 0x5a, 0x09, 0x08,
    0x01, 0x10, 0x01, 0x18, 0x01, 0x4a, 0x01, 0x02, 0x7a, 0x33, 0x0a, 0x07,
    0x6c, 0x6b, 0x2e, 0x6e, 0x6f, 0x74, 0x65, 0x12, 0x28, 0x78, 0x78, 0x78,
    0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78,
    0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78,
    0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78,
    0x78, 0x7a, 0x1c, 0x0a, 0x17, 0x65, 0x69, 0x64, 0x6f, 0x6c, 0x6f, 0x6e,
    0x2e, 0x70, 0x6f, 0x6c, 0x69, 0x63, 0x79, 0x5f, 0x72, 0x65, 0x76, 0x69,
    0x73, 0x69, 0x6f, 0x6e, 0x12, 0x01, 0x39, 0x0a, 0x21, 0x0a, 0x08, 0x50,
    0x41, 0x5f, 0x61, 0x67, 0x65, 0x6e, 0x74, 0x12, 0x07, 0x61, 0x67, 0x65,
    0x6e, 0x74, 0x2d, 0x31, 0x18, 0x02, 0x5a, 0x08, 0x08, 0x01, 0x10, 0x01,
    0x18, 0x01, 0x58, 0x01, 0x70, 0x04
};

static const livekit_pb_participant_info_attributes_entry_t* find(
    const livekit_pb_participant_info_t* info, const char* key)
{
    for (pb_size_t i = 0; i < info->attributes_count; i++) {
        if (info->attributes[i].key && strcmp(info->attributes[i].key, key) == 0) {
            return &info->attributes[i];
        }
    }
    return NULL;
}

int main(void)
{
    livekit_pb_signal_response_t res = LIVEKIT_PB_SIGNAL_RESPONSE_INIT_ZERO;
    pb_istream_t stream = pb_istream_from_buffer(kParticipantUpdate, sizeof(kParticipantUpdate));
    const bool decoded = pb_decode(&stream, LIVEKIT_PB_SIGNAL_RESPONSE_FIELDS, &res);
    if (!decoded) fprintf(stderr, "decode: %s\n", PB_GET_ERROR(&stream));
    assert(decoded);
    assert(res.which_message == LIVEKIT_PB_SIGNAL_RESPONSE_UPDATE_TAG);

    const livekit_pb_participant_update_t* update = &res.message.update;
    assert(update->participants_count == 2);
    const livekit_pb_participant_info_t* device = &update->participants[0];
    assert(strcmp(device->identity, "device-instance-1") == 0);
    assert(device->state == LIVEKIT_PB_PARTICIPANT_INFO_STATE_ACTIVE);
    assert(device->attributes_count == 2);
    const livekit_pb_participant_info_attributes_entry_t* revision =
        find(device, "eidolon.policy_revision");
    assert(revision != NULL && strcmp(revision->value, "9") == 0);
    const livekit_pb_participant_info_attributes_entry_t* note = find(device, "lk.note");
    assert(note != NULL && strlen(note->value) == 40);
    // Fields decoded before the attributes still arrive as they did.
    assert(strcmp(device->metadata, "{\"kind\":\"device\"}") == 0);
    assert(device->permission.can_publish && device->permission.can_subscribe);

    // A participant with no attributes, as from a server that sets none.
    const livekit_pb_participant_info_t* agent = &update->participants[1];
    assert(agent->kind == LIVEKIT_PB_PARTICIPANT_INFO_KIND_AGENT);
    assert(agent->attributes_count == 0 && agent->attributes == NULL);

    assert(live_allocations > 0);
    pb_release(LIVEKIT_PB_SIGNAL_RESPONSE_FIELDS, &res);
    assert(live_allocations == 0);

    puts("PASS: participant attributes decode from signalling and are released with the response");
    return 0;
}
