/*
 * wrap_ui_menu.c: src/ui_menu.c as it is, plus a read-only dump of the menu
 * the panel is showing (/sim/menu), so a script can walk the menus by name.
 */
#include "ui_menu.c"
#include "sim.h"

static size_t esc(char *b, size_t cap, const char *s)
{
    size_t n = 0;
    for (; s && *s && n + 4 < cap; s++) {               /* the panel font's own codes: 0x7f degree, 0x80 middle dot, 0x81+ icons */
        unsigned char c = (unsigned char)*s;
        if (c == 0x7f)      { b[n++] = (char)0xC2; b[n++] = (char)0xB0; }
        else if (c == 0x80) { b[n++] = (char)0xC2; b[n++] = (char)0xB7; }
        else if (c > 0x80)  b[n++] = '#';
        else { if (c == '"' || c == '\\') b[n++] = '\\'; b[n++] = c < 32 ? ' ' : (char)c; }
    }
    b[n] = 0;
    return n;
}

size_t sim_menu_json(char *b, size_t cap)
{
    static const char *const MODES[] = { "home", "list", "value", "choice" };
    static const char *const KINDS[] = { "toggle", "value", "choice", "sub", "info", "action", "back" };
    size_t n = (size_t)snprintf(b, cap, "{\"mode\":\"%s\",\"depth\":%d", MODES[s_mode & 3], s_depth);
    if (s_depth > 0 && n < cap) {
        node_t nd = s_stack[s_depth - 1].node;
        row_def_t r[ROWS_MAX];
        int rows = node_rows(nd, r);
        char t[160];
        esc(t, sizeof t, node_title(nd));
        n += (size_t)snprintf(b + n, cap - n, ",\"node\":%d,\"title\":\"%s\",\"sel\":%d,\"rows\":[", (int)nd, t, s_stack[s_depth - 1].sel);
        for (int i = 0; i < rows && n < cap - 300; i++) {
            char v[96] = "", ev[200], el[120];
            row_value(&r[i], v, sizeof v);
            esc(ev, sizeof ev, v); esc(el, sizeof el, r[i].label ? r[i].label : "");
            n += (size_t)snprintf(b + n, cap - n, "%s{\"label\":\"%s\",\"kind\":\"%s\",\"value\":\"%s\",\"id\":%d}", i ? "," : "", el,
                                  KINDS[r[i].kind < 7 ? r[i].kind : 6], ev, (int)r[i].id);
        }
        n += (size_t)snprintf(b + n, cap - n, "]");
    }
    n += (size_t)snprintf(b + n, cap - n, "}");
    return n;
}
