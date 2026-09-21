#pragma once
#include "FreeRTOS.h"
#include <algorithm>
#include <deque>
#include <cassert>
// Models the IDF 5.5.4 API capacity distinction, not the PCM policy:
// dynamic creation adds a sentinel byte; WithCaps uses static creation directly.
struct TestByteStream { size_t capacity; std::deque<uint8_t> bytes; };
using StreamBufferHandle_t = TestByteStream*;
inline auto xStreamBufferCreate(size_t n, size_t) { return new TestByteStream{n,{}}; }
inline auto xStreamBufferCreateWithCaps(size_t n, size_t, unsigned) { return new TestByteStream{n-1,{}}; }
inline void vStreamBufferDelete(StreamBufferHandle_t h) { delete h; }
inline void vStreamBufferDeleteWithCaps(StreamBufferHandle_t h) { delete h; }
inline size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t h) { return h->capacity-h->bytes.size(); }
inline size_t xStreamBufferSend(StreamBufferHandle_t h,const void* p,size_t n,TickType_t) {
 n=std::min(n,xStreamBufferSpacesAvailable(h)); auto* b=static_cast<const uint8_t*>(p);
 h->bytes.insert(h->bytes.end(),b,b+n); return n;
}
inline size_t xStreamBufferReceive(StreamBufferHandle_t h,void* p,size_t n,TickType_t) {
 assert(!h->bytes.empty()); // fail instead of hanging if a test asks for unavailable data
 n=std::min(n,h->bytes.size()); auto* b=static_cast<uint8_t*>(p);
 for(size_t i=0;i<n;++i) { b[i]=h->bytes.front(); h->bytes.pop_front(); } return n;
}
inline int xStreamBufferReset(StreamBufferHandle_t h) { h->bytes.clear(); return 1; }
