#ifndef WFC_SLIRP_IMPL_H
#define WFC_SLIRP_IMPL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WfcSlirpContext WfcSlirpContext;

WfcSlirpContext *wfc_slirp_create(const char *dns_ip);
void wfc_slirp_destroy(WfcSlirpContext *ctx);
int wfc_slirp_send(WfcSlirpContext *ctx, const uint8_t *data, int len);
int wfc_slirp_poll(WfcSlirpContext *ctx, uint8_t *buf, int buflen);

#ifdef __cplusplus
}
#endif

#endif
