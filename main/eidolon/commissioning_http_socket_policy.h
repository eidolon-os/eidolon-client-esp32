#pragma once

#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

namespace eidolon {

// This protocol carries small request/response messages, not a bulk stream.
// Each accepted socket must send HTTP framing without waiting for another
// segment's acknowledgement. This does not acknowledge delivery or retry data.
inline bool ConfigureCommissioningHttpSocket(int socket)
{
    const int enabled = 1;
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                      &enabled, sizeof(enabled)) == 0;
}

}  // namespace eidolon
