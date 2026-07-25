/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mDNS/DNS-SD presence — see sb_mdns.c. Everything here runs on the stack
 * task; the module takes the core lock itself around the lwIP raw API.
 */

#ifndef SB_MDNS_H
#define SB_MDNS_H

#include <exec/types.h>

struct netif;
struct SbNetConfig;

/* Bring the responder up on nif: advertise cfg_Hostname as <name>.local, add
 * the prefs services, and publish the control port. A no-op when MDNS=no. */
void sb_mdns_start(struct netif *nif, const struct SbNetConfig *cfg);

/* Withdraw the port and the responder. Safe when never started. */
void sb_mdns_stop(void);

/* Control-port signal for the stack task's Wait(), 0 when there is no port. */
ULONG sb_mdns_sigmask(void);

/* Serve every queued control message. */
void sb_mdns_service(void);

#endif /* SB_MDNS_H */
