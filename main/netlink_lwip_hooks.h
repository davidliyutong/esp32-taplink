#pragma once

#include "dhcpserver/dhcpserver.h"
#include "lwip/arch.h"

struct netif;

void router_append_dhcps_routes(struct netif *netif, u8_t state, u8_t **opts);
s16_t router_observe_dhcps_state(struct dhcps_msg *msg, u16_t len, s16_t state);

#define LWIP_HOOK_DHCPS_POST_APPEND_OPTS(netif, dhcps, state, pp_opts)         \
    router_append_dhcps_routes((netif), (state), (pp_opts));

#define LWIP_HOOK_DHCPS_POST_STATE(msg, len, state)                            \
    router_observe_dhcps_state((msg), (len), (state))
