/* NetHack's window port for Viper: what NetHack asks to show or to ask, it
 * hands to the app's game model (nh_port.h, nethack_engine.cpp), which the
 * views draw. Nothing here draws.
 *
 * NetHack's menu identifiers (`anything`, often pointers) stay on this side:
 * the model sees each item's index, and select_menu maps the chosen indexes
 * back.
 */

#include "hack.h" /* first: NetHack's other headers need it */

#include "dlb.h"
#include "func_tab.h"

#include "nh_port.h"

#define MAX_WINDOWS 64

struct vwin {
    int used;
    int kind;
    anything *ids; /* menu items' identifiers, by index - 1 */
    int nids, capacity;
    int status_row; /* the status window's cursor row */
};

static struct vwin windows[MAX_WINDOWS];
static char status_lines[3][BUFSZ];

static struct vwin *win_of(winid w) {
    return (w >= 0 && w < MAX_WINDOWS && windows[w].used) ? &windows[w] : NULL;
}

/* ---- setup ---- */

static void viper_init_nhwindows(int *argcp UNUSED, char **argv UNUSED) {
    memset(windows, 0, sizeof windows);
    memset(status_lines, 0, sizeof status_lines);
    iflags.window_inited = TRUE;
    iflags.use_color = TRUE;
}

static void viper_player_selection(void) {
    /* NetHack's own menus (role.c), through select_menu */
    genl_player_setup(0);
}

static void viper_askname(void) {
    /* The app names the hero (nhv_nethack_main()); this asks only if that
       name was taken from it, which can't be typed yet */
    Strcpy(svp.plname, "Viper");
}

static void viper_get_nh_event(void) {}

static void viper_exit_nhwindows(const char *str) {
    if (str && *str)
        nhv_message(str);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        free(windows[i].ids);
        windows[i].ids = NULL;
        windows[i].used = 0;
    }
    iflags.window_inited = FALSE;
}

static void viper_suspend_nhwindows(const char *str UNUSED) {}

static void viper_resume_nhwindows(void) {}

/* ---- windows ---- */

static winid viper_create_nhwindow(int type) {
    int kind;

    switch (type) {
    case NHW_MESSAGE:
        kind = NHV_WIN_MESSAGE;
        break;
    case NHW_STATUS:
        kind = NHV_WIN_STATUS;
        break;
    case NHW_MAP:
        kind = NHV_WIN_MAP;
        break;
    case NHW_MENU:
        kind = NHV_WIN_MENU;
        break;
    default:
        kind = NHV_WIN_TEXT;
        break;
    }
    int w = nhv_window_create(kind);
    if (w <= 0 || w >= MAX_WINDOWS)
        panic("viper_create_nhwindow: too many windows");
    memset(&windows[w], 0, sizeof windows[w]);
    windows[w].used = 1;
    windows[w].kind = kind;
    return w;
}

static void viper_clear_nhwindow(winid w) {
    struct vwin *vw = win_of(w);

    if (!vw)
        return;
    if (vw->kind == NHV_WIN_MAP)
        nhv_map_clear();
    else
        nhv_window_clear(w);
    vw->nids = 0;
}

static void viper_display_nhwindow(winid w, boolean blocking) {
    struct vwin *vw = win_of(w);

    if (!vw)
        return;
    switch (vw->kind) {
    case NHV_WIN_MESSAGE:
        if (blocking)
            nhv_more();
        break;
    case NHV_WIN_MENU:
    case NHV_WIN_TEXT:
        nhv_window_show(w);
        break;
    default:
        break;
    }
}

static void viper_destroy_nhwindow(winid w) {
    struct vwin *vw = win_of(w);

    if (!vw)
        return;
    nhv_window_destroy(w);
    free(vw->ids);
    memset(vw, 0, sizeof *vw);
}

static void viper_curs(winid w, int x UNUSED, int y) {
    struct vwin *vw = win_of(w);

    if (vw && vw->kind == NHV_WIN_STATUS)
        vw->status_row = y;
}

/* Status text with the gold glyph encoded (\GXXXXNNNN) shows '$' */
static void decode_glyphs(const char *in, char *out, size_t size) {
    size_t n = 0;

    while (*in && n + 1 < size) {
        if (in[0] == '\\' && in[1] == 'G' && strlen(in) >= 10) {
            out[n++] = '$';
            in += 10;
        } else {
            out[n++] = *in++;
        }
    }
    out[n] = '\0';
}

static void viper_putstr(winid w, int attr, const char *str) {
    struct vwin *vw = win_of(w);

    if (!vw || !str)
        return;
    switch (vw->kind) {
    case NHV_WIN_MESSAGE:
        nhv_message(str);
        break;
    case NHV_WIN_STATUS: {
        int row = vw->status_row;
        const char *lines[3];

        if (row < 0 || row > 2)
            row = 2;
        decode_glyphs(str, status_lines[row], sizeof status_lines[row]);
        for (int i = 0; i < 3; i++)
            lines[i] = status_lines[i];
        nhv_status(lines, status_lines[2][0] ? 3 : 2);
        break;
    }
    case NHV_WIN_MAP:
        break;
    default:
        nhv_window_text(w, attr, str);
        break;
    }
}

static void viper_display_file(const char *fname, boolean complain) {
    dlb *f = dlb_fopen(fname, "r");
    char line[BUFSZ];

    if (!f) {
        if (complain)
            pline("Can't open %s.", fname);
        return;
    }
    winid w = create_nhwindow(NHW_TEXT);
    while (dlb_fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');

        if (nl)
            *nl = '\0';
        putstr(w, 0, line);
    }
    (void)dlb_fclose(f);
    display_nhwindow(w, TRUE);
    destroy_nhwindow(w);
}

/* ---- menus ---- */

static void viper_start_menu(winid w, unsigned long mbehavior UNUSED) {
    struct vwin *vw = win_of(w);

    if (!vw)
        return;
    vw->kind = NHV_WIN_MENU;
    vw->nids = 0;
    nhv_menu_start(w);
}

static void viper_add_menu(winid w, const glyph_info *glyphinfo UNUSED, const ANY_P *identifier,
                           char ch, char gch UNUSED, int attr, int clr, const char *str,
                           unsigned int itemflags) {
    struct vwin *vw = win_of(w);
    int index = 0;

    if (!vw)
        return;
    if (identifier && identifier->a_void) {
        if (vw->nids == vw->capacity) {
            int capacity = vw->capacity ? vw->capacity * 2 : 32;
            anything *ids = (anything *)realloc(vw->ids, capacity * sizeof *ids);

            if (!ids)
                return;
            vw->ids = ids;
            vw->capacity = capacity;
        }
        vw->ids[vw->nids++] = *identifier;
        index = vw->nids;
    }
    nhv_menu_add(w, index, ch, attr, clr, (itemflags & MENU_ITEMFLAGS_SELECTED) != 0,
                 str ? str : "");
}

static void viper_end_menu(winid w, const char *prompt) {
    if (win_of(w))
        nhv_menu_end(w, prompt);
}

static int viper_select_menu(winid w, int how, menu_item **menu_list) {
    struct vwin *vw = win_of(w);
    int pick = how == PICK_ONE ? NHV_PICK_ONE : how == PICK_ANY ? NHV_PICK_ANY : NHV_PICK_NONE;
    int *indexes;
    long *counts;
    int n;

    *menu_list = NULL;
    if (!vw)
        return -1;
    if (vw->nids == 0 || pick == NHV_PICK_NONE) {
        /* nothing to choose: show it, like a text window */
        nhv_window_show(w);
        return pick == NHV_PICK_NONE ? 0 : -1;
    }
    indexes = (int *)alloc(vw->nids * sizeof *indexes);
    counts = (long *)alloc(vw->nids * sizeof *counts);
    n = nhv_menu_select(w, pick, indexes, counts, vw->nids);
    if (n > 0) {
        *menu_list = (menu_item *)alloc(n * sizeof **menu_list);
        for (int i = 0; i < n; i++) {
            (*menu_list)[i].item = vw->ids[indexes[i] - 1];
            (*menu_list)[i].count = counts[i];
            (*menu_list)[i].itemflags = 0;
        }
    }
    free(indexes);
    free(counts);
    return n;
}

static char viper_message_menu(char let, int how UNUSED, const char *mesg) {
    pline("%s", mesg);
    return let;
}

static void viper_mark_synch(void) {}

static void viper_wait_synch(void) {}

static void viper_cliparound(int x, int y) { nhv_map_hero(x, y); }

/* ---- the map ---- */

static void viper_print_glyph(winid w UNUSED, coordxy x, coordxy y, const glyph_info *glyphinfo,
                              const glyph_info *bkglyphinfo UNUSED) {
    unsigned gf = glyphinfo->gm.glyphflags, flags = 0;

    if (gf & MG_PET)
        flags |= NHV_CELL_PET;
    if (gf & MG_DETECT)
        flags |= NHV_CELL_DETECTED;
    if (gf & MG_OBJPILE)
        flags |= NHV_CELL_OBJPILE;
    if (gf & MG_RIDDEN)
        flags |= NHV_CELL_RIDDEN;
    if (gf & (MG_INVIS | MG_STATUE))
        flags |= NHV_CELL_INVERSE;
    if (!cansee(x, y))
        flags |= NHV_CELL_REMEMBERED;
    nhv_map_glyph(x, y, glyphinfo->glyph, glyphinfo->ttychar, glyphinfo->gm.sym.color, flags);
}

static void viper_raw_print(const char *str) {
    if (str && *str)
        nhv_log(str);
}

/* ---- input ---- */

static int viper_nhgetch(void) { return nhv_get_key(); }

static int viper_nh_poskey(coordxy *x, coordxy *y, int *mod) {
    *x = *y = 0;
    *mod = 0;
    return nhv_get_key();
}

static void viper_nhbell(void) {}

static int viper_doprev_message(void) { return 0; }

static char viper_yn_function(const char *query, const char *resp, char def) {
    return nhv_yn(query, resp ? resp : "", def);
}

static void viper_getlin(const char *query, char *bufp) { nhv_getlin(query, bufp, BUFSZ); }

/* The extended commands, as a menu: the player's own, not wizard mode's or
   those this build can't do */
static int viper_get_ext_cmd(void) {
    winid w = create_nhwindow(NHW_MENU);
    anything any;
    menu_item *picked = NULL;
    int n, result = -1;
    char text[BUFSZ];

    start_menu(w, MENU_BEHAVE_STANDARD);
    for (int i = 0; extcmdlist[i].ef_txt; i++) {
        const struct ext_func_tab *ec = &extcmdlist[i];

        if ((ec->flags & (CMD_NOT_AVAILABLE | INTERNALCMD)) != 0 ||
            ((ec->flags & WIZMODECMD) && !wizard) || !(ec->flags & AUTOCOMPLETE))
            continue;
        any = cg.zeroany;
        any.a_int = i + 1;
        Snprintf(text, sizeof text, "%-14s %s", ec->ef_txt, ec->ef_desc);
        add_menu(w, &nul_glyphinfo, &any, 0, 0, ATR_NONE, NO_COLOR, text, MENU_ITEMFLAGS_NONE);
    }
    end_menu(w, "Which command?");
    n = select_menu(w, PICK_ONE, &picked);
    if (n > 0)
        result = picked[0].item.a_int - 1;
    free(picked);
    destroy_nhwindow(w);
    return result;
}

static void viper_number_pad(int state UNUSED) {}

static void viper_delay_output(void) {}

static void viper_preference_update(const char *pref UNUSED) {}

static char *viper_getmsghistory(boolean init UNUSED) { return NULL; }

static void viper_putmsghistory(const char *msg UNUSED, boolean restoring UNUSED) {}

static void viper_update_inventory(int arg UNUSED) {}

static win_request_info *viper_ctrl_nhwindow(winid w UNUSED, int request UNUSED,
                                             win_request_info *wri UNUSED) {
    return NULL;
}

struct window_procs viper_procs = {
    WPID(viper),
    (0 | WC_COLOR | WC_HILITE_PET | WC_INVERSE),
    (0 | WC2_DARKGRAY | WC2_FLUSH_STATUS),
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, /* color availability */
    viper_init_nhwindows,
    viper_player_selection,
    viper_askname,
    viper_get_nh_event,
    viper_exit_nhwindows,
    viper_suspend_nhwindows,
    viper_resume_nhwindows,
    viper_create_nhwindow,
    viper_clear_nhwindow,
    viper_display_nhwindow,
    viper_destroy_nhwindow,
    viper_curs,
    viper_putstr,
    genl_putmixed,
    viper_display_file,
    viper_start_menu,
    viper_add_menu,
    viper_end_menu,
    viper_select_menu,
    viper_message_menu,
    viper_mark_synch,
    viper_wait_synch,
#ifdef CLIPPING
    viper_cliparound,
#endif
#ifdef POSITIONBAR
    NULL,
#endif
    viper_print_glyph,
    viper_raw_print,
    viper_raw_print,
    viper_nhgetch,
    viper_nh_poskey,
    viper_nhbell,
    viper_doprev_message,
    viper_yn_function,
    viper_getlin,
    viper_get_ext_cmd,
    viper_number_pad,
    viper_delay_output,
#ifdef CHANGE_COLOR
    NULL,
#ifdef MAC
    NULL,
    NULL,
#endif
    NULL,
#endif
    genl_outrip,
    viper_preference_update,
    viper_getmsghistory,
    viper_putmsghistory,
    genl_status_init,
    genl_status_finish,
    genl_status_enablefield,
    genl_status_update,
    genl_can_suspend_no,
    viper_update_inventory,
    viper_ctrl_nhwindow,
};
