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
 *   DOMAIN   = example.com             resolver search domain; unqualified
 *                                      (dot-less) names are retried as
 *                                      "name.DOMAIN" when the bare lookup fails
 *   VLAN     = vid[,pcp]               in-band 802.1Q (vid 1..4094, pcp 0..7);
 *                                      absent = untagged
 *   MDNS     = yes | no                advertise HOSTNAME.local over mDNS
 *                                      (default yes)
 *   MDNS_SERVICE = _ssh._tcp 22 [name] DNS-SD service to advertise (repeatable,
 *                                      up to SB_CFG_MDNS_MAX); the instance
 *                                      name defaults to the advertised host.
 *                                      More can be registered while running —
 *                                      see the `mdns` command / mdns_ctl.h
 *   NETWORK  = name number             networks-database entry (repeatable,
 *                                      up to SB_CFG_NETWORKS_MAX), /etc/networks
 *                                      notation ("homelan 192.168.0"); entries
 *                                      shadow the built-ins (default, loopback)
 *
 * Multi-interface reservation: unprefixed keys are interface 0. A future
 * IFn_ prefix (IF1_DEVICE, IF1_MODE, ...) adds interfaces without a format
 * break — old stacks ignore the unknown keys.
 */

#ifndef SB_CONFIG_H
#define SB_CONFIG_H

#include <exec/types.h>

#include <lwip/ip_addr.h>
#include <mdns_ctl.h> /* service type/name field sizes, shared with the tool */

#define SB_CFG_DEVICE_MAX   64
#define SB_CFG_HOSTNAME_MAX 64
#define SB_CFG_NETWORKS_MAX 8
#define SB_CFG_NETNAME_MAX  32
#define SB_CFG_MDNS_MAX     MDNSCTL_MAX_SERVICES

struct SbCfgNetwork
{
    char  name[SB_CFG_NETNAME_MAX];
    ULONG net; /* classful network number, host order (/etc/networks) */
};

/* One MDNS_SERVICE line, unvalidated: sb_mdns.c owns the "_base._tcp" split
 * (it is the same parse the control port's ADD needs). */
struct SbCfgMdnsService
{
    char  type[MDNSCTL_TYPE_MAX];
    char  name[MDNSCTL_NAME_MAX]; /* "" = use the advertised hostname */
    UWORD port;
};

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
    char       cfg_Domain[SB_CFG_HOSTNAME_MAX]; /* resolver search domain; "" = none */
    LONG       cfg_VlanTci;   /* -1 = no VLAN; else (pcp<<13)|(vid&0xFFF) */
    BOOL       cfg_Mdns;      /* advertise HOSTNAME.local */
    struct SbCfgNetwork cfg_Networks[SB_CFG_NETWORKS_MAX];
    ULONG      cfg_NumNetworks;
    struct SbCfgMdnsService cfg_MdnsServices[SB_CFG_MDNS_MAX];
    ULONG      cfg_NumMdnsServices;
};

/* Defaults, then overrides from ENV:netstack.prefs. Needs a Process (DOS
 * file I/O); a bare-Task caller keeps the defaults. */
void sb_config_load(struct SbNetConfig *cfg);

#endif /* SB_CONFIG_H */
