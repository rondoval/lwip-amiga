/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed menu — see nsp_menu.h.
 */

#include "nsp_menu.h"

#include <string.h>

#include <classes/window.h>

#include <clib/alib_protos.h>
#include <proto/intuition.h>

/* Item ids (nm_UserData): the group in the high byte, the choice in the
 * low. Group 0 holds the plain actions. */
#define MID_ID(group, choice) (((group) << 8) | (choice))
#define MID_GROUP(id) ((id) >> 8)
#define MID_CHOICE(id) ((id) & 0xFFUL)
#define MID_IFACE 1UL              /* choice 0 = all interfaces, n = ifaceNames[n - 1] */
#define MID_SETTING(i) (2UL + (i)) /* choice = index into nspSettings[i].choices */

enum
{
    MID_ABOUT = 1,
    MID_RESET,
    MID_ICONIFY,
    MID_QUIT,
    MID_SAVE
};

/* --- building ------------------------------------------------------------- */

/* One NewMenu entry; the NM_END slot is always kept free. */
static void nsp_menu_add(struct NspMenu *m, UBYTE type, const char *label, const char *key,
                         UWORD flags, LONG mx, ULONG id)
{
    if (m->count >= NSP_MENU_MAX - 1)
        return;
    struct NewMenu *nm = &m->items[m->count++];
    nm->nm_Type = type;
    nm->nm_Label = (CONST_STRPTR)label;
    nm->nm_CommKey = (CONST_STRPTR)key;
    nm->nm_Flags = flags;
    nm->nm_MutualExclude = mx;
    nm->nm_UserData = (APTR)id;
}

/* A radio group is the whole run of checkmarked sub-items under one NM_ITEM:
 * open it with nsp_menu_group(), add the choices, close it with
 * nsp_menu_group_end(). */
static ULONG nsp_menu_group(struct NspMenu *m, const char *label)
{
    nsp_menu_add(m, NM_ITEM, label, NULL, 0, 0, 0);
    return m->count; /* the group's first sub-item lands here */
}

/* One checkmarked sub-item; nsp_menu_group_end() does the excluding. */
static void nsp_menu_choice(struct NspMenu *m, const char *label, BOOL checked, ULONG id)
{
    nsp_menu_add(m, NM_SUB, label, NULL, (UWORD)(CHECKIT | (checked ? CHECKED : 0)), 0, id);
}

/* nm_MutualExclude bit j means "clear sub-item j of this item", so each
 * choice excludes every sibling but itself. Filling the masks in once the
 * group is complete keeps them right however many choices there were, and
 * however many nsp_menu_add() had room for. */
static void nsp_menu_group_end(struct NspMenu *m, ULONG first)
{
    ULONG siblings = (1UL << (m->count - first)) - 1;
    for (ULONG i = first; i < m->count; i++)
        m->items[i].nm_MutualExclude = (LONG)(siblings & ~(1UL << (i - first)));
}

/* The whole array from the settings and the copied interface names. */
static void nsp_menu_build(struct NspMenu *m, const struct NspSettings *s)
{
    m->count = 0;
    nsp_menu_add(m, NM_TITLE, "Project", NULL, 0, 0, 0);
    nsp_menu_add(m, NM_ITEM, "About...", "?", 0, 0, MID_ABOUT);
    nsp_menu_add(m, NM_ITEM, "Reset statistics", "R", 0, 0, MID_RESET);
    nsp_menu_add(m, NM_ITEM, "Iconify", "I", 0, 0, MID_ICONIFY);
    nsp_menu_add(m, NM_ITEM, (const char *)NM_BARLABEL, NULL, 0, 0, 0);
    nsp_menu_add(m, NM_ITEM, "Quit", "Q", 0, 0, MID_QUIT);

    nsp_menu_add(m, NM_TITLE, "Settings", NULL, 0, 0, 0);
    ULONG ifaceGroup = nsp_menu_group(m, "Interface");
    nsp_menu_choice(m, "All interfaces", s->interface[0] == '\0', MID_ID(MID_IFACE, 0));
    for (ULONG i = 0; i < m->ifaceCount; i++)
        nsp_menu_choice(m, m->ifaceNames[i], strcmp(m->ifaceNames[i], s->interface) == 0,
                        MID_ID(MID_IFACE, i + 1));
    nsp_menu_group_end(m, ifaceGroup);
    for (ULONG i = 0; i < NSP_SET_COUNT; i++)
    {
        const struct NspSetting *set = &nspSettings[i];
        ULONG group = nsp_menu_group(m, set->label);
        for (ULONG k = 0; k < set->count; k++)
            nsp_menu_choice(m, set->choices[k].label, s->choice[i] == k, MID_ID(MID_SETTING(i), k));
        nsp_menu_group_end(m, group);
    }
    nsp_menu_add(m, NM_ITEM, (const char *)NM_BARLABEL, NULL, 0, 0, 0);
    nsp_menu_add(m, NM_ITEM, "Save", "S", 0, 0, MID_SAVE);

    struct NewMenu *end = &m->items[m->count];
    end->nm_Type = NM_END;
    end->nm_Label = NULL;
    end->nm_CommKey = NULL;
    end->nm_Flags = 0;
    end->nm_MutualExclude = 0;
    end->nm_UserData = NULL;
}

/* window.class recreates the strip now, or at the next open while iconified. */
void nsp_menu_sync(struct NspMenu *m, Object *winObj, const struct NspIfaceSet *ifaces,
                   const struct NspSettings *s)
{
    m->ifaceCount = ifaces->count < NSP_IFACE_MAX ? ifaces->count : NSP_IFACE_MAX;
    for (ULONG i = 0; i < m->ifaceCount; i++)
        strlcpy(m->ifaceNames[i], ifaces->names[i], NSP_NAME_MAX);
    nsp_menu_build(m, s);
    if (winObj != NULL)
        SetAttrs(winObj, WINDOW_NewMenu, (ULONG)m->items, TAG_END);
}

/* --- picking -------------------------------------------------------------- */

/* Project » About... */
static void nsp_menu_about(struct Window *win)
{
    nsp_request(win, "OK",
                NSP_NAME " " TOOL_VERSION " " TOOL_DATE "\n\n"
                "Network throughput per interface.\n"
                "Received above the line, sent below,\n"
                "both halves on one scale.");
}

/* Intuition has already moved the checkmarks; after a setting pick the array
 * is rebuilt to match. Quit ends the walk, everything else is latched so the
 * rest of a multi-selection still runs. */
ULONG nsp_menu_pick(struct NspMenu *m, struct Window *win, struct NspSettings *s, UWORD code)
{
    ULONG ev = 0;
    BOOL changed = FALSE;

    while (code != MENUNULL)
    {
        struct MenuItem *item = ItemAddress(win->MenuStrip, code);
        if (item == NULL)
            break;
        ULONG id = (ULONG)GTMENUITEM_USERDATA(item);
        ULONG group = MID_GROUP(id);
        ULONG choice = MID_CHOICE(id);
        if (group == MID_IFACE)
        {
            strlcpy(s->interface,
                    choice >= 1 && choice <= m->ifaceCount ? m->ifaceNames[choice - 1] : "",
                    NSP_NAME_MAX);
            ev |= NSP_EV_SOURCE;
            changed = TRUE;
        }
        else if (group >= MID_SETTING(0))
        {
            ULONG i = group - MID_SETTING(0);
            if (i < NSP_SET_COUNT && choice < nspSettings[i].count)
            {
                s->choice[i] = choice;
                changed = TRUE;
            }
        }
        else
        {
            switch (id)
            {
            case MID_ABOUT:
                nsp_menu_about(win);
                break;
            case MID_RESET:
                ev |= NSP_EV_RESET;
                break;
            case MID_ICONIFY:
                ev |= NSP_EV_ICONIFY;
                break;
            case MID_QUIT:
                return NSP_EV_QUIT;
            case MID_SAVE:
                ev |= NSP_EV_SAVE;
                break;
            default:
                break;
            }
        }
        code = item->NextSelect;
    }
    if (changed)
        nsp_menu_build(m, s);
    return ev;
}
