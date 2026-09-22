#include "eidolon/commissioning_http_socket_policy.h"
#include <cassert>
#include <unistd.h>

int main()
{
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(socket >= 0);
    assert(eidolon::ConfigureCommissioningHttpSocket(socket));
    int enabled = 0;
    socklen_t size = sizeof(enabled);
    assert(getsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, &size) == 0);
    assert(enabled != 0);
    close(socket);
    assert(!eidolon::ConfigureCommissioningHttpSocket(-1));
}
