/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * ENV:netstack.prefs — the stack's startup configuration.
 *
 * One flat KEY = VALUE file (case-insensitive keys, '#'/';' comment lines,
 * unknown keys ignored), read once by the stack task before the driver
 * attach. Every key is optional; a missing key (or a missing file) keeps
 * the default, so an absent config means DHCP on networks/genet.device
 * unit 0.
 *
 * Keys:
 *   DEVICE   = networks/genet.device   OpenDevice name (loaded from DEVS:)
 *   UNIT     = 0
 *   MODE     = DHCP | STATIC
 *   ADDRESS  = a.b.c.d                 STATIC needs ADDRESS and NETMASK;
 *   NETMASK  = a.b.c.d                 an incomplete static config falls
 *   GATEWAY  = a.b.c.d                 back to DHCP (fail-safe)
 *   DNS1     = a.b.c.d
 *   DNS2     = a.b.c.d
 *   HOSTNAME = amiga                   DHCP option 12 / gethostname()
 *   VLAN     = vid[,pcp]               in-band 802.1Q (vid 1..4094, pcp 0..7);
 *                                      absent = untagged
 *
 * Multi-interface reservation: unprefixed keys are interface 0. A future
 * IFn_ prefix (IF1_DEVICE, IF1_MODE, ...) adds interfaces without a format
 * break — old stacks ignore the unknown keys.
 */

#ifndef SB_CONFIG_H
#define SB_CONFIG_H

#include <exec/types.h>

#include <lwip/ip_addr.h>

#define SB_CFG_DEVICE_MAX   64
#define SB_CFG_HOSTNAME_MAX 64

struct SbNetConfig
{
    char       cfg_Device[SB_CFG_DEVICE_MAX];
    LONG       cfg_Unit;
    BOOL       cfg_Dhcp;
    ip4_addr_t cfg_Addr;
    ip4_addr_t cfg_Mask;
    ip4_addr_t cfg_Gateway;
    ip4_addr_t cfg_Dns[2];
    char       cfg_Hostname[SB_CFG_HOSTNAME_MAX];
    LONG       cfg_VlanTci;   /* -1 = no VLAN; else (pcp<<13)|(vid&0xFFF) */
};

/* Defaults, then overrides from ENV:netstack.prefs. Needs a Process (DOS
 * file I/O); a bare-Task caller keeps the defaults. */
void sb_config_load(struct SbNetConfig *cfg);

#endif /* SB_CONFIG_H */
