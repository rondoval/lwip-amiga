/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * AddNetInterface — add network interfaces to the lwip-amiga stack from
 * DEVS:NetInterfaces/ configuration files, Roadshow-style:
 *
 *   AddNetInterface genet
 *   AddNetInterface DEVS:NetInterfaces/~(#?.info) QUIET
 *
 * Template: INTERFACE/M,QUIET/S,TIMEOUT/K/N. INTERFACE is an interface name
 * (a bare name resolves against DEVS:NetInterfaces/ only — the active
 * drawer), a file path, or an AmigaDOS pattern; the FILE NAME is the
 * interface name (max 15 characters). Multiple interfaces are ordered by
 * the config file icon's PRI/PRIORITY tooltype (higher first), ties
 * alphabetically.
 *
 * Two passes, like Roadshow: every file is located and parsed first; any
 * error there stops the run before the stack is touched. Then each
 * interface is added over the stack's control port (netstack_ctl.h); the
 * add blocks until the interface is operational — link up (static) or DHCP
 * lease bound — or TIMEOUT (default TIMEOUT_MIN s) expires. On timeout the
 * interface stays up and keeps trying in the background (exit code 5,
 * "warn").
 *
 * The config file format reads Roadshow interface files unchanged: one
 * option per line — the option name, '=' and/or blanks, the value — with
 * '#'/';' comment lines. An unknown option is a warning and is skipped; a
 * bad value for an option that matters here is an error. Roadshow options
 * the stack has no use for are accepted and ignored (FILE_TEMPLATE). Added
 * beyond Roadshow's: TYPE, GATEWAY, VLAN. A bare DEVICE name is found in
 * DEVS:Networks/. A file with STATE=DOWN is parsed but not added. DNS is
 * stack-wide: netstack.prefs.
 *
 * Workbench: set AddNetInterface as the Default Tool of an interface file;
 * QUIET / TIMEOUT / PRI come from the project icon's tooltypes and every
 * message is a requester. Opening bsdsocket.library here is what boots the
 * (loopback-only) stack when it is not running yet.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dos/dos.h>
#include <dos/dosasl.h>
#include <dos/rdargs.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/intuition.h>

#include "netctl_client.h"

struct Library *IconBase;             /* optional: PRI ordering, tooltypes */
struct IntuitionBase *IntuitionBase;  /* optional: Workbench requesters */

#define ARG_TEMPLATE "INTERFACE/M,QUIET/S,TIMEOUT/K/N"
enum
{
    ARG_INTERFACE,
    ARG_QUIET,
    ARG_TIMEOUT,
    ARG_COUNT
};

/* Config file options, looked up with FindArg(): the index is the enum
 * position. Everything from K_IGNORED on is a Roadshow option that has no
 * effect here (its value is not even looked at); from K_WARNED on it asks
 * for something this stack does not do, which is worth a warning. */
#define FILE_TEMPLATE \
    "DEVICE,UNIT,TYPE,ADDRESS,NETMASK,GATEWAY,CONFIGURE,MTU,VLAN,ID,STATE,HARDWAREADDRESS," \
    "IPREQUESTS,WRITEREQUESTS,ARPREQUESTS,METRIC,IPTYPE,ARPTYPE,DEBUG,DOWNGOESOFFLINE," \
    "REPORTOFFLINE,REQUIRESINITDELAY,DHCPUNICAST,POINTTOPOINT,MULTICAST,COPYMODE,FILTER," \
    "HARDWARETYPE," \
    "ALIAS,BROADCASTADDRESS,DESTINATION=DESTINATIONADDRESS,LEASE,LINKSTATUSCOMMAND"
enum
{
    K_DEVICE,
    K_UNIT,
    K_TYPE,
    K_ADDRESS,
    K_NETMASK,
    K_GATEWAY,
    K_CONFIGURE,
    K_MTU,
    K_VLAN,
    K_ID,
    K_STATE,
    K_HWADDR,
    K_IGNORED,                 /* IPREQUESTS: 16 options that only tune Roadshow */
    K_WARNED = K_IGNORED + 16  /* ALIAS: features the stack lacks */
};

#define MAX_IFS       16
#define TIMEOUT_MIN   30
#define PATH_MAX_LEN  256
#define LINE_MAX_LEN  512

struct IfEntry
{
    char path[PATH_MAX_LEN];
    LONG pri;  /* icon PRI/PRIORITY tooltype; higher runs first */
    BOOL skip; /* parsed fine, but STATE=DOWN: not added */
    struct NetCtlIfConfig cfg;
};

static struct IfEntry ifs[MAX_IFS];
static struct IfEntry *order[MAX_IFS]; /* ifs[] in run order */
static ULONG numIfs;

static BOOL fromWb; /* started from Workbench: report through requesters */
static BOOL quiet;
static LONG timeoutSecs = TIMEOUT_MIN;
static struct FileInfoBlock *fib;

static char msgbuf[512];

static void report(BOOL error, const char *fmt, ...)
{
    if (quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
    va_end(ap);

    if (fromWb)
    {
        if (IntuitionBase != NULL)
        {
            struct EasyStruct es = {
                sizeof(struct EasyStruct), 0,
                (UBYTE *)"AddNetInterface", (UBYTE *)"%s", (UBYTE *)"OK",
            };
            APTR earg[1] = { msgbuf };
            EasyRequestArgs(NULL, &es, NULL, (APTR)earg);
        }
        return;
    }
    fprintf(error ? stderr : stdout, "AddNetInterface: %s\n", msgbuf);
}

static BOOL file_exists(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == 0)
        return FALSE;
    BOOL isFile = Examine(lock, fib) && fib->fib_DirEntryType < 0;
    UnLock(lock);
    return isFile;
}

/* PRI/PRIORITY tooltype of the config file's icon; 0 without one */
static LONG icon_pri(const char *path)
{
    if (IconBase == NULL)
        return 0;
    struct DiskObject *dob = GetDiskObject((STRPTR)path);
    if (dob == NULL)
        return 0;

    LONG pri = 0;
    if (dob->do_ToolTypes != NULL)
    {
        STRPTR v = FindToolType((CONST_STRPTR *)dob->do_ToolTypes, (CONST_STRPTR) "PRI");
        if (v == NULL)
            v = FindToolType((CONST_STRPTR *)dob->do_ToolTypes, (CONST_STRPTR) "PRIORITY");
        if (v != NULL)
            StrToLong(v, &pri);
    }
    FreeDiskObject(dob);
    return pri;
}

/* ------------------------------------------------------------------------ */
/* Config file parser                                                       */

struct Parse
{
    struct NetCtlIfConfig *cfg;
    const char *path;
    LONG line;        /* current line; 0 = a message about the whole file */
    const char *opt;  /* option name of the current line (points into it) */
    ULONG staticAddr; /* a dotted-quad ADDRESS; 0 = none seen */
    BOOL dhcp;        /* ADDRESS=DHCP, NETMASK=DHCP or CONFIGURE=DHCP seen */
    BOOL down;        /* STATE=DOWN or OFFLINE */
};

/* One diagnostic about the current line (or the file when line == 0). An
 * error stops the file, a warning does not: the result is "go on". */
static BOOL say(struct Parse *ps, BOOL error, const char *msg)
{
    const char *pfx = error ? "" : "warning: ";
    if (ps->line > 0)
        report(error, "%s'%s' line %ld: %s: %s", pfx, ps->path, (long)ps->line, ps->opt, msg);
    else
        report(error, "%s'%s': %s", pfx, ps->path, msg);
    return !error;
}

/* the whole of @s is one decimal number */
static BOOL num(const char *s, LONG *out)
{
    LONG n = StrToLong((CONST_STRPTR)s, out);
    return n > 0 && s[n] == '\0';
}

/* dotted quad -> network-byte-order word (68k is big-endian) */
static BOOL aton(const char *s, ULONG *out)
{
    ULONG v = 0;
    for (int i = 0; i < 4; i++)
    {
        LONG octet;
        LONG n = StrToLong((CONST_STRPTR)s, &octet);
        if (n <= 0 || octet < 0 || octet > 255 || s[n] != (i < 3 ? '.' : '\0'))
            return FALSE;
        v = (v << 8) | (ULONG)octet;
        s += n + 1;
    }
    *out = v;
    return TRUE;
}

/* six hex octets separated by ':' or '-' */
static BOOL mac(const char *s, UBYTE *out)
{
    for (int i = 0; i < 6; i++)
    {
        char *end;
        ULONG v = strtoul(s, &end, 16);
        if (end - s < 1 || end - s > 2)
            return FALSE;
        if (i < 5 ? (*end != ':' && *end != '-') : *end != '\0')
            return FALSE;
        out[i] = (UBYTE)v;
        s = end + 1;
    }
    return TRUE;
}

/* One line as read (newline included; modified in place). FALSE = stop. */
static BOOL parse_line(struct Parse *ps, char *line)
{
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1]))
        line[--len] = '\0';

    char *p = line;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '\0' || *p == '#' || *p == ';')
        return TRUE;

    /* option name up to a blank or '='; any run of both separates the value */
    char *key = p;
    while (*p != '\0' && *p != '=' && !isspace((unsigned char)*p))
        p++;
    char *val = p;
    while (*val == '=' || isspace((unsigned char)*val))
        val++;
    *p = '\0';
    ps->opt = key;

    LONG k = FindArg((CONST_STRPTR)FILE_TEMPLATE, (CONST_STRPTR)key);
    if (k < 0)
        return say(ps, FALSE, "unknown option, ignored");
    if (k >= K_WARNED)
        return say(ps, FALSE, "not supported, ignored");
    if (k >= K_IGNORED)
        return TRUE;
    if (*val == '\0')
        return say(ps, TRUE, "needs a value");

    struct NetCtlIfConfig *cfg = ps->cfg;
    LONG n;
    switch (k)
    {
    case K_DEVICE:
        if (strlen(val) > NETCTL_DEV_MAX - 1)
            return say(ps, TRUE, "longer than 63 characters");
        strcpy(cfg->nif_Device, val);
        return TRUE;
    case K_UNIT:
        if (num(val, &cfg->nif_Unit))
            return TRUE;
        break;
    case K_TYPE:
        n = FindArg((CONST_STRPTR) "AUTO,NETDEV,SANA2", (CONST_STRPTR)val); /* = NETCTL_TYPE_* */
        if (n >= 0)
        {
            cfg->nif_Type = n;
            return TRUE;
        }
        break;
    case K_ADDRESS:
        if (stricmp(val, "DHCP") == 0)
        {
            ps->dhcp = TRUE;
            return TRUE;
        }
        if (aton(val, &ps->staticAddr))
            return TRUE;
        break;
    case K_NETMASK:
        if (stricmp(val, "DHCP") == 0)
        {
            ps->dhcp = TRUE;
            return TRUE;
        }
        if (aton(val, &cfg->nif_Mask))
        {
            cfg->nif_Flags |= NETCTL_IFF_HAS_MASK;
            return TRUE;
        }
        break;
    case K_GATEWAY:
        if (aton(val, &cfg->nif_Gateway))
        {
            cfg->nif_Flags |= NETCTL_IFF_HAS_GW;
            return TRUE;
        }
        break;
    case K_CONFIGURE:
        n = FindArg((CONST_STRPTR) "DHCP,AUTO=SLOWAUTO,FASTAUTO", (CONST_STRPTR)val);
        if (n > 0)
            return say(ps, TRUE, "ZeroConf is not supported - use DHCP or a fixed ADDRESS");
        if (n == 0)
        {
            ps->dhcp = TRUE;
            return TRUE;
        }
        break;
    case K_MTU: /* 0 = the driver's own */
        if (num(val, &cfg->nif_Mtu) && cfg->nif_Mtu >= 0)
        {
            if (cfg->nif_Mtu > 0)
                cfg->nif_Flags |= NETCTL_IFF_HAS_MTU;
            else
                cfg->nif_Flags &= ~NETCTL_IFF_HAS_MTU;
            return TRUE;
        }
        break;
    case K_VLAN: /* <vid>[,<pcp>]: vid 1..4094, pcp 0..7 -> TCI (pcp<<13)|vid */
    {
        char *pcpStr = strchr(val, ',');
        if (pcpStr != NULL)
            *pcpStr++ = '\0';
        LONG vid, pcp = 0;
        if (!num(val, &vid) || vid < 1 || vid > 4094)
            break;
        if (pcpStr != NULL && (!num(pcpStr, &pcp) || pcp < 0 || pcp > 7))
            break;
        cfg->nif_VlanTci = (pcp << 13) | vid;
        return TRUE;
    }
    case K_ID:
        n = (LONG)strlen(val);
        if (n < 2 || n > NETCTL_ID_MAX - 1)
            return say(ps, TRUE, "must be 2 to 63 characters long");
        strcpy(cfg->nif_Id, val);
        return TRUE;
    case K_STATE:
        n = FindArg((CONST_STRPTR) "UP=ONLINE,DOWN=OFFLINE", (CONST_STRPTR)val);
        if (n >= 0)
        {
            ps->down = n == 1;
            return TRUE;
        }
        break;
    case K_HWADDR:
        if (mac(val, cfg->nif_HwAddr) && netctl_mac_usable(cfg->nif_HwAddr))
        {
            cfg->nif_Flags |= NETCTL_IFF_HAS_HWADDR;
            return TRUE;
        }
        break;
    }
    return say(ps, TRUE, "bad value");
}

/* Whole-file checks after the last line. FALSE = the file is unusable. */
static BOOL parse_end(struct Parse *ps)
{
    struct NetCtlIfConfig *cfg = ps->cfg;
    if (cfg->nif_Device[0] == '\0')
        return say(ps, TRUE, "DEVICE is required");

    /* no address at all is DHCP too — the one thing an address-less
     * interface can usefully do here */
    BOOL dhcp = ps->dhcp || ps->staticAddr == 0;
    if (!dhcp && !(cfg->nif_Flags & NETCTL_IFF_HAS_MASK))
        return say(ps, TRUE, "a fixed ADDRESS needs a NETMASK");
    if (!dhcp)
    {
        cfg->nif_Addr = ps->staticAddr;
        return TRUE;
    }
    cfg->nif_Flags |= NETCTL_IFF_DHCP;
    if (ps->staticAddr != 0)
        return say(ps, FALSE, "ADDRESS ignored, DHCP assigns the address");
    return TRUE;
}

/* Parse one interface file into @cfg; the FILE NAME is the interface name.
 * FALSE = an error was reported. *skip is set for a STATE=DOWN file. */
static BOOL parse_file(const char *path, struct NetCtlIfConfig *cfg, BOOL *skip)
{
    static char line[LINE_MAX_LEN]; /* one file at a time */

    const char *name = (const char *)FilePart((CONST_STRPTR)path);
    size_t nameLen = strlen(name);
    if (nameLen == 0 || nameLen > NETCTL_IFNAME_MAX - 1)
    {
        report(TRUE, "'%s': the file name is the interface name and must be 1 to %d "
                     "characters long", path, NETCTL_IFNAME_MAX - 1);
        return FALSE;
    }
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->nif_Name, name);
    cfg->nif_VlanTci = -1;

    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh == 0)
    {
        report(TRUE, "cannot open '%s'", path);
        return FALSE;
    }

    struct Parse ps = { .cfg = cfg, .path = path };
    BOOL ok = TRUE;
    while (ok && FGets(fh, (STRPTR)line, sizeof(line) - 1) != NULL)
    {
        ps.line++;
        ok = parse_line(&ps, line);
    }
    Close(fh);

    ps.line = 0;
    if (!ok || !parse_end(&ps))
        return FALSE;

    *skip = ps.down;
    if (ps.down)
        report(FALSE, "'%s': STATE=DOWN, interface not added", path);
    return TRUE;
}

/* ------------------------------------------------------------------------ */

static BOOL add_entry(const char *path)
{
    if (numIfs >= MAX_IFS)
    {
        report(TRUE, "too many interface files (max %d)", MAX_IFS);
        return FALSE;
    }
    struct IfEntry *e = &ifs[numIfs];
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->pri = icon_pri(path);
    if (!parse_file(path, &e->cfg, &e->skip))
        return FALSE;
    numIfs++;
    return TRUE;
}

/* one INTERFACE argument: a pattern, a path, or a bare name to resolve
 * against the standard drawers */
static BOOL collect_arg(const char *arg)
{
    static char patbuf[PATH_MAX_LEN * 2];
    static char path[PATH_MAX_LEN];

    LONG isPat = ParsePatternNoCase((CONST_STRPTR)arg, (STRPTR)patbuf, sizeof(patbuf));
    if (isPat < 0)
    {
        report(TRUE, "bad pattern '%s'", arg);
        return FALSE;
    }
    if (isPat > 0)
    {
        struct AnchorPath *ap =
            AllocMem(sizeof(struct AnchorPath) + PATH_MAX_LEN, MEMF_PUBLIC | MEMF_CLEAR);
        if (ap == NULL)
        {
            report(TRUE, "out of memory");
            return FALSE;
        }
        ap->ap_Strlen = PATH_MAX_LEN - 1;
        ap->ap_BreakBits = SIGBREAKF_CTRL_C;

        BOOL ok = TRUE, any = FALSE;
        LONG err = MatchFirst((STRPTR)arg, ap);
        while (err == 0)
        {
            if (ap->ap_Info.fib_DirEntryType < 0) /* files only */
            {
                any = TRUE;
                if (!add_entry((char *)ap->ap_Buf))
                {
                    ok = FALSE;
                    break;
                }
            }
            err = MatchNext(ap);
        }
        MatchEnd(ap);
        FreeMem(ap, sizeof(struct AnchorPath) + PATH_MAX_LEN);
        if (ok && !any)
        {
            report(TRUE, "no interface files match '%s'", arg);
            ok = FALSE;
        }
        return ok;
    }

    /* bare name: only the ACTIVE drawer - a config parked in
     * SYS:Storage/NetInterfaces is deliberately not resolvable by name
     * (activate it by moving it to DEVS:NetInterfaces;
     * an explicit path still works) */
    if (FilePart((CONST_STRPTR)arg) == (STRPTR)arg)
    {
        strcpy(path, "DEVS:NetInterfaces");
        if (AddPart((STRPTR)path, (CONST_STRPTR)arg, sizeof(path)) && file_exists(path))
            return add_entry(path);
        report(TRUE, "interface '%s' not found (looked in DEVS:NetInterfaces)", arg);
        return FALSE;
    }

    if (file_exists(arg))
        return add_entry(arg);

    report(TRUE, "interface file '%s' not found", arg);
    return FALSE;
}

/* order[]: higher PRI first, ties alphabetically (insertion sort; tiny list) */
static void sort_entries(void)
{
    for (ULONG i = 0; i < numIfs; i++)
    {
        struct IfEntry *e = &ifs[i];
        ULONG j = i;
        while (j > 0 &&
               (order[j - 1]->pri < e->pri ||
                (order[j - 1]->pri == e->pri &&
                 strcmp(order[j - 1]->cfg.nif_Name, e->cfg.nif_Name) > 0)))
        {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = e;
    }
}

static BOOL wb_collect(struct WBStartup *wbs)
{
    static char wbPath[PATH_MAX_LEN];

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR) "intuition.library", 36);

    if (wbs->sm_NumArgs < 2)
    {
        report(TRUE, "AddNetInterface is a project tool: make it the Default Tool "
                     "of an interface file in DEVS:NetInterfaces.");
        return FALSE;
    }

    for (LONG i = 1; i < wbs->sm_NumArgs; i++)
    {
        struct WBArg *wa = &wbs->sm_ArgList[i];
        if (wa->wa_Lock == 0 || wa->wa_Name == NULL || wa->wa_Name[0] == '\0')
            continue;

        BPTR old = CurrentDir(wa->wa_Lock);

        /* QUIET / TIMEOUT come from the project icon's tooltypes */
        if (IconBase != NULL)
        {
            struct DiskObject *dob = GetDiskObject(wa->wa_Name);
            if (dob != NULL)
            {
                if (dob->do_ToolTypes != NULL)
                {
                    if (FindToolType((CONST_STRPTR *)dob->do_ToolTypes,
                                     (CONST_STRPTR) "QUIET") != NULL)
                        quiet = TRUE;
                    STRPTR v = FindToolType((CONST_STRPTR *)dob->do_ToolTypes,
                                            (CONST_STRPTR) "TIMEOUT");
                    LONG t;
                    if (v != NULL && StrToLong(v, &t) > 0 && t >= TIMEOUT_MIN)
                        timeoutSecs = t;
                }
                FreeDiskObject(dob);
            }
        }

        BOOL ok = NameFromLock(wa->wa_Lock, (STRPTR)wbPath, sizeof(wbPath)) &&
                  AddPart((STRPTR)wbPath, wa->wa_Name, sizeof(wbPath));
        CurrentDir(old);
        if (!ok || !collect_arg(wbPath))
            return FALSE;
    }
    return numIfs > 0;
}

/* A shut-down library whose RemLibrary was
 * missed would otherwise satisfy OpenLibrary with a stack-less husk. With
 * no clients an expunge either succeeds (stack stopped: fresh load next)
 * or is refused by a healthy running stack — both are what we want. */
static void flush_dead_library(void)
{
    Forbid();
    struct Library *lib =
        (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR) "bsdsocket.library");
    if (lib != NULL && lib->lib_OpenCnt == 0)
        RemLibrary(lib);
    Permit();
}

/* Add one interface; blocks for DHCP up to timeoutSecs. Returns the exit
 * code; sets *broke on Ctrl-C (the caller stops the whole run). */
static int run_add(struct IfEntry *e, struct MsgPort *reply,
                   struct timerequest *treq, struct MsgPort *timerPort, BOOL *broke)
{
    static struct NetCtlMsg msg, cancel; /* one add in flight at a time */

    netctl_msg_init(&msg, reply, NETCTL_OP_ADD_IF);
    msg.ncm_Config = e->cfg;

    if (!netctl_send(&msg))
    {
        report(TRUE, "the network stack is not running");
        return RETURN_FAIL;
    }

    treq->tr_node.io_Command = TR_ADDREQUEST;
    treq->tr_time.tv_secs = (ULONG)timeoutSecs;
    treq->tr_time.tv_micro = 0;
    SendIO(&treq->tr_node);

    ULONG replySig = 1UL << reply->mp_SigBit;
    ULONG timerSig = 1UL << timerPort->mp_SigBit;
    BOOL timedOut = FALSE;
    for (;;)
    {
        ULONG sigs = Wait(replySig | timerSig | SIGBREAKF_CTRL_C);
        if (GetMsg(reply) != NULL)
            break; /* only the ADD is in flight: that was its reply */

        if (sigs & SIGBREAKF_CTRL_C)
            *broke = TRUE;
        if ((sigs & timerSig) && CheckIO(&treq->tr_node) != NULL)
            timedOut = TRUE;
        if (*broke || timedOut)
        {
            /* recall the wait; the interface stays and keeps trying. The
             * ADD may still win the race — its result decides below. */
            netctl_msg_init(&cancel, reply, NETCTL_OP_CANCEL_ADD);
            netctl_drain(reply, netctl_send(&cancel) ? 2 : 1);
            break;
        }
    }

    AbortIO(&treq->tr_node);
    WaitIO(&treq->tr_node);
    SetSignal(0, timerSig);

    LONG res = msg.ncm_Result;
    const char *name = e->cfg.nif_Name;
    switch (res)
    {
    case NETCTL_OK:
    {
        char addr[16];
        netctl_ntoa(msg.ncm_AddrOut, addr);
        report(FALSE, "interface '%s' configured, address %s", name, addr);
        return RETURN_OK;
    }
    case NETCTL_ERR_PENDING:
        if (e->cfg.nif_Flags & NETCTL_IFF_DHCP)
            report(FALSE, "interface '%s' is up but has no DHCP lease yet - "
                          "it keeps trying in the background", name);
        else
            report(FALSE, "interface '%s' is configured but the link is not "
                          "up yet - it becomes usable when the link comes up", name);
        return RETURN_WARN;
    case NETCTL_ERR_EXISTS:
        report(TRUE, "interface '%s': an interface is already installed "
                     "(RemoveNetInterface first)", name);
        return RETURN_ERROR;
    case NETCTL_ERR_DEVICE:
        report(TRUE, "interface '%s': cannot start %s unit %ld (device error %ld)",
               name, e->cfg.nif_Device, (long)e->cfg.nif_Unit, (long)msg.ncm_Aux);
        return RETURN_ERROR;
    default:
        report(TRUE, "interface '%s': %s", name, netctl_strerror(res));
        return RETURN_ERROR;
    }
}

int main(int argc, char **argv)
{
    int rc = RETURN_OK;
    struct RDArgs *rda = NULL;

    IconBase = OpenLibrary((CONST_STRPTR) "icon.library", 36);
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib == NULL)
        return RETURN_FAIL;

    if (argc == 0)
    {
        fromWb = TRUE;
        if (!wb_collect((struct WBStartup *)argv))
            rc = RETURN_FAIL;
    }
    else
    {
        LONG args[ARG_COUNT];
        memset(args, 0, sizeof(args));
        rda = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, args, NULL);
        if (rda == NULL)
        {
            PrintFault(IoErr(), (CONST_STRPTR) "AddNetInterface");
            FreeDosObject(DOS_FIB, fib);
            if (IconBase != NULL)
                CloseLibrary(IconBase);
            return RETURN_ERROR;
        }
        quiet = args[ARG_QUIET] != 0;
        if (args[ARG_TIMEOUT] != 0)
        {
            timeoutSecs = *(LONG *)args[ARG_TIMEOUT];
            if (timeoutSecs < TIMEOUT_MIN)
            {
                report(FALSE, "TIMEOUT raised to the minimum of %d seconds", TIMEOUT_MIN);
                timeoutSecs = TIMEOUT_MIN;
            }
        }

        STRPTR *names = (STRPTR *)args[ARG_INTERFACE];
        if (names == NULL || names[0] == NULL)
        {
            report(TRUE, "no interface given (usage: AddNetInterface <name|file|pattern>...)");
            rc = RETURN_ERROR;
        }
        else
        {
            for (ULONG i = 0; names[i] != NULL; i++)
            {
                if (!collect_arg((const char *)names[i]))
                {
                    rc = RETURN_ERROR;
                    break;
                }
            }
        }
    }

    ULONG numActive = 0;
    for (ULONG i = 0; rc == RETURN_OK && i < numIfs; i++)
        numActive += ifs[i].skip ? 0 : 1;

    /* STATE=DOWN files only: nothing to add, and no reason to boot the stack */
    if (rc == RETURN_OK && numActive > 0)
    {
        sort_entries();
        flush_dead_library();

        /* what boots the (loopback-only) stack if it is not up yet */
        struct Library *sockBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
        if (sockBase == NULL)
        {
            report(TRUE, "cannot open bsdsocket.library v4 - is it installed in LIBS:?");
            rc = RETURN_FAIL;
        }
        else
        {
            struct MsgPort *reply = CreateMsgPort();
            struct MsgPort *timerPort = CreateMsgPort();
            struct timerequest *treq = (struct timerequest *)CreateIORequest(
                timerPort, sizeof(struct timerequest));
            BOOL timerOpen =
                treq != NULL &&
                OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, &treq->tr_node, 0) == 0;

            if (reply == NULL || !timerOpen)
            {
                report(TRUE, "out of memory");
                rc = RETURN_FAIL;
            }
            else
            {
                BOOL broke = FALSE;
                for (ULONG i = 0; i < numIfs && !broke; i++)
                {
                    if (order[i]->skip)
                        continue;
                    int r = run_add(order[i], reply, treq, timerPort, &broke);
                    if (r > rc)
                        rc = r;
                }
                if (broke)
                {
                    PrintFault(ERROR_BREAK, (CONST_STRPTR) "AddNetInterface");
                    if (rc < RETURN_WARN)
                        rc = RETURN_WARN;
                }
            }

            if (timerOpen)
                CloseDevice(&treq->tr_node);
            if (treq != NULL)
                DeleteIORequest(&treq->tr_node);
            if (timerPort != NULL)
                DeleteMsgPort(timerPort);
            if (reply != NULL)
                DeleteMsgPort(reply);
            CloseLibrary(sockBase);
        }
    }

    if (rda != NULL)
        FreeArgs(rda);
    FreeDosObject(DOS_FIB, fib);
    if (IntuitionBase != NULL)
        CloseLibrary((struct Library *)IntuitionBase);
    if (IconBase != NULL)
        CloseLibrary(IconBase);

    /* QUIET demotes every failure to a testable warn (IF WARN in scripts) */
    if (quiet && rc > RETURN_WARN)
        rc = RETURN_WARN;
    return rc;
}
