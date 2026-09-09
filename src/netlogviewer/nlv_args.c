/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetLogViewer argument parsing. Shell: ReadArgs with the documented
 * template (CX_POPKEY/K,CX_PRIORITY/K/N,CX_POPUP/K). Workbench: the same
 * keys as tooltypes of the program's icon. The $VER cookie rides on the
 * template string the way the other tools do it.
 */

#include "nlv_args.h"

#include <stdio.h>
#include <string.h>

#include <dos/rdargs.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/icon.h>

#define VERSTAG "\0$VER: " NLV_NAME " " TOOL_VERSION " " TOOL_DATE
static const char argTemplate[] = "CX_POPKEY/K,CX_PRIORITY/K/N,CX_POPUP/K" VERSTAG;

enum
{
    ARG_POPKEY,
    ARG_PRIORITY,
    ARG_POPUP,
    ARG_COUNT
};

/* yes/no, on/off, true/false, 1/0 — case-insensitive */
static BOOL nlv_parse_bool(const char *s, BOOL *out)
{
    static const char *const yes[] = {"yes", "on", "true", "1", NULL};
    static const char *const no[] = {"no", "off", "false", "0", NULL};
    for (ULONG i = 0; yes[i] != NULL; i++)
        if (strcasecmp(s, yes[i]) == 0)
        {
            *out = TRUE;
            return TRUE;
        }
    for (ULONG i = 0; no[i] != NULL; i++)
        if (strcasecmp(s, no[i]) == 0)
        {
            *out = FALSE;
            return TRUE;
        }
    return FALSE;
}

/* nb_Pri is a BYTE */
static LONG nlv_clamp_pri(LONG p)
{
    return p < -128 ? -128 : (p > 127 ? 127 : p);
}

static BOOL nlv_args_wb(struct WBStartup *wbs, struct NlvArgs *a)
{
    if (IconBase == NULL || wbs->sm_NumArgs < 1)
        return TRUE; /* no icon access: defaults */
    struct WBArg *wa = &wbs->sm_ArgList[0];
    if (wa->wa_Lock == 0 || wa->wa_Name == NULL)
        return TRUE;

    BOOL ok = TRUE;
    BPTR old = CurrentDir(wa->wa_Lock);
    struct DiskObject *dob = GetDiskObject(wa->wa_Name);
    if (dob != NULL)
    {
        CONST_STRPTR *tt = (CONST_STRPTR *)dob->do_ToolTypes;
        if (tt != NULL)
        {
            STRPTR v = FindToolType(tt, (CONST_STRPTR) "CX_POPKEY");
            if (v != NULL)
                snprintf(a->popKey, sizeof(a->popKey), "%s", (const char *)v);
            v = FindToolType(tt, (CONST_STRPTR) "CX_PRIORITY");
            LONG p;
            if (v != NULL && StrToLong(v, &p) > 0)
                a->priority = nlv_clamp_pri(p);
            v = FindToolType(tt, (CONST_STRPTR) "CX_POPUP");
            if (v != NULL && !nlv_parse_bool((const char *)v, &a->popup))
            {
                nlv_report("CX_POPUP must be YES or NO, not '%s'", (const char *)v);
                ok = FALSE;
            }
        }
        FreeDiskObject(dob);
    }
    CurrentDir(old);
    return ok;
}

static BOOL nlv_args_cli(struct NlvArgs *a)
{
    LONG args[ARG_COUNT];
    memset(args, 0, sizeof(args));
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)argTemplate, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)NLV_NAME);
        return FALSE;
    }

    BOOL ok = TRUE;
    if (args[ARG_POPKEY] != 0)
        snprintf(a->popKey, sizeof(a->popKey), "%s", (const char *)args[ARG_POPKEY]);
    if (args[ARG_PRIORITY] != 0)
        a->priority = nlv_clamp_pri(*(LONG *)args[ARG_PRIORITY]);
    if (args[ARG_POPUP] != 0 && !nlv_parse_bool((const char *)args[ARG_POPUP], &a->popup))
    {
        nlv_report("CX_POPUP must be YES or NO, not '%s'", (const char *)args[ARG_POPUP]);
        ok = FALSE;
    }
    FreeArgs(rda);
    return ok;
}

BOOL nlv_args_parse(int argc, char **argv, struct NlvArgs *a)
{
    snprintf(a->popKey, sizeof(a->popKey), "%s", NLV_DEFAULT_POPKEY);
    a->priority = 0;
    a->popup = TRUE;

    if (argc == 0)
        return nlv_args_wb((struct WBStartup *)argv, a);
    return nlv_args_cli(a);
}
