/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GetNetStatus — query whether the network is operational, Roadshow-style.
 * Template: CHECK/K,QUIET/S.
 *
 * One SBTC_SYSTEM_STATUS query answers all six conditions. With CHECK, every
 * selected condition that is unsatisfied prints a message and the command
 * exits RETURN_WARN (5) for scripts ("IF WARN" after boot); an unknown
 * condition exits RETURN_ERROR (10), since it was never actually checked.
 * Without CHECK, prints the stack version and all six condition lines. QUIET
 * suppresses everything except error messages.
 *
 * Fresh implementation of the Roadshow command of the same name (behavior
 * per the NDK's GetNetStatus.doc); English-only. Opening bsdsocket.library
 * boots the stack loopback-only — that is the Roadshow startup model, and
 * exactly what a readiness check in a boot script wants.
 */

#include <stdio.h>
#include <string.h>

#include <dos/dos.h>
#include <exec/types.h>
#include <libraries/bsdsocket.h>
#include <utility/tagitem.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/socket.h>

struct Library *SocketBase;

/* $VER: cookie appended to the ReadArgs template (the leading NUL terminates
 * the template string). TOOL_VERSION/TOOL_DATE come from the build. */
#define VERSTAG "\0$VER: GetNetStatus " TOOL_VERSION " " TOOL_DATE

#define ARG_TEMPLATE "CHECK/K,QUIET/S" VERSTAG
enum
{
    ARG_CHECK,
    ARG_QUIET,
    ARG_COUNT
};

/* the six SBSYSSTAT_* conditions, in Roadshow's display order; the
 * unsatisfied text carries two %s slots for the emphasis on/off sequences */
static const struct condition
{
    ULONG flag;
    const char *names[3]; /* CHECK keyword + aliases, NULL-padded */
    const char *ok;
    const char *fail;
} conditions[] = {
    {SBSYSSTAT_Interfaces,
     {"INTERFACES", NULL, NULL},
     "Networking interfaces are available and configured.\n",
     "%sNo%s networking interfaces are available and configured.\n"},
    {SBSYSSTAT_PTP_Interfaces,
     {"PTPINTERFACES", "PTP", NULL},
     "Point-to-point networking interfaces are available and configured.\n",
     "%sNo%s point-to-point networking interfaces are available and "
     "configured.\n"},
    {SBSYSSTAT_BCast_Interfaces,
     {"BCASTINTERFACES", "BCAST", "BROADCAST"},
     "Broadcast networking interfaces are available and configured.\n",
     "%sNo%s broadcast networking interfaces are available and configured.\n"},
    {SBSYSSTAT_Resolver,
     {"RESOLVER", "NAMERESOLUTION", "DNS"},
     "Name resolution servers are configured.\n",
     "%sNo%s name resolution servers are configured.\n"},
    {SBSYSSTAT_Routes,
     {"ROUTES", NULL, NULL},
     "Routing information is configured.\n",
     "%sNo%s routing information is configured.\n"},
    {SBSYSSTAT_DefaultRoute,
     {"DEFAULTROUTE", "DEFAULTGATEWAY", NULL},
     "The default route is configured.\n",
     "The default route is %snot%s configured.\n"},
};
#define COND_COUNT (sizeof(conditions) / sizeof(conditions[0]))

/* case-insensitive match of a non-terminated token against a keyword */
static BOOL token_is(const char *tok, size_t len, const char *name)
{
    size_t i = 0;
    for (; i < len && name[i] != '\0'; i++)
    {
        char ca = tok[i], cb = name[i];
        if (ca >= 'a' && ca <= 'z')
            ca -= 'a' - 'A';
        if (cb >= 'a' && cb <= 'z')
            cb -= 'a' - 'A';
        if (ca != cb)
            return FALSE;
    }
    return i == len && name[i] == '\0';
}

/* advance past one comma/space-separated token; NULL when the input is spent */
static const char *next_token(const char *s, const char **tok, size_t *len)
{
    while (*s == ',' || *s == ' ' || *s == '\t')
        s++;
    if (*s == '\0')
        return NULL;
    *tok = s;
    while (*s != '\0' && *s != ',' && *s != ' ' && *s != '\t')
        s++;
    *len = (size_t)(s - *tok);
    return s;
}

/* library id string (trailing line ends trimmed) + the stack's release
 * string, e.g. "bsdsocket.library 4.100 (17.8.2026) [lwip-amiga 1.0]" */
static void print_version(void)
{
    const char *id = (const char *)SocketBase->lib_IdString;
    if (id == NULL)
        return;

    char line[256];
    strncpy(line, id, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
        line[--len] = '\0';
    if (len == 0)
        return;

    printf("%s", line);

    STRPTR release = NULL;
    struct TagItem tags[2];
    tags[0].ti_Tag = SBTM_GETREF(SBTC_RELEASESTRPTR);
    tags[0].ti_Data = (ULONG)&release;
    tags[1].ti_Tag = TAG_END;
    if (SocketBaseTagList(tags) == 0 && release != NULL && release[0] != '\0')
        printf(strchr(line, '\n') != NULL ? "\n[%s]" : " [%s]", release);
    printf("\n");
}

int main(void)
{
    LONG args[ARG_COUNT];
    memset(args, 0, sizeof(args));
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR) "GetNetStatus");
        return RETURN_ERROR;
    }
    const char *check = (const char *)args[ARG_CHECK];
    BOOL quiet = args[ARG_QUIET] != 0;

    int rc = RETURN_FAIL;
    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        fprintf(stderr, "GetNetStatus: failed to open \"bsdsocket.library\" V4.\n");
        goto out_args;
    }

    ULONG status = 0;
    struct TagItem tags[2];
    tags[0].ti_Tag = SBTM_GETREF(SBTC_SYSTEM_STATUS);
    tags[0].ti_Data = (ULONG)&status;
    tags[1].ti_Tag = TAG_END;
    if (SocketBaseTagList(tags) != 0)
    {
        fprintf(stderr, "GetNetStatus: \"%s\" V%ld.%ld does not support the "
                        "status query method used by this program.\n",
                SocketBase->lib_Node.ln_Name,
                (long)SocketBase->lib_Version, (long)SocketBase->lib_Revision);
        goto out_lib;
    }

    BOOL selected[COND_COUNT];
    memset(selected, 0, sizeof(selected));
    BOOL badCondition = FALSE;
    if (check != NULL)
    {
        if (strcmp(check, "?") == 0)
        {
            /* the condition sub-template, Roadshow-style */
            printf("INTERFACES/S,PTPINTERFACES=PTP/S,"
                   "BCASTINTERFACES=BCAST=BROADCAST/S,"
                   "RESOLVER=NAMERESOLUTION=DNS/S,ROUTES/S,"
                   "DEFAULTROUTE=DEFAULTGATEWAY/S\n");
        }
        else
        {
            const char *tok;
            size_t toklen;
            for (const char *s = check;
                 (s = next_token(s, &tok, &toklen)) != NULL;)
            {
                BOOL known = FALSE;
                for (size_t i = 0; i < COND_COUNT && !known; i++)
                    for (size_t a = 0; a < 3 && conditions[i].names[a] != NULL; a++)
                        if (token_is(tok, toklen, conditions[i].names[a]))
                        {
                            selected[i] = TRUE;
                            known = TRUE;
                            break;
                        }
                if (!known)
                {
                    fprintf(stderr, "GetNetStatus: unknown condition \"%.*s\".\n",
                            (int)toklen, tok);
                    badCondition = TRUE;
                }
            }
        }
    }
    else if (!quiet)
    {
        print_version();
    }

    /* emphasize the negations on an interactive console */
    const char *emph_on = "", *emph_off = "";
    if (IsInteractive(Output()))
    {
        emph_on = "\33[4m";
        emph_off = "\33[24m";
    }

    rc = RETURN_OK;
    for (size_t i = 0; i < COND_COUNT; i++)
    {
        if (status & conditions[i].flag)
        {
            if (!quiet && check == NULL)
                printf("%s", conditions[i].ok);
        }
        else
        {
            if (!quiet && (check == NULL || selected[i]))
                printf(conditions[i].fail, emph_on, emph_off);
            if (selected[i])
                rc = RETURN_WARN;
        }
    }

    /* a misspelt condition was never checked, so a bare "IF WARN" must not
     * read as "all clear" — outrank the unsatisfied-condition warning */
    if (badCondition)
        rc = RETURN_ERROR;

out_lib:
    CloseLibrary(SocketBase);
out_args:
    FreeArgs(rda);
    if (quiet && rc > RETURN_WARN)
        rc = RETURN_WARN;
    return rc;
}
