/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed settings — see nsp_prefs.h.
 */

#include "nsp_prefs.h"

#include <stdio.h>
#include <string.h>

#include <dos/dos.h>

#include <proto/dos.h>

#include <prefs.h> /* emu68-common prefs_split() */

#define NSP_PREFS_ENV "ENV:" NSP_NAME ".prefs"
#define NSP_PREFS_ENVARC "ENVARC:" NSP_NAME ".prefs"

/* --- the settings table --------------------------------------------------- */

static const struct NspChoice nspGraphChoices[] = {
    {"1 second per pixel", "1", 1},
    {"5 seconds per pixel", "5", 5},
    {"30 seconds per pixel", "30", 30},
};

static const struct NspChoice nspAverageChoices[] = {
    {"10 seconds", "10", 10},
    {"1 minute", "60", 60},
    {"5 minutes", "300", 300},
    {"Since reset", "0", 0},
};

static const struct NspChoice nspUnitsChoices[] = {
    {"bytes/s", "BYTES", 0},
    {"bits/s", "BITS", 1},
};

static const struct NspChoice nspScaleChoices[] = {
    {"Automatic", "AUTO", 0},
    {"Link speed", "LINK", 1},
};

#define NSP_CHOICES(t) t, sizeof(t) / sizeof(t[0])

const struct NspSetting nspSettings[NSP_SET_COUNT] = {
    [NSP_SET_GRAPH] = {"Graph", "GRAPH", NSP_CHOICES(nspGraphChoices), 0},
    [NSP_SET_AVERAGE] = {"Average", "AVERAGE", NSP_CHOICES(nspAverageChoices), 1},
    [NSP_SET_UNITS] = {"Units", "UNITS", NSP_CHOICES(nspUnitsChoices), 0},
    [NSP_SET_SCALE] = {"Scale", "SCALE", NSP_CHOICES(nspScaleChoices), 0},
};

/* All interfaces, every setting at its table default, no saved box. */
void nsp_prefs_defaults(struct NspSettings *s)
{
    s->interface[0] = '\0';
    for (ULONG i = 0; i < NSP_SET_COUNT; i++)
        s->choice[i] = nspSettings[i].def;
    s->haveBox = FALSE;
    s->box.Left = 0;
    s->box.Top = 0;
    s->box.Width = 0;
    s->box.Height = 0;
}

/* --- load ----------------------------------------------------------------- */

/* The table setting a prefs key belongs to; -1 for the other keys. */
static LONG nsp_prefs_setting(const char *key)
{
    for (ULONG i = 0; i < NSP_SET_COUNT; i++)
        if (strcasecmp(key, nspSettings[i].key) == 0)
            return (LONG)i;
    return -1;
}

/* A setting's value word: one of its tokens selects that choice, anything
 * else leaves the current (default) choice. */
static void nsp_prefs_parse_choice(struct NspSettings *s, ULONG id, const char *val)
{
    const struct NspSetting *set = &nspSettings[id];
    for (ULONG i = 0; i < set->count; i++)
        if (strcasecmp(val, set->choices[i].token) == 0)
        {
            s->choice[id] = i;
            return;
        }
}

/* "left top width height"; all four must parse and the size must be positive */
static BOOL nsp_prefs_parse_box(const char *v, struct IBox *box)
{
    LONG n[4];
    for (ULONG i = 0; i < 4; i++)
    {
        while (*v == ' ' || *v == '\t')
            v++;
        LONG used = StrToLong((CONST_STRPTR)v, &n[i]);
        if (used <= 0)
            return FALSE;
        v += used;
    }
    if (n[2] <= 0 || n[3] <= 0)
        return FALSE;
    box->Left = (WORD)n[0];
    box->Top = (WORD)n[1];
    box->Width = (WORD)n[2];
    box->Height = (WORD)n[3];
    return TRUE;
}

/* Defaults, then one "KEY = VALUE" line at a time; unknown keys are ignored
 * like netstack.prefs does. */
void nsp_prefs_load(struct NspSettings *s)
{
    nsp_prefs_defaults(s);

    BPTR fh = Open((CONST_STRPTR)NSP_PREFS_ENV, MODE_OLDFILE);
    if (fh == 0)
        return;

    char line[128];
    while (FGets(fh, (STRPTR)line, sizeof(line)) != NULL)
    {
        char *key, *val;
        if (!prefs_split(line, &key, &val))
            continue;
        LONG id = nsp_prefs_setting(key);
        if (id >= 0)
            nsp_prefs_parse_choice(s, (ULONG)id, val);
        else if (strcasecmp(key, "INTERFACE") == 0)
            strlcpy(s->interface, val, NSP_NAME_MAX);
        else if (strcasecmp(key, "WINDOW") == 0)
            s->haveBox = nsp_prefs_parse_box(val, &s->box);
    }
    Close(fh);
}

/* --- save ----------------------------------------------------------------- */

/* The file text for these settings; returns its length. A full buffer
 * truncates (it never is: the longest file is well under 256 bytes). */
static size_t nsp_prefs_format(const struct NspSettings *s, char *text, size_t size)
{
    size_t len =
        (size_t)snprintf(text, size, "# " NSP_NAME " preferences - written by Settings > Save\n");
    if (s->interface[0] != '\0' && len < size)
        len += (size_t)snprintf(text + len, size - len, "INTERFACE = %s\n", s->interface);
    for (ULONG i = 0; i < NSP_SET_COUNT && len < size; i++)
        len += (size_t)snprintf(text + len, size - len, "%s = %s\n", nspSettings[i].key,
                                nspSettings[i].choices[s->choice[i]].token);
    if (s->haveBox && len < size)
        len += (size_t)snprintf(text + len, size - len, "WINDOW = %ld %ld %ld %ld\n",
                                (long)s->box.Left, (long)s->box.Top, (long)s->box.Width,
                                (long)s->box.Height);
    return len < size ? len : size - 1;
}

/* One file, fully written and closed; FALSE with the IoErr() of whichever
 * step failed. */
static BOOL nsp_prefs_write(const char *path, const char *text, LONG len, LONG *ioErr)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (fh == 0)
    {
        *ioErr = IoErr();
        return FALSE;
    }
    BOOL ok = Write(fh, (APTR)text, len) == len;
    if (!ok)
        *ioErr = IoErr();
    if (!Close(fh) && ok)
    {
        ok = FALSE;
        *ioErr = IoErr();
    }
    return ok;
}

/* ENV: first: it is what the next start reads. If ENVARC: is a link to
 * ENV: the second write just rewrites the same bytes. */
BOOL nsp_prefs_save(const struct NspSettings *s, const char **failedPath, LONG *ioErr)
{
    char text[256];
    LONG len = (LONG)nsp_prefs_format(s, text, sizeof(text));

    static const char *const paths[2] = {NSP_PREFS_ENV, NSP_PREFS_ENVARC};
    BOOL ok = TRUE;
    for (ULONG i = 0; i < 2; i++)
    {
        LONG err = 0;
        if (!nsp_prefs_write(paths[i], text, len, &err) && ok)
        {
            ok = FALSE;
            *failedPath = paths[i];
            *ioErr = err;
        }
    }
    return ok;
}
