/* Exercise the firmware SDK's real TCP output path without a radio or RTOS.
 * The peer deliberately withholds ACKs. This isolates Nagle's framing delay;
 * it does not model Wi-Fi loss or claim to reproduce the entire hardware fault.
 */
#include "lwip/init.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/netif.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned packets;
static char delivered[512];
static size_t delivered_length;

static err_t output(struct netif *netif, struct pbuf *packet,
                    const ip4_addr_t *destination)
{
    (void)netif;
    (void)destination;
    struct ip_hdr ip;
    struct tcp_hdr tcp;
    assert(pbuf_copy_partial(packet, &ip, sizeof(ip), 0) == sizeof(ip));
    const u16_t ip_length = IPH_HL_BYTES(&ip);
    assert(pbuf_copy_partial(packet, &tcp, sizeof(tcp), ip_length) == sizeof(tcp));
    const u16_t offset = ip_length + TCPH_HDRLEN_BYTES(&tcp);
    const u16_t length = packet->tot_len - offset;
    assert(delivered_length + length < sizeof(delivered));
    assert(pbuf_copy_partial(packet, delivered + delivered_length, length,
                             offset) == length);
    delivered_length += length;
    delivered[delivered_length] = '\0';
    packets++;
    return ERR_OK;
}

static err_t initialize_netif(struct netif *netif)
{
    netif->output = output;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

int main(void)
{
    lwip_init();
    struct netif netif = {0};
    ip4_addr_t local, remote, mask;
    IP4_ADDR(&local, 192, 168, 4, 1);
    IP4_ADDR(&remote, 192, 168, 4, 2);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    assert(netif_add(&netif, &local, &mask, &local, NULL,
                     initialize_netif, ip_input) != NULL);
    netif_set_default(&netif);

    for (int nodelay = 0; nodelay < 2; nodelay++) {
        struct tcp_pcb *pcb = tcp_new();
        assert(pcb != NULL);
        // An established connection with ample congestion and receive windows.
        pcb->state = ESTABLISHED;
        pcb->local_ip = local;
        pcb->remote_ip = remote;
        pcb->local_port = 80;
        pcb->remote_port = 30000;
        pcb->snd_wnd = TCP_WND;
        pcb->cwnd = TCP_WND;
        pcb->mss = TCP_MSS;
        if (nodelay) tcp_nagle_disable(pcb);
        packets = 0;
        delivered_length = 0;
        delivered[0] = '\0';

        // Same write boundaries as IDF httpd_resp_send for a normal response.
        const char *parts[] = {
            "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n", "\r\n", "abc"
        };
        for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
            assert(tcp_write(pcb, parts[i], strlen(parts[i]),
                              TCP_WRITE_FLAG_COPY) == ERR_OK);
            assert(tcp_output(pcb) == ERR_OK);
        }
        if (nodelay) {
            assert(packets == 3 && pcb->unsent == NULL);
            assert(strcmp(delivered,
                "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc") == 0);
        } else {
            assert(packets == 1 && pcb->unsent != NULL);
            // A peer HTTP reader cannot complete even the headers yet.
            assert(strstr(delivered, "\r\n\r\n") == NULL);
        }
        printf("TCP_NODELAY=%d: output_packets=%u pending_tail=%d complete_headers=%d\n",
               nodelay, packets, pcb->unsent != NULL,
               strstr(delivered, "\r\n\r\n") != NULL);
        tcp_abort(pcb);
    }
    return 0;
}
