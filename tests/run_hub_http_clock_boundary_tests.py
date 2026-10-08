#!/usr/bin/env python3
"""Run the production HTTP event observer across slow TLS and HTTP responses."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'main/eidolon/hub_pinned_http.cc').read_text()
a = s.index('struct HubDateObservation')
b = s.index('\nesp_http_client_method_t MethodFor',a)
code = r'''
#include <cassert>
#include <cstdint>
#include <strings.h>
#include "eidolon/hub_clock.h"
#include "eidolon/http_date_utc.h"
#define ESP_LOGW(...) ((void)0)
using esp_err_t=int;
constexpr int ESP_OK=0, HTTP_EVENT_ON_CONNECTED=1, HTTP_EVENT_ON_HEADER=2;
struct esp_http_client_event_t {int event_id; void* user_data; const char* header_key=nullptr; const char* header_value=nullptr;};
int64_t now_ms=0;
int64_t esp_timer_get_time() {return now_ms*1000;}
using namespace eidolon;
OBSERVER
int main() {
    HubDateObservation observation;
    esp_http_client_event_t event{HTTP_EVENT_ON_CONNECTED,&observation};
    // 12 seconds DNS/TCP/TLS must not inflate a fresh response's clock.
    now_ms=12000; CaptureHubDate(&event);
    event.event_id=HTTP_EVENT_ON_HEADER;event.header_key="Date";
    event.header_value="Thu, 08 Oct 2026 04:00:00 GMT";
    CaptureHubDate(&event);
    HubClock clock;
    clock.Observe(observation.utc_ms,observation.request_start_ms,12100);
    assert(clock.Now(12100)==observation.utc_ms+1099);
    assert(clock.Now(13100)==observation.utc_ms+2099);
    // A slow response after sending the request remains conservative.
    clock.Observe(observation.utc_ms,observation.request_start_ms,22000);
    assert(clock.Now(22000)==observation.utc_ms+10999);
    HubDateObservation missing;
    clock.Observe(observation.utc_ms,missing.request_start_ms,12100);
    assert(clock.Now(12100)==0);
    event.user_data=&missing; event.header_value="invalid";
    CaptureHubDate(&event);assert(missing.utc_ms==0);
    assert(CaptureHubDate(nullptr)==ESP_OK);
}
'''.replace('OBSERVER',s[a:b])
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cc').write_text(code)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(root/'main'),str(p/'test.cc'),str(root/'main/eidolon/http_date_utc.cc'),str(root/'main/eidolon/rfc3339_utc.cc'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('HTTP clock: TLS exclusion, delayed response, missing/invalid Date PASS')
