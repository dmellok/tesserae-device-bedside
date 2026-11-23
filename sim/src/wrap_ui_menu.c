/*
 * wrap_ui_menu.c: src/ui_menu.c as it is, plus a read-only dump of the menu
 * the panel is showing (/sim/menu), so a script can walk the menus by name,
 * and the words each layer draws (/sim/text), so it can read the home page.
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

/* The text ops of each layer as the firmware built them (not what the glass
 * shows): {"body":["06:30","in 9 min",...],"top":[...],"bar":[...],"card":[...]} */
size_t sim_text_json(char *b, size_t cap)
{
    const layer_t *L[4] = { &L_body, &L_top, &L_sb, &L_ring };
    static const char *const NAMES[4] = { "body", "top", "bar", "card" };
    size_t n = (size_t)snprintf(b, cap, "{");
    for (int k = 0; k < 4 && n < cap; k++) {
        n += (size_t)snprintf(b + n, cap - n, "%s\"%s\":[", k ? "," : "", NAMES[k]);
        int m = 0;
        for (int i = 0; L[k]->on && i < L[k]->n && n + 300 < cap; i++) {
            const op_t *o = &L[k]->ops[i];
            if (o->kind != OP_TEXT || !o->text[0] || (unsigned char)o->text[0] > 0x80) continue;   /* icons are one glyph above 0x80 */
            char t[200]; esc(t, sizeof t, o->text);
            n += (size_t)snprintf(b + n, cap - n, "%s\"%s\"", m++ ? "," : "", t);
        }
        n += (size_t)snprintf(b + n, cap - n, "]");
    }
    n += (size_t)snprintf(b + n, cap - n, "}");
    return n;
}
