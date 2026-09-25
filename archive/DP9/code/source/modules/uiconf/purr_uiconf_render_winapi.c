// purr_uiconf_render_winapi.c — see purr_uiconf_render_winapi.h for the
// full picture, including the "stays unverified until a real device
// registers a UI backend" flag the approved plan itself carries for this
// file.
#include <string.h>
#include <stdio.h>
#include "purr_uiconf_render_winapi.h"
#include "purr_uiconf_sources.h"

bool purr_uiconf_render_winapi_init(purr_uiconf_render_winapi_t *r, const char *title)
{
    memset(r, 0, sizeof(*r));
    r->win = purr_win_create(title);
    return r->win != 0;
}

void purr_uiconf_render_winapi_deinit(purr_uiconf_render_winapi_t *r)
{
    if (r->win) purr_win_destroy(r->win);
    memset(r, 0, sizeof(*r));
}

void purr_uiconf_render_winapi_draw(purr_uiconf_render_winapi_t *r, const purr_uiconf_state_t *state,
                                     purr_win_cb_t cb, void *user)
{
    if (!r->win) return;
    const purr_uiconf_screen_t *s = &state->screen;
    if (!s->data) return;

    purr_win_clear(r->win);
    r->menu = 0;
    r->menu_node = PUI_NONE;
    r->row_count = 0;
    r->button_count = 0;

    for (int child = purr_uiconf_first_child(s, PUI_ROOT); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        pui_kind_t k = purr_uiconf_kind(s, child);
        // SECTION nesting deferred, same first-pass scope as the fb
        // backend's own header comment; ON_HANDLER nodes carry no widget.
        if (k == PUI_KIND_ON_HANDLER || k == PUI_KIND_SECTION) continue;
        if (!purr_uiconf_visible(s, child)) continue;

        if (k == PUI_KIND_WIDGET_TEXT) {
            char buf[64] = {0};
            char bind_name[24], format[32];
            bool have_bind = purr_uiconf_attr_str(s, child, "bind", bind_name, sizeof(bind_name));
            bool have_format = purr_uiconf_attr_str(s, child, "format", format, sizeof(format));
            if (have_bind) {
                purr_uiconf_resolve_bind(bind_name, have_format ? format : NULL, buf, sizeof(buf));
            } else {
                purr_uiconf_attr_str(s, child, "text", buf, sizeof(buf));
            }
            purr_wid_t lbl = purr_win_label(r->win, buf);
            char align[16];
            if (purr_uiconf_attr_str(s, child, "align", align, sizeof(align)) && strcmp(align, "center") == 0)
                purr_win_label_align(lbl, PURR_ALIGN_CENTER);

        } else if (k == PUI_KIND_WIDGET_BUTTON) {
            char label[40] = {0};
            purr_uiconf_attr_str(s, child, "label", label, sizeof(label));
            purr_wid_t wid = purr_win_button(r->win, label, cb, user);
            if (wid && r->button_count < PUI_WINAPI_MAX_BUTTONS) {
                r->button_wids[r->button_count] = wid;
                r->button_nodes[r->button_count] = child;
                r->button_count++;
            }

        } else if (k == PUI_KIND_WIDGET_LIST && r->menu == 0) {
            // Only the FIRST top-level list widget becomes this window's
            // one native menu — same "first list widget" rule
            // purr_uiconf_state_t's own selected_index already applies
            // for the fb backend.
            int count = purr_uiconf_list_item_count(s, child);
            if (count > PUI_WINAPI_MAX_ROWS) count = PUI_WINAPI_MAX_ROWS;
            if (count <= 0) {
                char empty[40];
                if (!purr_uiconf_attr_str(s, child, "empty_text", empty, sizeof(empty)))
                    snprintf(empty, sizeof(empty), "Empty");
                purr_win_label(r->win, empty);
                continue;
            }
            for (int i = 0; i < count; i++) {
                if (!purr_uiconf_list_item_label(s, child, i, r->row_bufs[i], PUI_WINAPI_ROW_LEN))
                    r->row_bufs[i][0] = 0;
                r->row_ptrs[i] = r->row_bufs[i];
            }
            r->row_count = count;
            purr_menu_section_t sec = { .header = NULL, .items = r->row_ptrs, .values = NULL, .count = count };
            r->menu = purr_win_menu(r->win);
            r->menu_node = child;
            purr_win_menu_set_sections(r->menu, &sec, 1);
            purr_win_menu_on_select(r->menu, cb, user);
            int idx = state->selected_index;
            if (idx < 0) idx = 0;
            if (idx >= count) idx = count - 1;
            purr_win_list_set_selected(r->menu, idx);
        }
    }
    purr_win_show(r->win);
}

int purr_uiconf_render_winapi_node_for_wid(const purr_uiconf_render_winapi_t *r, purr_wid_t wid)
{
    if (wid == 0) return PUI_NONE;
    if (r->menu && wid == r->menu) return r->menu_node;
    for (int i = 0; i < r->button_count; i++) {
        if (r->button_wids[i] == wid) return r->button_nodes[i];
    }
    return PUI_NONE;
}

int purr_uiconf_render_winapi_move_selection(purr_uiconf_render_winapi_t *r, purr_uiconf_state_t *state, int delta)
{
    const purr_uiconf_screen_t *s = &state->screen;
    int list_node = purr_uiconf_find_first_list_widget(s);
    if (list_node == PUI_NONE) return state->selected_index;
    int count = purr_uiconf_list_item_count(s, list_node);
    if (count <= 0) return state->selected_index;

    int old = state->selected_index;
    int new_idx = ((old + delta) % count + count) % count;
    state->selected_index = new_idx;
    // No manual redraw needed — the native widget manager owns rendering
    // its own selection highlight; this backend's job stops at telling it
    // which row that is.
    if (r->menu) purr_win_list_set_selected(r->menu, new_idx);
    return new_idx;
}

bool purr_uiconf_render_winapi_selected_target(const purr_uiconf_render_winapi_t *r, const purr_uiconf_state_t *state,
                                                char *out, size_t out_sz)
{
    (void)r;
    const purr_uiconf_screen_t *s = &state->screen;
    int list_node = purr_uiconf_find_first_list_widget(s);
    if (list_node == PUI_NONE) return false;
    return purr_uiconf_list_item_target(s, list_node, state->selected_index, out, out_sz);
}
