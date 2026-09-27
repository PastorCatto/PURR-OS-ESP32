// purr_uiconf_sources.c — see purr_uiconf_sources.h for the full picture.
#include <string.h>
#include <stdio.h>
#include "purr_uiconf_sources.h"
#include "../app_manager/app_manager.h"
#include "../user_mgr/user_mgr.h"
#include "../pairing/pairing.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"

// Index-identical to catstrap/uiconf.py's own SOURCES table.
static const char *s_sources[] = { "app_list", "user_list", "paired_devices", "mesh_backend_options", "module_list" };
#define SOURCE_COUNT (int)(sizeof(s_sources) / sizeof(s_sources[0]))

int purr_uiconf_source_id(const char *name)
{
    for (int i = 0; i < SOURCE_COUNT; i++)
        if (strcmp(s_sources[i], name) == 0) return i;
    return -1;
}

int purr_uiconf_source_count(int sid)
{
    switch (sid) {
        case 0: return app_manager_count();
        case 1: return user_mgr_count();
        case 2: return pairing_device_count();
        // mesh_backend_options: no mesh-backend enumeration API exists
        // anywhere in this tree yet (confirmed) — wire this up once one
        // does, per this whole effort's own "gradually re-introducing
        // features" direction. Reports honestly empty rather than a fake
        // static list.
        case 3: return 0;
        case 4: return purr_kernel_module_count();
        default: return 0;
    }
}

bool purr_uiconf_source_label(int sid, int idx, char *out, size_t sz)
{
    switch (sid) {
        case 0: return app_manager_entry_name(idx, out, sz);
        case 1: return user_mgr_at(idx, out, sz);
        case 2: {
            paired_device_t pd;
            if (!pairing_device_at(idx, &pd)) return false;
            snprintf(out, sz, "%s", pd.name);
            return true;
        }
        case 4: {
            const purr_module_header_t *m = purr_kernel_module_at(idx);
            if (!m) return false;
            snprintf(out, sz, "%s", m->name);
            return true;
        }
        default: return false;
    }
}

// See this header's own comment: each source's natural key, as a plain
// string.
//   app_list        -> the app's index, decimal — exactly what
//                       app_manager_launch_idx(int) takes.
//   user_list       -> the username itself (same string as the label).
//   paired_devices  -> "XX:XX:XX:XX:XX:XX" — the caller parses this back
//                       into 6 bytes before calling pairing_forget()/
//                       _is_trusted() etc., which take uint8_t mac[6].
bool purr_uiconf_source_target(int sid, int idx, char *out, size_t sz)
{
    switch (sid) {
        case 0: snprintf(out, sz, "%d", idx); return true;
        case 1: return user_mgr_at(idx, out, sz);
        case 2: {
            paired_device_t pd;
            if (!pairing_device_at(idx, &pd)) return false;
            snprintf(out, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
                     pd.mac[0], pd.mac[1], pd.mac[2], pd.mac[3], pd.mac[4], pd.mac[5]);
            return true;
        }
        // module_list: no "$item" target defined — nothing in this
        // effort's first-pass apps needs to activate a loaded-module row
        // (diagnostics' own module list is purely read-only). Add one
        // (module name? index into purr_kernel_module_at()?) once a real
        // caller needs it.
        default: return false;
    }
}

int purr_uiconf_find_first_list_widget(const purr_uiconf_screen_t *s)
{
    for (int child = purr_uiconf_first_child(s, PUI_ROOT); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        if (purr_uiconf_kind(s, child) == PUI_KIND_WIDGET_LIST) return child;
    }
    return PUI_NONE;
}

int purr_uiconf_list_item_count(const purr_uiconf_screen_t *s, int node)
{
    char src[24];
    if (purr_uiconf_attr_str(s, node, "source", src, sizeof(src))) {
        int sid = purr_uiconf_source_id(src);
        return sid < 0 ? 0 : purr_uiconf_source_count(sid);
    }
    int count = 0;
    for (int child = purr_uiconf_first_child(s, node); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        if (purr_uiconf_kind(s, child) == PUI_KIND_ITEM) count++;
    }
    return count;
}

bool purr_uiconf_list_item_label(const purr_uiconf_screen_t *s, int node, int idx, char *out, size_t sz)
{
    char src[24];
    if (purr_uiconf_attr_str(s, node, "source", src, sizeof(src))) {
        int sid = purr_uiconf_source_id(src);
        return sid < 0 ? false : purr_uiconf_source_label(sid, idx, out, sz);
    }
    int i = 0;
    for (int child = purr_uiconf_first_child(s, node); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        if (purr_uiconf_kind(s, child) != PUI_KIND_ITEM) continue;
        if (i == idx) return purr_uiconf_attr_str(s, child, "label", out, sz);
        i++;
    }
    return false;
}

bool purr_uiconf_list_item_target(const purr_uiconf_screen_t *s, int node, int idx, char *out, size_t sz)
{
    char src[24];
    if (purr_uiconf_attr_str(s, node, "source", src, sizeof(src))) {
        int sid = purr_uiconf_source_id(src);
        return sid < 0 ? false : purr_uiconf_source_target(sid, idx, out, sz);
    }
    int i = 0;
    for (int child = purr_uiconf_first_child(s, node); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        if (purr_uiconf_kind(s, child) != PUI_KIND_ITEM) continue;
        if (i == idx) {
            if (purr_uiconf_attr_str(s, child, "target", out, sz)) return true;
            return purr_uiconf_attr_str(s, child, "label", out, sz);
        }
        i++;
    }
    return false;
}

// ── Unified selection model ──────────────────────────────────────────
// See this header's own comment. Both functions below walk top-level
// screen children in the SAME order/skip rules (ON_HANDLER and SECTION
// skipped, invisible nodes skipped) — they must never disagree about
// which flat index lands where.
int purr_uiconf_selectable_count(const purr_uiconf_screen_t *s)
{
    int count = 0;
    for (int child = purr_uiconf_first_child(s, PUI_ROOT); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        pui_kind_t k = purr_uiconf_kind(s, child);
        if (k == PUI_KIND_ON_HANDLER || k == PUI_KIND_SECTION) continue;
        if (!purr_uiconf_visible(s, child)) continue;
        if (k == PUI_KIND_WIDGET_LIST) {
            int n = purr_uiconf_list_item_count(s, child);
            if (n > 0) count += n;   // an empty list's "Empty" placeholder isn't selectable
        } else if (k == PUI_KIND_WIDGET_BUTTON) {
            count += 1;
        }
    }
    return count;
}

bool purr_uiconf_selectable_at(const purr_uiconf_screen_t *s, int flat_index, purr_uiconf_selectable_t *out)
{
    if (flat_index < 0) return false;
    int seen = 0;
    for (int child = purr_uiconf_first_child(s, PUI_ROOT); child != PUI_NONE;
         child = purr_uiconf_next_sibling(s, child)) {
        pui_kind_t k = purr_uiconf_kind(s, child);
        if (k == PUI_KIND_ON_HANDLER || k == PUI_KIND_SECTION) continue;
        if (!purr_uiconf_visible(s, child)) continue;
        if (k == PUI_KIND_WIDGET_LIST) {
            int n = purr_uiconf_list_item_count(s, child);
            if (n > 0 && flat_index < seen + n) {
                out->node = child;
                out->item_index = flat_index - seen;
                return true;
            }
            seen += n;
        } else if (k == PUI_KIND_WIDGET_BUTTON) {
            if (flat_index == seen) {
                out->node = child;
                out->item_index = -1;
                return true;
            }
            seen += 1;
        }
    }
    return false;
}
