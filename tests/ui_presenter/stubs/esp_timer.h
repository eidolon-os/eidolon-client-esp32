#pragma once
#include <cstdint>
// The presenter's clock, advanced by the test to cross request time limits.
inline int64_t& FakeTimeUs() { static int64_t now_us = 1000000000; return now_us; }
inline int64_t esp_timer_get_time() { return FakeTimeUs(); }
