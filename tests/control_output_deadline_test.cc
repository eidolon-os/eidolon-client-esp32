#include "eidolon/control_protocol.h"
#include <cassert>
#include <ctime>
using namespace eidolon;
int main() {
    const std::string now=std::to_string(static_cast<long long>(std::time(nullptr))*1000);
    auto cmd=ParseControlCommand("{\"v\":1,\"kind\":\"cmd\",\"op\":\"expression.play\",\"ts\":"+now+",\"ttl_ms\":5000}");
    assert(cmd.valid && cmd.bounded_deadline && !cmd.expired);
    for (auto ttl: {"null","true","-1","1.5","15001","1e999"}) {
        cmd=ParseControlCommand("{\"v\":1,\"kind\":\"cmd\",\"op\":\"expression.play\",\"ts\":"+now+",\"ttl_ms\":"+ttl+"}");
        assert(!cmd.bounded_deadline);
    }
    cmd=ParseControlCommand(R"({"v":1,"kind":"cmd","op":"expression.play","ts":1e999,"ttl_ms":5})");
    assert(!cmd.bounded_deadline);
}
