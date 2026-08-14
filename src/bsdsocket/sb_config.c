/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * ENV:netstack.prefs reader — see sb_config.h for the schema. The same
 * defaults-then-override pattern as genet's runtime config: a missing or
 * unreadable file is not an error, it is the default configuration.
 */

#include "sb_base.h"

#include <dos/dos.h>

#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#else
#include <proto/dos.h>
#endif

#include <debug.h>
#include <strutil.h>
#include <prefs.h>

static void sb_cfg_copy(char *dst, ULONG max, const char *src)
{
    ULONG i = 0;
    while (src[i] != '\0' && i < max - 1)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Cut the leading blank-delimited word off *rest, in place: the word is
 * NUL-terminated and *rest is left on the first character after it, blanks
 * skipped. Whatever remains is the caller's — a value whose last field is
 * free text (an mDNS instance name) just stops calling this. Both the word
 * and the remainder are "" once the value is exhausted, never NULL. */
static char *sb_cfg_word(char **rest)
{
    char *word = *rest;
    char *p = word;

    while (*p != '\0' && *p != ' ' && *p != '\t')
        p++;
    if (*p != '\0')
    {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
    }
    *rest = p;
    return word;
}

/* /etc/networks notation: 1..4 dot-separated octets, most significant first —
 * "127" = 127, "192.168.6" = 0xC0A806. NOT an IP address (inet_addr pads the
 * missing octets differently). */
static BOOL sb_cfg_parse_netnum(const char *s, ULONG *out)
{
    ULONG net = 0;
    ULONG parts = 0;
    while (*s != '\0')
    {
        ULONG octet = 0;
        ULONG digits = 0;
        while (*s >= '0' && *s <= '9')
        {
            octet = octet * 10 + (ULONG)(*s - '0');
            if (octet > 255)
                return FALSE;
            s++;
            digits++;
        }
        if (digits == 0 || ++parts > 4)
            return FALSE;
        net = (net << 8) | octet;
        if (*s == '.')
            s++;
        else if (*s != '\0')
            return FALSE;
    }
    if (parts == 0)
        return FALSE;
    *out = net;
    return TRUE;
}

/* yes/no key: everything else leaves the default alone (and says so) */
static BOOL sb_cfg_parse_bool(const char *s, BOOL *out)
{
    if (_Stricmp((CONST_STRPTR)s, (CONST_STRPTR) "yes") == 0 ||
        _Stricmp((CONST_STRPTR)s, (CONST_STRPTR) "on") == 0 ||
        _Stricmp((CONST_STRPTR)s, (CONST_STRPTR) "1") == 0)
        *out = TRUE;
    else if (_Stricmp((CONST_STRPTR)s, (CONST_STRPTR) "no") == 0 ||
             _Stricmp((CONST_STRPTR)s, (CONST_STRPTR) "off") == 0 ||
             _Stricmp((CONST_STRPTR)s, (CONST_STRPTR) "0") == 0)
        *out = FALSE;
    else
        return FALSE;
    return TRUE;
}

static void sb_cfg_defaults(struct SbNetConfig *cfg)
{
    for (ULONG i = 0; i < sizeof(*cfg); i++)
        ((UBYTE *)cfg)[i] = 0;
    sb_cfg_copy(cfg->cfg_Hostname, sizeof(cfg->cfg_Hostname), "amiga");
    cfg->cfg_Mdns = TRUE;    /* HOSTNAME.local costs one multicast group */
}

/* The previous model interface keys: recognized so a stale prefs file
 * gets one clear notice instead of silent misconfiguration. Interfaces are
 * configured in DEVS:NetInterfaces/ via AddNetInterface now. */
static BOOL sb_cfg_key_obsolete(const char *key)
{
    static const char *const obsolete[] = {
        "DEVICE", "UNIT", "MODE", "ADDRESS", "NETMASK", "GATEWAY", "VLAN",
    };
    for (ULONG i = 0; i < sizeof(obsolete) / sizeof(obsolete[0]); i++)
    {
        if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR)obsolete[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

void sb_config_load(struct SbNetConfig *cfg)
{
    sb_cfg_defaults(cfg);

    /* dos packet I/O needs a Process (the stack task is one) */
    if (FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
        return;

    struct DosLibrary *DOSBase =
        (struct DosLibrary *)OpenLibrary((CONST_STRPTR) "dos.library", 36);
    if (DOSBase == NULL)
        return;

    BPTR fh = Open((CONST_STRPTR) "ENV:netstack.prefs", MODE_OLDFILE);
    if (fh == 0)
    {
        CloseLibrary((struct Library *)DOSBase);
        return;
    }
    KprintfT("[bsdsocket] %s: reading ENV:netstack.prefs\n", __func__);

    BOOL warnedObsolete = FALSE;
    char linebuf[256];
    while (FGets(fh, (STRPTR)linebuf, sizeof(linebuf)))
    {
        char *key, *val;
        if (!prefs_split(linebuf, &key, &val))
            continue;

        if (sb_cfg_key_obsolete(key))
        {
            if (!warnedObsolete)
            {
                Kprintf("[bsdsocket] netstack.prefs: DEVICE/UNIT/MODE/ADDRESS/"
                        "NETMASK/GATEWAY/VLAN are obsolete — interfaces are "
                        "configured in DEVS:NetInterfaces/ (see AddNetInterface)\n");
                warnedObsolete = TRUE;
            }
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "DNS1") == 0)
        {
            if (!ip4addr_aton(val, &cfg->cfg_Dns[0]))
                Kprintf("[bsdsocket] netstack.prefs: bad DNS1 '%s'\n", val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "DNS2") == 0)
        {
            if (!ip4addr_aton(val, &cfg->cfg_Dns[1]))
                Kprintf("[bsdsocket] netstack.prefs: bad DNS2 '%s'\n", val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "HOSTNAME") == 0)
        {
            sb_cfg_copy(cfg->cfg_Hostname, sizeof(cfg->cfg_Hostname), val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "DOMAIN") == 0)
        {
            sb_cfg_copy(cfg->cfg_Domain, sizeof(cfg->cfg_Domain), val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "MDNS") == 0)
        {
            if (!sb_cfg_parse_bool(val, &cfg->cfg_Mdns))
                Kprintf("[bsdsocket] netstack.prefs: bad MDNS '%s'\n", val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "MDNS_SERVICE") == 0)
        {
            /* MDNS_SERVICE = <_type._proto> <port> [instance name]  (repeatable).
             * The name is the rest of the line, spaces included ("Amiga 1200");
             * the type is validated where it is registered (sb_mdns.c). */
            char *rest = val;
            char *type = sb_cfg_word(&rest);
            char *portStr = sb_cfg_word(&rest);
            char *name = rest;

            LONG port;
            if (*type == '\0' || !StrToLong((STRPTR)portStr, &port) || port <= 0 ||
                port > 65535)
                Kprintf("[bsdsocket] netstack.prefs: bad MDNS_SERVICE '%s'\n", type);
            else if (cfg->cfg_NumMdnsServices >= SB_CFG_MDNS_MAX)
                Kprintf("[bsdsocket] netstack.prefs: MDNS_SERVICE table full (max %ld)\n",
                        (LONG)SB_CFG_MDNS_MAX);
            else
            {
                struct SbCfgMdnsService *s =
                    &cfg->cfg_MdnsServices[cfg->cfg_NumMdnsServices++];
                sb_cfg_copy(s->type, sizeof(s->type), type);
                sb_cfg_copy(s->name, sizeof(s->name), name);
                s->port = (UWORD)port;
            }
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "NETWORK") == 0)
        {
            /* NETWORK = <name> <number>  (repeatable): extra networks-database
             * entries for getnetbyname/getnetbyaddr, shadowing the built-ins */
            char *num = val;
            char *name = sb_cfg_word(&num);

            ULONG netnum;
            if (*name == '\0' || !sb_cfg_parse_netnum(num, &netnum))
                Kprintf("[bsdsocket] netstack.prefs: bad NETWORK '%s'\n", name);
            else if (cfg->cfg_NumNetworks >= SB_CFG_NETWORKS_MAX)
                Kprintf("[bsdsocket] netstack.prefs: NETWORK table full (max %ld)\n",
                        (LONG)SB_CFG_NETWORKS_MAX);
            else
            {
                struct SbCfgNetwork *n = &cfg->cfg_Networks[cfg->cfg_NumNetworks++];
                sb_cfg_copy(n->name, sizeof(n->name), name);
                n->net = netnum;
            }
        }
        /* unknown keys are ignored */
    }

    Close(fh);
    CloseLibrary((struct Library *)DOSBase);
}
