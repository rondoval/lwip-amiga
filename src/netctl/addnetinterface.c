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
 * The config file format is lwip-amiga's own — one option per line, '#'/';'
 * comment lines, and an unknown option is an error:
 *   DEVICE/K (required), UNIT/K/N, TYPE/K (AUTO | NETDEV | SANA2; omitted =
 *   AUTO, which probes the device), ADDRESS/K (dotted quad | DHCP; omitted =
 *   DHCP), NETMASK/K + GATEWAY/K (static), MTU/K/N, VLAN/K (vid[,pcp]),
 *   ID/K (per-interface DHCP hostname). DNS is stack-wide: netstack.prefs.
 *
 * Workbench: set AddNetInterface as the Default Tool of an interface file;
 * QUIET / TIMEOUT / PRI come from the project icon's tooltypes and errors
 * show as requesters. Opening bsdsocket.library here is what boots the
 * (loopback-only) stack when it is not running yet.
 */

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

#define FILE_TEMPLATE \
    "DEVICE/K,UNIT/K/N,TYPE/K,ADDRESS/K,NETMASK/K,GATEWAY/K,MTU/K/N,VLAN/K,ID/K"
enum
{
    FA_DEVICE,
    FA_UNIT,
    FA_TYPE,
    FA_ADDRESS,
    FA_NETMASK,
    FA_GATEWAY,
    FA_MTU,
    FA_VLAN,
    FA_ID,
    FA_COUNT
};

#define MAX_IFS       16
#define TIMEOUT_MIN   30
#define PATH_MAX_LEN  256

struct IfEntry
{
    char path[PATH_MAX_LEN];
    LONG pri; /* icon PRI/PRIORITY tooltype; higher runs first */
    struct NetCtlIfConfig cfg;
};

static struct IfEntry ifs[MAX_IFS];
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
    vsprintf(msgbuf, fmt, ap);
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

/* VLAN = <vid>[,<pcp>]  (vid 1..4094, pcp 0..7) -> (pcp<<13)|vid */
static BOOL parse_vlan(const char *val, LONG *tci)
{
    char buf[32];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    LONG pcp = 0;
    char *comma = strchr(buf, ',');
    if (comma != NULL)
    {
        *comma = '\0';
        if (StrToLong((STRPTR)(comma + 1), &pcp) <= 0 || pcp < 0 || pcp > 7)
            return FALSE;
    }
    LONG vid;
    if (StrToLong((STRPTR)buf, &vid) <= 0 || vid < 1 || vid > 4094)
        return FALSE;
    *tci = ((pcp & 7) << 13) | (vid & 0xFFF);
    return TRUE;
}

/* Parse one interface file into @cfg. Strict: any unknown or malformed
 * option is an error with file and line number — a typo must not silently
 * come up misconfigured. */
static BOOL parse_file(const char *path, struct NetCtlIfConfig *cfg)
{
    const char *name = (const char *)FilePart((CONST_STRPTR)path);
    ULONG nameLen = strlen(name);
    if (nameLen == 0 || nameLen > NETCTL_IFNAME_MAX - 1)
    {
        report(TRUE, "'%s': interface name '%s' is longer than %d characters",
               path, name, NETCTL_IFNAME_MAX - 1);
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
    struct RDArgs *rda = AllocDosObject(DOS_RDARGS, NULL);
    if (rda == NULL)
    {
        Close(fh);
        report(TRUE, "out of memory");
        return FALSE;
    }

    BOOL ok = TRUE;
    BOOL dhcp = FALSE, haveAddr = FALSE;
    LONG lineNo = 0;
    char line[258];
    while (ok && FGets(fh, (STRPTR)line, sizeof(line) - 2) != NULL)
    {
        lineNo++;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#' || *p == ';')
            continue;

        /* ReadArgs needs the buffer newline-terminated */
        ULONG len = strlen(p);
        if (p[len - 1] != '\n')
        {
            p[len] = '\n';
            p[++len] = '\0';
        }

        LONG vals[FA_COUNT];
        memset(vals, 0, sizeof(vals));
        rda->RDA_Source.CS_Buffer = (UBYTE *)p;
        rda->RDA_Source.CS_Length = (LONG)len;
        rda->RDA_Source.CS_CurChr = 0;
        rda->RDA_DAList = 0;
        rda->RDA_Flags = RDAF_NOPROMPT;

        if (ReadArgs((CONST_STRPTR)FILE_TEMPLATE, vals, rda) == NULL)
        {
            report(TRUE, "'%s' line %ld: unknown or malformed option", path, lineNo);
            ok = FALSE;
            break;
        }

        if (vals[FA_DEVICE] != 0)
            strncpy(cfg->nif_Device, (char *)vals[FA_DEVICE], NETCTL_DEV_MAX - 1);
        if (vals[FA_UNIT] != 0)
            cfg->nif_Unit = *(LONG *)vals[FA_UNIT];
        if (vals[FA_TYPE] != 0)
        {
            const char *v = (char *)vals[FA_TYPE];
            if (stricmp(v, "AUTO") == 0)
                cfg->nif_Type = NETCTL_TYPE_AUTO;
            else if (stricmp(v, "NETDEV") == 0)
                cfg->nif_Type = NETCTL_TYPE_NETDEV;
            else if (stricmp(v, "SANA2") == 0)
                cfg->nif_Type = NETCTL_TYPE_SANA2;
            else
            {
                report(TRUE, "'%s' line %ld: bad TYPE '%s' (AUTO | NETDEV | SANA2)",
                       path, lineNo, v);
                ok = FALSE;
            }
        }
        if (ok && vals[FA_ADDRESS] != 0)
        {
            const char *v = (char *)vals[FA_ADDRESS];
            if (stricmp(v, "DHCP") == 0)
            {
                dhcp = TRUE;
                haveAddr = TRUE;
            }
            else if (netctl_aton(v, &cfg->nif_Addr))
            {
                dhcp = FALSE;
                haveAddr = TRUE;
            }
            else
            {
                report(TRUE, "'%s' line %ld: bad ADDRESS '%s'", path, lineNo, v);
                ok = FALSE;
            }
        }
        if (ok && vals[FA_NETMASK] != 0)
        {
            if (netctl_aton((char *)vals[FA_NETMASK], &cfg->nif_Mask))
                cfg->nif_Flags |= NETCTL_IFF_HAS_MASK;
            else
            {
                report(TRUE, "'%s' line %ld: bad NETMASK", path, lineNo);
                ok = FALSE;
            }
        }
        if (ok && vals[FA_GATEWAY] != 0)
        {
            if (netctl_aton((char *)vals[FA_GATEWAY], &cfg->nif_Gateway))
                cfg->nif_Flags |= NETCTL_IFF_HAS_GW;
            else
            {
                report(TRUE, "'%s' line %ld: bad GATEWAY", path, lineNo);
                ok = FALSE;
            }
        }
        if (ok && vals[FA_MTU] != 0)
        {
            LONG mtu = *(LONG *)vals[FA_MTU];
            if (mtu > 0)
            {
                cfg->nif_Mtu = mtu;
                cfg->nif_Flags |= NETCTL_IFF_HAS_MTU;
            }
            else
            {
                report(TRUE, "'%s' line %ld: bad MTU", path, lineNo);
                ok = FALSE;
            }
        }
        if (ok && vals[FA_VLAN] != 0)
        {
            if (!parse_vlan((char *)vals[FA_VLAN], &cfg->nif_VlanTci))
            {
                report(TRUE, "'%s' line %ld: bad VLAN (vid[,pcp])", path, lineNo);
                ok = FALSE;
            }
        }
        if (ok && vals[FA_ID] != 0)
            strncpy(cfg->nif_Id, (char *)vals[FA_ID], NETCTL_ID_MAX - 1);

        FreeArgs(rda);
    }

    FreeDosObject(DOS_RDARGS, rda);
    Close(fh);
    if (!ok)
        return FALSE;

    if (cfg->nif_Device[0] == '\0')
    {
        report(TRUE, "'%s': DEVICE is required", path);
        return FALSE;
    }
    if (!haveAddr || dhcp)
        cfg->nif_Flags |= NETCTL_IFF_DHCP;
    else if (!(cfg->nif_Flags & NETCTL_IFF_HAS_MASK))
    {
        report(TRUE, "'%s': a static ADDRESS needs a NETMASK", path);
        return FALSE;
    }
    return TRUE;
}

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
    if (!parse_file(path, &e->cfg))
        return FALSE;
    numIfs++;
    return TRUE;
}

/* one INTERFACE argument: a pattern, a path, or a bare name to resolve
 * against the standard drawers */
static BOOL collect_arg(const char *arg)
{
    char patbuf[PATH_MAX_LEN * 2];
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

    if (file_exists(arg))
        return add_entry(arg);

    /* bare name: only the ACTIVE drawer — a config parked in
     * SYS:Storage/NetInterfaces is deliberately not resolvable by name
     * (activate it by moving it to DEVS:NetInterfaces, AmigaOS-style;
     * an explicit path still works) */
    if (FilePart((CONST_STRPTR)arg) == (STRPTR)arg)
    {
        char path[PATH_MAX_LEN];
        strcpy(path, "DEVS:NetInterfaces");
        if (AddPart((STRPTR)path, (CONST_STRPTR)arg, sizeof(path)) && file_exists(path))
            return add_entry(path);
    }
    report(TRUE, "interface '%s' not found (looked in DEVS:NetInterfaces)", arg);
    return FALSE;
}

/* higher PRI first, ties alphabetically (insertion sort; the list is tiny) */
static void sort_entries(void)
{
    for (ULONG i = 1; i < numIfs; i++)
    {
        struct IfEntry tmp = ifs[i];
        ULONG j = i;
        while (j > 0 &&
               (ifs[j - 1].pri < tmp.pri ||
                (ifs[j - 1].pri == tmp.pri &&
                 strcmp(ifs[j - 1].cfg.nif_Name, tmp.cfg.nif_Name) > 0)))
        {
            ifs[j] = ifs[j - 1];
            j--;
        }
        ifs[j] = tmp;
    }
}

static BOOL wb_collect(struct WBStartup *wbs)
{
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

        char path[PATH_MAX_LEN];
        BOOL ok = NameFromLock(wa->wa_Lock, (STRPTR)path, sizeof(path)) &&
                  AddPart((STRPTR)path, wa->wa_Name, sizeof(path));
        CurrentDir(old);
        if (!ok || !collect_arg(path))
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
    struct NetCtlMsg msg, cancel;
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

    if (rc == RETURN_OK && numIfs > 0)
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
                    int r = run_add(&ifs[i], reply, treq, timerPort, &broke);
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
