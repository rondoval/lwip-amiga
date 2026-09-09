/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetLogViewer window — see nlv_window.h.
 *
 */

#include "nlv_window.h"

#include <stdio.h>
#include <string.h>

#include <dos/datetime.h>
#include <exec/lists.h>
#include <gadgets/layout.h>
#include <gadgets/listbrowser.h>
#include <classes/window.h>
#include <intuition/gadgetclass.h>
#include <intuition/intuition.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <reaction/reaction_macros.h>

#include <clib/alib_protos.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/layout.h>
#include <proto/listbrowser.h>
#include <proto/window.h>

struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *ListBrowserBase;
struct Library *AslBase;

enum
{
    GID_LIST = 1
};

enum
{
    MID_CLEAR = 1,
    MID_SAVE,
    MID_HIDE,
    MID_QUIT
};

static struct NewMenu nlvMenu[] = {
    {NM_TITLE, (STRPTR) "Project", NULL, 0, 0, NULL},
    {NM_ITEM, (STRPTR) "Clear", (STRPTR) "C", 0, 0, (APTR)MID_CLEAR},
    {NM_ITEM, (STRPTR) "Save message list as...", (STRPTR) "A", 0, 0, (APTR)MID_SAVE},
    {NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL},
    {NM_ITEM, (STRPTR) "Hide", (STRPTR) "H", 0, 0, (APTR)MID_HIDE},
    {NM_ITEM, (STRPTR) "Quit", (STRPTR) "Q", 0, 0, (APTR)MID_QUIT},
    {NM_END, NULL, NULL, 0, 0, NULL},
};

enum
{
    COL_TIME,
    COL_ORIGIN,
    COL_SEVERITY,
    COL_MESSAGE,
    COL_COUNT
};

static struct ColumnInfo nlvColumns[] = {
    {10, (STRPTR) "Time", CIF_WEIGHTED},
    {18, (STRPTR) "Origin", CIF_WEIGHTED},
    {12, (STRPTR) "Severity", CIF_WEIGHTED},
    {60, (STRPTR) "Message", CIF_WEIGHTED},
    {-1, (STRPTR)~0UL, ~0UL},
};

static const char *const nlvSeverity[8] = {
    "Emergency", "Alert", "Critical", "Error", "Warning", "Note", "Information", "Debug",
};

static Object *winObj;
static struct Gadget *lbGad;
static struct Window *win; /* NULL while hidden */
static struct List lines;
static ULONG lineCount;

/* --- lifecycle ------------------------------------------------------------ */

BOOL nlv_window_init(void)
{
    NewList(&lines);

    WindowBase = OpenLibrary((CONST_STRPTR) "window.class", 47);
    LayoutBase = OpenLibrary((CONST_STRPTR) "gadgets/layout.gadget", 47);
    ListBrowserBase = OpenLibrary((CONST_STRPTR) "gadgets/listbrowser.gadget", 47);
    AslBase = OpenLibrary((CONST_STRPTR) "asl.library", 38);
    if (WindowBase == NULL || LayoutBase == NULL || ListBrowserBase == NULL || AslBase == NULL)
    {
        nlv_report("Could not open the ReAction classes (window.class, layout.gadget, "
                   "listbrowser.gadget V47) or asl.library.");
        return FALSE;
    }

    /* explicit NewObject calls: the reaction_macros "...End" closers hide
     * their ')' inside a macro, which cannot terminate this toolchain's
     * vararg NewObject macro */
    lbGad = (struct Gadget *)NewObject(LISTBROWSER_GetClass(), NULL,
        GA_ID, GID_LIST,
        GA_ReadOnly, TRUE,
        LISTBROWSER_Labels, (ULONG)&lines,
        LISTBROWSER_ColumnInfo, (ULONG)nlvColumns,
        LISTBROWSER_ColumnTitles, TRUE,
        LISTBROWSER_VertSeparators, TRUE,
        LISTBROWSER_HorizontalProp, TRUE,
        LISTBROWSER_FastRender, TRUE,
        TAG_END);
    Object *layout = NULL;
    if (lbGad != NULL)
        layout = NewObject(LAYOUT_GetClass(), NULL,
            LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
            LAYOUT_SpaceOuter, TRUE,
            LAYOUT_DeferLayout, TRUE,
            LAYOUT_AddChild, (ULONG)lbGad,
            TAG_END);
    if (layout != NULL)
        winObj = NewObject(WINDOW_GetClass(), NULL,
            WA_Title, (ULONG)NLV_NAME,
            WA_DragBar, TRUE,
            WA_DepthGadget, TRUE,
            WA_SizeGadget, TRUE,
            WA_CloseGadget, TRUE,
            WA_Activate, TRUE,
            WA_Width, 560,
            WA_Height, 200,
            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MENUPICK | IDCMP_NEWSIZE | IDCMP_REFRESHWINDOW,
            WINDOW_Position, WPOS_CENTERSCREEN,
            WINDOW_NewMenu, (ULONG)nlvMenu,
            WINDOW_ParentGroup, (ULONG)layout,
            TAG_END);
    if (winObj == NULL)
    {
        /* the window object owns the layout, which owns the gadget; whatever
         * was made before the failure is ours to dispose */
        if (layout != NULL)
            DisposeObject(layout);
        else if (lbGad != NULL)
            DisposeObject((Object *)lbGad);
        lbGad = NULL;
        nlv_report("Could not create the log window.");
        return FALSE;
    }
    return TRUE;
}

static void nlv_lines_free(void)
{
    struct Node *n;
    while ((n = RemHead(&lines)) != NULL)
        FreeListBrowserNode(n);
    lineCount = 0;
}

void nlv_window_exit(void)
{
    if (winObj != NULL)
    {
        nlv_window_hide();
        SetGadgetAttrs(lbGad, NULL, NULL, LISTBROWSER_Labels, ~0UL, TAG_DONE);
        DisposeObject(winObj);
        winObj = NULL;
        lbGad = NULL;
    }
    nlv_lines_free();
    if (AslBase != NULL)
        CloseLibrary(AslBase);
    if (ListBrowserBase != NULL)
        CloseLibrary(ListBrowserBase);
    if (LayoutBase != NULL)
        CloseLibrary(LayoutBase);
    if (WindowBase != NULL)
        CloseLibrary(WindowBase);
}

/* --- show / hide ---------------------------------------------------------- */

BOOL nlv_window_show(void)
{
    if (win != NULL)
    {
        WindowToFront(win);
        ActivateWindow(win);
        return TRUE;
    }
    win = RA_OpenWindow(winObj);
    if (win == NULL)
    {
        nlv_report("Could not open the log window.");
        return FALSE;
    }
    SetGadgetAttrs(lbGad, win, NULL, LISTBROWSER_Position, LBP_BOTTOM, TAG_DONE);
    return TRUE;
}

void nlv_window_hide(void)
{
    if (win == NULL)
        return;
    RA_CloseWindow(winObj);
    win = NULL;
}

ULONG nlv_window_sigmask(void)
{
    if (win == NULL)
        return 0;
    ULONG mask = 0;
    GetAttr(WINDOW_SigMask, winObj, &mask);
    return mask;
}

/* --- ingest --------------------------------------------------------------- */

void nlv_window_update_begin(void)
{
    SetGadgetAttrs(lbGad, win, NULL, LISTBROWSER_Labels, ~0UL, TAG_DONE);
}

void nlv_window_update_add(const struct NlvLogMsg *m)
{
    char time[12];
    ULONG mins = (ULONG)m->nlm_Date.ds_Minute;
    snprintf(time, sizeof(time), "%02lu:%02lu:%02lu", (unsigned long)(mins / 60 % 24),
             (unsigned long)(mins % 60),
             (unsigned long)((ULONG)m->nlm_Date.ds_Tick / TICKS_PER_SECOND % 60));

    /* one line per node: embedded line breaks would render as garbage */
    char text[NLV_TEXT_MAX];
    ULONG i;
    for (i = 0; i < sizeof(text) - 1 && m->nlm_Text[i] != '\0'; i++)
    {
        char c = m->nlm_Text[i];
        text[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    text[i] = '\0';

    struct Node *node = AllocListBrowserNode(COL_COUNT,
        LBNA_UserData, (ULONG)m->nlm_Date.ds_Days,
        LBNA_Column, COL_TIME, LBNCA_CopyText, TRUE, LBNCA_Text, (ULONG)time,
        LBNA_Column, COL_ORIGIN, LBNCA_CopyText, TRUE,
            LBNCA_Text, (ULONG)(m->nlm_Tag != NULL ? m->nlm_Tag : "Unknown"),
        LBNA_Column, COL_SEVERITY, LBNCA_CopyText, TRUE,
            LBNCA_Text, (ULONG)nlvSeverity[m->nlm_Pri & 7],
        LBNA_Column, COL_MESSAGE, LBNCA_CopyText, TRUE, LBNCA_Text, (ULONG)text,
        TAG_DONE);
    if (node == NULL)
        return;
    AddTail(&lines, node);
    lineCount++;
    while (lineCount > NLV_MAX_LINES)
    {
        FreeListBrowserNode(RemHead(&lines));
        lineCount--;
    }
}

void nlv_window_update_end(void)
{
    SetGadgetAttrs(lbGad, win, NULL, LISTBROWSER_Labels, (ULONG)&lines, TAG_DONE);
    if (win != NULL)
        SetGadgetAttrs(lbGad, win, NULL, LISTBROWSER_Position, LBP_BOTTOM, TAG_DONE);
}

/* --- menu actions --------------------------------------------------------- */

static void nlv_clear(void)
{
    if (lineCount == 0)
        return;
    if (nlv_request(win, "Yes|No", "Do you really want to clear the message list?") != 1)
        return;
    nlv_window_update_begin();
    nlv_lines_free();
    nlv_window_update_end();
}

static const char *nlv_node_text(struct Node *n, ULONG column)
{
    STRPTR text = NULL;
    GetListBrowserNodeAttrs(n, LBNA_Column, column, LBNCA_Text, (ULONG)&text, TAG_DONE);
    return text != NULL ? (const char *)text : "";
}

/* "DD-MMM-YY HH:MM:SS origin severity: message" per line */
static BOOL nlv_write_lines(BPTR fh)
{
    for (struct Node *n = lines.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        APTR days = NULL;
        GetListBrowserNodeAttrs(n, LBNA_UserData, (ULONG)&days, TAG_DONE);

        char date[LEN_DATSTRING];
        struct DateTime dt;
        memset(&dt, 0, sizeof(dt));
        dt.dat_Stamp.ds_Days = (LONG)days;
        dt.dat_Format = FORMAT_DOS;
        dt.dat_StrDate = (STRPTR)date;
        if (DateToStr(&dt) == 0)
            snprintf(date, sizeof(date), "?");

        char line[LEN_DATSTRING + 16 + NLV_TAG_MAX + 16 + NLV_TEXT_MAX];
        snprintf(line, sizeof(line), "%s %s %s %s: %s\n", date, nlv_node_text(n, COL_TIME),
                 nlv_node_text(n, COL_ORIGIN), nlv_node_text(n, COL_SEVERITY),
                 nlv_node_text(n, COL_MESSAGE));
        if (FPuts(fh, (CONST_STRPTR)line) != 0)
            return FALSE;
    }
    return TRUE;
}

static BOOL nlv_file_exists(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == 0)
        return FALSE;
    UnLock(lock);
    return TRUE;
}

static void nlv_save(void)
{
    struct FileRequester *fr = AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window, (ULONG)win,
        ASLFR_SleepWindow, TRUE,
        ASLFR_TitleText, (ULONG) "Save log messages",
        ASLFR_PositiveText, (ULONG) "Save",
        ASLFR_DoSaveMode, TRUE,
        ASLFR_InitialFile, (ULONG) "NetLog.txt",
        TAG_DONE);
    if (fr == NULL)
    {
        nlv_request(win, "OK", "Could not open the file requester.");
        return;
    }

    for (;;)
    {
        if (!AslRequest(fr, NULL))
            break; /* cancelled */

        char path[512];
        snprintf(path, sizeof(path), "%s", fr->fr_Drawer != NULL ? (const char *)fr->fr_Drawer : "");
        if (!AddPart((STRPTR)path, fr->fr_File, sizeof(path)))
        {
            nlv_request(win, "OK", "The file name is too long.");
            continue;
        }

        BOOL append = FALSE;
        if (nlv_file_exists(path))
        {
            LONG r = nlv_request(win, "Overwrite|Append|Choose new name...|Cancel",
                                 "The file\n%s\nalready exists.", path);
            if (r == 0)
                break;
            if (r == 3)
                continue;
            append = r == 2;
        }

        BPTR fh = Open((CONST_STRPTR)path, append ? MODE_OLDFILE : MODE_NEWFILE);
        BOOL ok = fh != 0;
        if (ok && append)
            ok = Seek(fh, 0, OFFSET_END) != -1;
        if (ok)
            ok = nlv_write_lines(fh);
        LONG err = ok ? 0 : IoErr();
        if (fh != 0 && !Close(fh) && ok)
        {
            ok = FALSE;
            err = IoErr();
        }
        if (!ok)
        {
            char fault[128];
            Fault(err, NULL, (STRPTR)fault, sizeof(fault));
            nlv_request(win, "OK", "Could not save the log messages to\n%s\n%s", path, fault);
        }
        break;
    }
    FreeAslRequest(fr);
}

static enum NlvAction nlv_menu_pick(UWORD code)
{
    enum NlvAction act = NLV_ACT_NONE;

    while (code != MENUNULL)
    {
        struct MenuItem *item = ItemAddress(win->MenuStrip, code);
        if (item == NULL)
            break;
        switch ((ULONG)GTMENUITEM_USERDATA(item))
        {
        case MID_CLEAR:
            nlv_clear();
            break;
        case MID_SAVE:
            nlv_save();
            break;
        case MID_HIDE:
            /* latched, not returned: the rest of a multi-selection still runs */
            act = NLV_ACT_HIDE;
            break;
        case MID_QUIT:
            return NLV_ACT_QUIT; /* nothing after it can matter */
        default:
            break;
        }
        code = item->NextSelect;
    }
    return act;
}

enum NlvAction nlv_window_handle_input(void)
{
    enum NlvAction act = NLV_ACT_NONE;
    WORD code = 0;
    ULONG result;

    while (win != NULL && (result = RA_HandleInput(winObj, &code)) != WMHI_LASTMSG)
    {
        enum NlvAction a = NLV_ACT_NONE;
        switch (result & WMHI_CLASSMASK)
        {
        case WMHI_CLOSEWINDOW:
            a = NLV_ACT_HIDE;
            break;
        case WMHI_MENUPICK:
            a = nlv_menu_pick((UWORD)(result & WMHI_MENUMASK));
            break;
        default:
            break;
        }
        if (a == NLV_ACT_QUIT)
            return a;
        if (a == NLV_ACT_HIDE)
            act = a;
    }
    return act;
}
