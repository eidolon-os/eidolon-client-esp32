#include "eidolon/policy_audio_renderer.h"
#include "eidolon/output_policy.h"
#include <cassert>
#include <cstdlib>
extern "C" void* media_lib_calloc(size_t n,size_t size) { return std::calloc(n,size); }
extern "C" void media_lib_free(void* p) { std::free(p); }
int writes=0,freed=0;
void* Init(void* p,int) { return p; }
int Open(void*,av_render_audio_frame_info_t*) { return 0; }
int Write(void*,av_render_audio_frame_t*) { ++writes;return 0; }
int Latency(void*,uint32_t* ms) { *ms=0;return 0; }
int Info(void*,av_render_audio_frame_info_t*) { return 0; }
int Speed(void*,float) { return 0; }
int Close(void*) { return 0; }
void Free(void*) { ++freed; }
int main() {
    int config=1;
    audio_render_cfg_t cfg={{Init,Open,Write,Latency,Info,Speed,Close,Free},&config,sizeof(config)};
    auto* inner=audio_render_alloc_handle(&cfg);
    auto* outer=eidolon::AllocatePolicyAudioRenderer(inner);
    assert(outer);
    av_render_audio_frame_info_t info={};av_render_audio_frame_t frame={};
    assert(audio_render_open(outer,&info)==0);
    auto& gate=eidolon::CurrentOutputGate();
    using eidolon::presentation::Output;
    auto speech=eidolon::OutputBit(Output::Speech);
    assert(gate.Bind({true,1,speech}));
    assert(audio_render_write(outer,&frame)==0 && writes==0);
    assert(gate.Start({"s",1,speech,false},"s"));
    assert(audio_render_write(outer,&frame)==0 && writes==1);
    gate.Close();
    // Queue contents are irrelevant: each final write is independently gated.
    for(int i=0;i<100;++i) assert(audio_render_write(outer,&frame)==0);
    assert(writes==1);
    const auto text=eidolon::OutputBit(Output::DialogueText);
    const auto cue=eidolon::OutputBit(Output::AudioCue);
    assert(gate.Bind({true,2,text|cue}));
#if CONFIG_EIDOLON_CAP_AUDIO_CUE
    assert(gate.Start({"cue",2,text|cue,false},"cue"));
    assert(!gate.Allows(Output::Speech));
    assert(audio_render_write(outer,&frame)==0 && writes==2);
#else
    assert(!gate.Start({"cue",2,text|cue,false},"cue"));
    assert(audio_render_write(outer,&frame)==0 && writes==1);
#endif
    const auto before=writes;
    // A fresh text-only selection must stop stale buffered audio immediately.
    assert(gate.Start({"text",2,text,false},"text"));
    for(int i=0;i<100;++i) assert(audio_render_write(outer,&frame)==0);
    assert(writes==before);
    assert(audio_render_close(outer)==0);
    audio_render_free_handle(outer);
    assert(freed==1);
}
