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

void nsp_prefs_defaults(struct NspSettings *s)
{
    s->interface[0] = '\0';
    s->interval = NSP_INTERVAL_MIN;
    s->bits = FALSE;
    s->linkScale = FALSE;
    s->haveBox = FALSE;
    s->box.Left = 0;
    s->box.Top = 0;
    s->box.Width = 0;
    s->box.Height = 0;
}

LONG nsp_prefs_clamp_interval(LONG v)
{
    if (v < NSP_INTERVAL_MIN)
        return NSP_INTERVAL_MIN;
    if (v > NSP_INTERVAL_MAX)
        return NSP_INTERVAL_MAX;
    return v;
}

/* --- load ----------------------------------------------------------------- */

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
        if (strcasecmp(key, "INTERFACE") == 0)
            strlcpy(s->interface, val, NSP_NAME_MAX);
        else if (strcasecmp(key, "INTERVAL") == 0)
        {
            LONG v;
            if (StrToLong((CONST_STRPTR)val, &v) > 0)
                s->interval = nsp_prefs_clamp_interval(v);
        }
        else if (strcasecmp(key, "UNITS") == 0)
            s->bits = strcasecmp(val, "BITS") == 0;
        else if (strcasecmp(key, "SCALE") == 0)
            s->linkScale = strcasecmp(val, "LINK") == 0;
        else if (strcasecmp(key, "WINDOW") == 0)
            s->haveBox = nsp_prefs_parse_box(val, &s->box);
        /* anything else: ignored, like netstack.prefs does */
    }
    Close(fh);
}

/* --- save ----------------------------------------------------------------- */

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

BOOL nsp_prefs_save(const struct NspSettings *s, const char **failedPath, LONG *ioErr)
{
    char text[256];
    size_t len = 0;
    len += (size_t)snprintf(text + len, sizeof(text) - len,
                            "# " NSP_NAME " preferences - written by Settings > Save\n");
    if (s->interface[0] != '\0')
        len += (size_t)snprintf(text + len, sizeof(text) - len, "INTERFACE = %s\n",
                                s->interface);
    len += (size_t)snprintf(text + len, sizeof(text) - len,
                            "INTERVAL = %ld\nUNITS = %s\nSCALE = %s\n", (long)s->interval,
                            s->bits ? "BITS" : "BYTES", s->linkScale ? "LINK" : "AUTO");
    if (s->haveBox)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "WINDOW = %ld %ld %ld %ld\n",
                                (long)s->box.Left, (long)s->box.Top, (long)s->box.Width,
                                (long)s->box.Height);
    if (len >= sizeof(text))
        len = sizeof(text) - 1;

    /* ENV: first: it is what the next start reads. If ENVARC: is a link to
     * ENV: the second write just rewrites the same bytes. */
    static const char *const paths[2] = {NSP_PREFS_ENV, NSP_PREFS_ENVARC};
    BOOL ok = TRUE;
    for (ULONG i = 0; i < 2; i++)
    {
        LONG err = 0;
        if (!nsp_prefs_write(paths[i], text, (LONG)len, &err) && ok)
        {
            ok = FALSE;
            *failedPath = paths[i];
            *ioErr = err;
        }
    }
    return ok;
}
