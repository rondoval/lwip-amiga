/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * ENV:netstack.prefs — the stack's STACK-WIDE startup configuration.
 *
 * One flat KEY = VALUE file (case-insensitive keys, '#'/';' comment lines,
 * unknown keys ignored), read once when the stack task starts. Every key is
 * optional; a missing key (or a missing file) keeps the default.
 *
 * Interfaces are NOT configured here: they live in DEVS:NetInterfaces/<name>
 * files and are added at runtime by the AddNetInterface command (over the
 * netstack_ctl.h control port). The old interface keys (DEVICE, UNIT, MODE,
 * ADDRESS, NETMASK, GATEWAY, VLAN) are recognized as obsolete and ignored
 * with a debug-log notice.
 *
 * Keys:
 *   DNS1     = a.b.c.d                 explicit resolvers; they override
 *   DNS2     = a.b.c.d                 whatever a DHCP lease supplies
 *   HOSTNAME = amiga                   DHCP option 12 / gethostname(); a
 *                                      per-interface ID= overrides it there
 *   DOMAIN   = example.com             resolver search domain; unqualified
 *                                      (dot-less) names are retried as
 *                                      "name.DOMAIN" when the bare lookup fails
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
 */

#ifndef SB_CONFIG_H
#define SB_CONFIG_H

#include <exec/types.h>

#include <lwip/ip_addr.h>
#include <mdns_ctl.h>     /* service type/name field sizes, shared with the tool */
#include <netstack_ctl.h> /* hostname field size, shared with the tools */

#define SB_CFG_HOSTNAME_MAX NETCTL_ID_MAX
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
    ip4_addr_t cfg_Dns[2];    /* explicit resolvers; beat any DHCP lease */
    char       cfg_Hostname[SB_CFG_HOSTNAME_MAX];
    char       cfg_Domain[SB_CFG_HOSTNAME_MAX]; /* resolver search domain; "" = none */
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
