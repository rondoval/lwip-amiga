/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * ENV:netstack.prefs reader — see sb_config.h for the schema. The same
 * defaults-then-override pattern as genet's runtime config: a missing or
 * unreadable file is not an error, it is the DHCP default configuration.
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

static void sb_cfg_defaults(struct SbNetConfig *cfg)
{
    for (ULONG i = 0; i < sizeof(*cfg); i++)
        ((UBYTE *)cfg)[i] = 0;
    sb_cfg_copy(cfg->cfg_Device, sizeof(cfg->cfg_Device), "networks/genet.device");
    sb_cfg_copy(cfg->cfg_Hostname, sizeof(cfg->cfg_Hostname), "amiga");
    cfg->cfg_Dhcp = TRUE;
    cfg->cfg_VlanTci = -1;   /* no VLAN (0 would be priority-tagged VID 0) */
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
    KprintfH("[bsdsocket] %s: reading ENV:netstack.prefs\n", __func__);

    BOOL staticMode = FALSE;
    char linebuf[256];
    while (FGets(fh, (STRPTR)linebuf, sizeof(linebuf)))
    {
        char *line = linebuf;
        char *eol = line;
        while (*eol && *eol != '\n' && *eol != '\r')
            eol++;
        *eol = '\0';

        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '#' || *line == ';')
            continue;

        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (*eq != '=')
            continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        char *end = val;
        while (*end)
            end++;
        while (end > val && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        end = key;
        while (*end)
            end++;
        while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*key == '\0' || *val == '\0')
            continue;

        LONG parsed;

        if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "DEVICE") == 0)
        {
            sb_cfg_copy(cfg->cfg_Device, sizeof(cfg->cfg_Device), val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "UNIT") == 0)
        {
            if (StrToLong((STRPTR)val, &parsed) && parsed >= 0)
                cfg->cfg_Unit = parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "MODE") == 0)
        {
            if (_Stricmp((CONST_STRPTR)val, (CONST_STRPTR) "STATIC") == 0)
                staticMode = TRUE;
            else if (_Stricmp((CONST_STRPTR)val, (CONST_STRPTR) "DHCP") == 0)
                staticMode = FALSE;
            else
                Kprintf("[bsdsocket] netstack.prefs: bad MODE '%s'\n", val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "ADDRESS") == 0)
        {
            if (!ip4addr_aton(val, &cfg->cfg_Addr))
                Kprintf("[bsdsocket] netstack.prefs: bad ADDRESS '%s'\n", val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "NETMASK") == 0)
        {
            if (!ip4addr_aton(val, &cfg->cfg_Mask))
                Kprintf("[bsdsocket] netstack.prefs: bad NETMASK '%s'\n", val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "GATEWAY") == 0)
        {
            if (!ip4addr_aton(val, &cfg->cfg_Gateway))
                Kprintf("[bsdsocket] netstack.prefs: bad GATEWAY '%s'\n", val);
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
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "VLAN") == 0)
        {
            /* VLAN = <vid>[,<pcp>]  (in-band 802.1Q; vid 1..4094, pcp 0..7) */
            char *comma = val;
            while (*comma && *comma != ',')
                comma++;
            LONG pcp = 0;
            BOOL ok = TRUE;
            if (*comma == ',')
            {
                *comma = '\0';
                char *pcpStr = comma + 1;
                while (*pcpStr == ' ' || *pcpStr == '\t')
                    pcpStr++;
                ok = StrToLong((STRPTR)pcpStr, &pcp) && pcp >= 0 && pcp <= 7;
            }
            LONG vid;
            if (ok && StrToLong((STRPTR)val, &vid) && vid >= 1 && vid <= 4094)
                cfg->cfg_VlanTci = ((pcp & 7) << 13) | (vid & 0xFFF);
            else
                Kprintf("[bsdsocket] netstack.prefs: bad VLAN '%s'\n", val);
        }
        /* unknown keys are ignored (incl. future IFn_ prefixes) */
    }

    Close(fh);
    CloseLibrary((struct Library *)DOSBase);

    /* fail-safe: an incomplete static config must not leave the machine
     * unreachable — fall back to DHCP */
    if (staticMode &&
        (ip4_addr_isany_val(cfg->cfg_Addr) || ip4_addr_isany_val(cfg->cfg_Mask)))
    {
        Kprintf("[bsdsocket] netstack.prefs: MODE=STATIC needs ADDRESS and "
                "NETMASK — falling back to DHCP\n");
        staticMode = FALSE;
    }
    cfg->cfg_Dhcp = !staticMode;
}
