#include "wfc_slirp_impl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "libslirp/src/include/libslirp.h"

// Pull libslirp sources into a single translation unit for simpler builds.
#include "libslirp/src/arp_table.c"
#include "libslirp/src/bootp.c"
#include "libslirp/src/cksum.c"
#include "libslirp/src/dhcpv6.c"
#include "libslirp/src/dnssearch.c"
#include "libslirp/src/if.c"
#include "libslirp/src/ip6_icmp.c"
#include "libslirp/src/ip6_input.c"
#include "libslirp/src/ip6_output.c"
#include "libslirp/src/ip_icmp.c"
#include "libslirp/src/ip_input.c"
#include "libslirp/src/ip_output.c"
#include "libslirp/src/libslirp.c"
#include "libslirp/src/mbuf.c"
#include "libslirp/src/misc.c"
#include "libslirp/src/ndp_table.c"
#include "libslirp/src/qemu2libslirp.c"
#include "libslirp/src/sbuf.c"
#include "libslirp/src/slirp.c"
#include "libslirp/src/socket.c"
#include "libslirp/src/tcp_input.c"
#include "libslirp/src/tcp_output.c"
#include "libslirp/src/tcp_subr.c"
#include "libslirp/src/tcp_timer.c"
#include "libslirp/src/tftp.c"
#include "libslirp/src/udp.c"
#include "libslirp/src/udp6.c"
#include "libslirp/src/unixfwd.c"
#include "libslirp/src/timerfd.c"

struct WfcSlirpContext {
    SLIRP *slirp;
    int fd;
};

static int parse_dns_ip(const char *dns_ip, struct in_addr *out)
{
    if (!dns_ip || !dns_ip[0]) return 0;
    return inet_pton(AF_INET, dns_ip, out) == 1;
}

WfcSlirpContext *wfc_slirp_create(const char *dns_ip)
{
    WfcSlirpContext *ctx = (WfcSlirpContext *)calloc(1, sizeof(WfcSlirpContext));
    if (!ctx) return NULL;

    ctx->slirp = slirp_open(SLIRP_IPV4);
    if (!ctx->slirp)
    {
        free(ctx);
        return NULL;
    }

    struct in_addr dns_addr;
    if (parse_dns_ip(dns_ip, &dns_addr))
        slirp_set_dnsaddr(ctx->slirp, dns_addr);

    if (slirp_start(ctx->slirp) != 0)
    {
        slirp_close(ctx->slirp);
        free(ctx);
        return NULL;
    }

    ctx->fd = slirp_fd(ctx->slirp);
    return ctx;
}

void wfc_slirp_destroy(WfcSlirpContext *ctx)
{
    if (!ctx) return;
    if (ctx->slirp)
        slirp_close(ctx->slirp);
    free(ctx);
}

int wfc_slirp_send(WfcSlirpContext *ctx, const uint8_t *data, int len)
{
    if (!ctx || !ctx->slirp || !data || len <= 0) return 0;
    ssize_t res = slirp_send(ctx->slirp, data, (size_t)len);
    if (res < 0) return 0;
    return (int)res;
}

int wfc_slirp_poll(WfcSlirpContext *ctx, uint8_t *buf, int buflen)
{
    if (!ctx || !ctx->slirp || !buf || buflen <= 0) return 0;
    struct pollfd pfd;
    pfd.fd = ctx->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ready = poll(&pfd, 1, 0);
    if (ready <= 0 || !(pfd.revents & POLLIN)) return 0;

    ssize_t res = slirp_recv(ctx->slirp, buf, (size_t)buflen);
    if (res <= 0) return 0;
    return (int)res;
}
