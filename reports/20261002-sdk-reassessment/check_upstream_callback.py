"""Check event delivery using the unmodified upstream public converter.

This is a host boundary check, not a server or hardware permission test.
It establishes that adding public permission fields is unnecessary merely to
observe the delivery of a local participant update.
"""
import pathlib
import subprocess
import tempfile

SDK = pathlib.Path('/Users/manson/ai/eidolon/vendor/client-sdk-esp32')
UPSTREAM = 'fbf09edf27a504d82119f730a35bf00fa287f7c1'


def source(path):
    return subprocess.check_output(
        ['git', '-C', str(SDK), 'show', f'{UPSTREAM}:{path}'], text=True)


def definition(text, needle):
    begin = text.index(needle)
    end = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


header = source('components/livekit/include/livekit.h')
end = header.index('} livekit_participant_info_t;') + len('} livekit_participant_info_t;')
begin = header.rfind('typedef struct {', 0, end)
converter = definition(source('components/livekit/core/livekit.c'),
                       'static void on_eng_participant_info(')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int livekit_participant_kind_t;
typedef int livekit_participant_state_t;
''' + header[begin:end] + r'''
typedef struct {
    const char *sid, *identity, *name, *metadata;
    int kind, state;
    struct { bool can_publish, can_subscribe, can_publish_data; } permission;
} livekit_pb_participant_info_t;
typedef struct {
    struct {
        void (*on_participant_info)(const livekit_participant_info_t *, void *);
        void *ctx;
    } options;
} livekit_room_t;
static int local_updates, remote_updates;
static void collect(const livekit_participant_info_t *info, void *ctx) {
    assert(ctx == (void *)7);
    if (!strcmp(info->identity, "device")) {
        assert(!strcmp(info->sid, "PA_local"));
        ++local_updates;
    } else ++remote_updates;
}
''' + converter + r'''
int main(void) {
    livekit_room_t room = {.options = {.on_participant_info = collect, .ctx = (void *)7}};
    livekit_pb_participant_info_t participant = {
        .sid = "PA_local", .identity = "device", .name = "name", .metadata = "metadata",
        .kind = 0, .state = 2,
        .permission = {.can_publish = true, .can_subscribe = true, .can_publish_data = true}
    };
    on_eng_participant_info(&participant, true, &room);
    participant.permission.can_publish = false;
    on_eng_participant_info(&participant, true, &room);
    participant.permission.can_publish = true;
    on_eng_participant_info(&participant, true, &room);
    participant.identity = "other"; participant.sid = "PA_remote";
    on_eng_participant_info(&participant, false, &room);
    assert(local_updates == 3 && remote_updates == 1);
    room.options.on_participant_info = NULL;
    on_eng_participant_info(&participant, false, &room);
    assert(remote_updates == 1);
    puts("PASS: upstream converter delivers join/revoke/restore inputs; identity identifies local updates");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    (path / 'check.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(path / 'check.c'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)
print(f'UPSTREAM={UPSTREAM}')
