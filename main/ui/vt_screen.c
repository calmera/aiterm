#include "vt_screen.h"
#include <stdio.h>
#include <string.h>
#include "util/platform.h"

enum { ST_GROUND = 0, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_CHARSET, ST_IGNORE1 };

static const uint32_t k_ansi16[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};
uint32_t vt_palette(int i) {
    if (i < 0) i = 0;
    if (i < 16) return k_ansi16[i];
    if (i < 232) { int v = i - 16; int r = v / 36, g = (v / 6) % 6, b = v % 6; const int lv[6] = {0, 95, 135, 175, 215, 255}; return ((uint32_t)lv[r] << 16) | ((uint32_t)lv[g] << 8) | (uint32_t)lv[b]; }
    if (i < 256) { int l = 8 + (i - 232) * 10; return ((uint32_t)l << 16) | ((uint32_t)l << 8) | (uint32_t)l; }
    return 0xFFFFFF;
}

static vt_cell_t* at(vt_screen_t* t, int r, int c) { return &t->cells[r * t->cols + c]; }
static void dirty(vt_screen_t* t, int r) { if (r >= 0 && r < t->rows) t->dirty[r] = true; }
static void blank(vt_screen_t* t, vt_cell_t* c) { c->cp = 0; c->fg = t->cur_fg; c->bg = t->cur_bg; c->attr = 0; }
static void clear_cells(vt_screen_t* t, int r, int c0, int c1) {  // inclusive
    if (r < 0 || r >= t->rows) return;
    if (c0 < 0) c0 = 0;
    if (c1 >= t->cols) c1 = t->cols - 1;
    for (int c = c0; c <= c1; c++) blank(t, at(t, r, c));
    dirty(t, r);
}
static void clear_rows(vt_screen_t* t, int r0, int r1) { for (int r = r0; r <= r1; r++) clear_cells(t, r, 0, t->cols - 1); }

bool vt_init(vt_screen_t* t, int cols, int rows, uint32_t def_fg, uint32_t def_bg) {
    memset(t, 0, sizeof(*t));
    t->def_fg = def_fg; t->def_bg = def_bg;
    t->cols = cols; t->rows = rows;
    t->cells = (vt_cell_t*)plat_calloc((size_t)cols * rows, sizeof(vt_cell_t));
    t->tabstops = (uint8_t*)plat_calloc((size_t)cols, 1);
    t->dirty = (bool*)plat_calloc((size_t)rows, sizeof(bool));
    if (!t->cells || !t->tabstops || !t->dirty) { vt_free(t); return false; }
    vt_reset(t);
    return true;
}
void vt_free(vt_screen_t* t) { plat_free(t->cells); plat_free(t->tabstops); plat_free(t->dirty); t->cells = NULL; t->tabstops = NULL; t->dirty = NULL; }
void vt_reset(vt_screen_t* t) {
    t->cur_fg = t->def_fg; t->cur_bg = t->def_bg; t->cur_attr = 0;
    t->cx = t->cy = 0; t->scroll_top = 0; t->scroll_bot = t->rows - 1;
    t->wrap_pending = false; t->autowrap = true; t->origin_mode = false; t->cursor_visible = true; t->insert_mode = false; t->app_cursor_keys = false;
    t->state = ST_GROUND; t->utf8_need = 0; t->resp_len = 0;
    for (int c = 0; c < t->cols; c++) t->tabstops[c] = (c % 8 == 0);
    clear_rows(t, 0, t->rows - 1);
    vt_mark_all_dirty(t);
}
bool vt_resize(vt_screen_t* t, int cols, int rows) {
    if (cols == t->cols && rows == t->rows) return true;
    vt_cell_t* nc = (vt_cell_t*)plat_calloc((size_t)cols * rows, sizeof(vt_cell_t));
    uint8_t* nt = (uint8_t*)plat_calloc((size_t)cols, 1);
    bool* nd = (bool*)plat_calloc((size_t)rows, sizeof(bool));
    if (!nc || !nt || !nd) { plat_free(nc); plat_free(nt); plat_free(nd); return false; }
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
        vt_cell_t* d = &nc[r * cols + c];
        if (r < t->rows && c < t->cols) *d = *at(t, r, c); else { d->cp = 0; d->fg = t->def_fg; d->bg = t->def_bg; d->attr = 0; }
    }
    for (int c = 0; c < cols; c++) nt[c] = c < t->cols ? t->tabstops[c] : (c % 8 == 0);
    plat_free(t->cells); plat_free(t->tabstops); plat_free(t->dirty);
    t->cells = nc; t->tabstops = nt; t->dirty = nd; t->cols = cols; t->rows = rows;
    if (t->cx >= cols) t->cx = cols - 1;
    if (t->cy >= rows) t->cy = rows - 1;
    t->scroll_top = 0; t->scroll_bot = rows - 1;
    vt_mark_all_dirty(t);
    return true;
}
const vt_cell_t* vt_cell(const vt_screen_t* t, int row, int col) { return &t->cells[row * t->cols + col]; }
bool vt_row_dirty(const vt_screen_t* t, int row) { return t->dirty[row]; }
void vt_clear_dirty(vt_screen_t* t) { memset(t->dirty, 0, (size_t)t->rows * sizeof(bool)); }
void vt_mark_all_dirty(vt_screen_t* t) { for (int r = 0; r < t->rows; r++) t->dirty[r] = true; }
size_t vt_take_response(vt_screen_t* t, char* out, size_t n) {
    size_t k = (size_t)t->resp_len < n ? (size_t)t->resp_len : n;
    memcpy(out, t->response, k);
    t->resp_len = 0;
    return k;
}
static void respond(vt_screen_t* t, const char* s) {
    size_t l = strlen(s);
    if (t->resp_len + l < sizeof(t->response)) { memcpy(t->response + t->resp_len, s, l); t->resp_len += (int)l; }
}

// ---------------------------------------------------------------------------------------
static void scroll_up(vt_screen_t* t, int top, int bot, int n) {
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    memmove(at(t, top, 0), at(t, top + n, 0), sizeof(vt_cell_t) * (size_t)t->cols * (size_t)(bot - top + 1 - n));
    for (int r = bot - n + 1; r <= bot; r++) clear_cells(t, r, 0, t->cols - 1);
    for (int r = top; r <= bot; r++) dirty(t, r);
}
static void scroll_down(vt_screen_t* t, int top, int bot, int n) {
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    memmove(at(t, top + n, 0), at(t, top, 0), sizeof(vt_cell_t) * (size_t)t->cols * (size_t)(bot - top + 1 - n));
    for (int r = top; r < top + n; r++) clear_cells(t, r, 0, t->cols - 1);
    for (int r = top; r <= bot; r++) dirty(t, r);
}
static void linefeed(vt_screen_t* t) {
    if (t->cy == t->scroll_bot) scroll_up(t, t->scroll_top, t->scroll_bot, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}
static void reverse_index(vt_screen_t* t) {
    if (t->cy == t->scroll_top) scroll_down(t, t->scroll_top, t->scroll_bot, 1);
    else if (t->cy > 0) t->cy--;
}
static void put_char(vt_screen_t* t, uint32_t cp) {
    if (t->wrap_pending) {
        if (t->autowrap) { t->cx = 0; linefeed(t); }
        t->wrap_pending = false;
    }
    if (t->insert_mode && t->cx < t->cols - 1) {
        memmove(at(t, t->cy, t->cx + 1), at(t, t->cy, t->cx), sizeof(vt_cell_t) * (size_t)(t->cols - 1 - t->cx));
    }
    vt_cell_t* c = at(t, t->cy, t->cx);
    c->cp = cp; c->fg = t->cur_fg; c->bg = t->cur_bg; c->attr = t->cur_attr;
    dirty(t, t->cy);
    if (t->cx == t->cols - 1) t->wrap_pending = true; else t->cx++;
}
static void set_cursor(vt_screen_t* t, int row, int col) {
    int top = t->origin_mode ? t->scroll_top : 0, bot = t->origin_mode ? t->scroll_bot : t->rows - 1;
    if (t->origin_mode) row += top;
    if (row < top) row = top;
    if (row > bot) row = bot;
    if (col < 0) col = 0;
    if (col > t->cols - 1) col = t->cols - 1;
    t->cy = row; t->cx = col; t->wrap_pending = false;
}
static int P(vt_screen_t* t, int i, int def) { return (i < t->nparams && t->params[i] > 0) ? t->params[i] : def; }

static void sgr(vt_screen_t* t) {
    if (t->nparams == 0) { t->cur_fg = t->def_fg; t->cur_bg = t->def_bg; t->cur_attr = 0; return; }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) { t->cur_fg = t->def_fg; t->cur_bg = t->def_bg; t->cur_attr = 0; }
        else if (p == 1) t->cur_attr |= VT_ATTR_BOLD;
        else if (p == 2) t->cur_attr |= VT_ATTR_DIM;
        else if (p == 3) t->cur_attr |= VT_ATTR_ITALIC;
        else if (p == 4) t->cur_attr |= VT_ATTR_UNDERLINE;
        else if (p == 7) t->cur_attr |= VT_ATTR_INVERSE;
        else if (p == 22) t->cur_attr &= ~(VT_ATTR_BOLD | VT_ATTR_DIM);
        else if (p == 23) t->cur_attr &= ~VT_ATTR_ITALIC;
        else if (p == 24) t->cur_attr &= ~VT_ATTR_UNDERLINE;
        else if (p == 27) t->cur_attr &= ~VT_ATTR_INVERSE;
        else if (p >= 30 && p <= 37) t->cur_fg = vt_palette(p - 30);
        else if (p == 39) t->cur_fg = t->def_fg;
        else if (p >= 40 && p <= 47) t->cur_bg = vt_palette(p - 40);
        else if (p == 49) t->cur_bg = t->def_bg;
        else if (p >= 90 && p <= 97) t->cur_fg = vt_palette(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->cur_bg = vt_palette(p - 100 + 8);
        else if ((p == 38 || p == 48) && i + 1 < t->nparams) {
            int mode = t->params[i + 1];
            uint32_t col = 0; bool ok = false;
            if (mode == 5 && i + 2 < t->nparams) { col = vt_palette(t->params[i + 2]); i += 2; ok = true; }
            else if (mode == 2 && i + 4 < t->nparams) { col = ((uint32_t)(t->params[i + 2] & 255) << 16) | ((uint32_t)(t->params[i + 3] & 255) << 8) | (uint32_t)(t->params[i + 4] & 255); i += 4; ok = true; }
            if (ok) { if (p == 38) t->cur_fg = col; else t->cur_bg = col; }
        }
    }
}

static void csi_dispatch(vt_screen_t* t, char final) {
    int n;
    switch (final) {
        case 'A': t->cy -= P(t, 0, 1); if (t->cy < (t->origin_mode ? t->scroll_top : 0)) t->cy = t->origin_mode ? t->scroll_top : 0; t->wrap_pending = false; break;
        case 'B': t->cy += P(t, 0, 1); if (t->cy > (t->origin_mode ? t->scroll_bot : t->rows - 1)) t->cy = t->origin_mode ? t->scroll_bot : t->rows - 1; t->wrap_pending = false; break;
        case 'C': t->cx += P(t, 0, 1); if (t->cx > t->cols - 1) t->cx = t->cols - 1; t->wrap_pending = false; break;
        case 'D': t->cx -= P(t, 0, 1); if (t->cx < 0) t->cx = 0; t->wrap_pending = false; break;
        case 'E': t->cx = 0; t->cy += P(t, 0, 1); if (t->cy > t->rows - 1) t->cy = t->rows - 1; break;
        case 'F': t->cx = 0; t->cy -= P(t, 0, 1); if (t->cy < 0) t->cy = 0; break;
        case 'G': case '`':
            t->cx = P(t, 0, 1) - 1;
            if (t->cx > t->cols - 1) t->cx = t->cols - 1;
            if (t->cx < 0) t->cx = 0;
            t->wrap_pending = false;
            break;
        case 'd': set_cursor(t, P(t, 0, 1) - 1 - (t->origin_mode ? 0 : 0), t->cx); break;
        case 'H': case 'f': set_cursor(t, P(t, 0, 1) - 1, P(t, 1, 1) - 1); break;
        case 'J': {
            int m = t->nparams ? t->params[0] : 0;
            if (m == 0) { clear_cells(t, t->cy, t->cx, t->cols - 1); clear_rows(t, t->cy + 1, t->rows - 1); }
            else if (m == 1) { clear_rows(t, 0, t->cy - 1); clear_cells(t, t->cy, 0, t->cx); }
            else clear_rows(t, 0, t->rows - 1);
            break;
        }
        case 'K': {
            int m = t->nparams ? t->params[0] : 0;
            if (m == 0) clear_cells(t, t->cy, t->cx, t->cols - 1);
            else if (m == 1) clear_cells(t, t->cy, 0, t->cx);
            else clear_cells(t, t->cy, 0, t->cols - 1);
            break;
        }
        case 'L': if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot) scroll_down(t, t->cy, t->scroll_bot, P(t, 0, 1)); t->cx = 0; break;
        case 'M': if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot) scroll_up(t, t->cy, t->scroll_bot, P(t, 0, 1)); t->cx = 0; break;
        case 'P': {  // delete chars
            n = P(t, 0, 1); if (n > t->cols - t->cx) n = t->cols - t->cx;
            memmove(at(t, t->cy, t->cx), at(t, t->cy, t->cx + n), sizeof(vt_cell_t) * (size_t)(t->cols - t->cx - n));
            clear_cells(t, t->cy, t->cols - n, t->cols - 1);
            break;
        }
        case '@': {  // insert blanks
            n = P(t, 0, 1); if (n > t->cols - t->cx) n = t->cols - t->cx;
            memmove(at(t, t->cy, t->cx + n), at(t, t->cy, t->cx), sizeof(vt_cell_t) * (size_t)(t->cols - t->cx - n));
            clear_cells(t, t->cy, t->cx, t->cx + n - 1);
            break;
        }
        case 'X': n = P(t, 0, 1); clear_cells(t, t->cy, t->cx, t->cx + n - 1); break;
        case 'S': scroll_up(t, t->scroll_top, t->scroll_bot, P(t, 0, 1)); break;
        case 'T': scroll_down(t, t->scroll_top, t->scroll_bot, P(t, 0, 1)); break;
        case 'r': {
            int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
            if (bot >= t->rows) bot = t->rows - 1;
            if (top < 0) top = 0;
            if (top < bot) { t->scroll_top = top; t->scroll_bot = bot; set_cursor(t, 0, 0); }
            break;
        }
        case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
        case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; t->wrap_pending = false; break;
        case 'm': sgr(t); break;
        case 'h': case 'l': {
            bool on = final == 'h';
            for (int i = 0; i < t->nparams; i++) {
                int p = t->params[i];
                if (t->priv) {
                    if (p == 25) t->cursor_visible = on;
                    else if (p == 7) t->autowrap = on;
                    else if (p == 6) { t->origin_mode = on; set_cursor(t, 0, 0); }
                    else if (p == 1) t->app_cursor_keys = on;
                    else if (p == 1049 || p == 47 || p == 1047) { clear_rows(t, 0, t->rows - 1); if (p == 1049 && on) { t->saved_cx = t->cx; t->saved_cy = t->cy; } }
                } else if (p == 4) t->insert_mode = on;
            }
            break;
        }
        case 'n': {
            int p = t->nparams ? t->params[0] : 0;
            if (p == 6) { char b[32]; snprintf(b, sizeof(b), "\x1b[%d;%dR", t->cy + 1 - (t->origin_mode ? t->scroll_top : 0), t->cx + 1); respond(t, b); }
            else if (p == 5) respond(t, "\x1b[0n");
            break;
        }
        case 'c': if (!t->priv) respond(t, "\x1b[?62;22c"); break;  // VT220 with colour
        case 'g': { int p = t->nparams ? t->params[0] : 0; if (p == 0) t->tabstops[t->cx] = 0; else if (p == 3) memset(t->tabstops, 0, (size_t)t->cols); break; }
        case 'I': n = P(t, 0, 1); while (n-- > 0 && t->cx < t->cols - 1) { do t->cx++; while (t->cx < t->cols - 1 && !t->tabstops[t->cx]); } break;
        case 'Z': n = P(t, 0, 1); while (n-- > 0 && t->cx > 0) { do t->cx--; while (t->cx > 0 && !t->tabstops[t->cx]); } break;
        default: break;  // ignore unknown
    }
}

static void esc_dispatch(vt_screen_t* t, char c) {
    switch (c) {
        case '7': t->saved_cx = t->cx; t->saved_cy = t->cy; t->saved_fg = t->cur_fg; t->saved_bg = t->cur_bg; t->saved_attr = t->cur_attr; break;
        case '8': t->cx = t->saved_cx; t->cy = t->saved_cy; t->cur_fg = t->saved_fg; t->cur_bg = t->saved_bg; t->cur_attr = t->saved_attr; t->wrap_pending = false; break;
        case 'D': linefeed(t); break;
        case 'E': t->cx = 0; linefeed(t); break;
        case 'M': reverse_index(t); break;
        case 'H': t->tabstops[t->cx] = 1; break;
        case 'c': vt_reset(t); break;
        default: break;  // = > N O etc.
    }
}

static void control(vt_screen_t* t, unsigned char c) {
    switch (c) {
        case 0x07: t->bell = true; break;
        case 0x08: if (t->cx > 0) t->cx--; t->wrap_pending = false; break;
        case 0x09: { int x = t->cx; do x++; while (x < t->cols - 1 && !t->tabstops[x]); t->cx = x > t->cols - 1 ? t->cols - 1 : x; break; }
        case 0x0A: case 0x0B: case 0x0C: linefeed(t); break;
        case 0x0D: t->cx = 0; t->wrap_pending = false; break;
        default: break;
    }
}

void vt_feed(vt_screen_t* t, const char* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        // UTF-8 continuation
        if (t->utf8_need > 0) {
            if ((c & 0xC0) == 0x80) { t->utf8_cp = (t->utf8_cp << 6) | (c & 0x3F); if (--t->utf8_need == 0) put_char(t, t->utf8_cp); continue; }
            t->utf8_need = 0;  // invalid: fall through and process c normally
            put_char(t, 0xFFFD);
        }
        switch (t->state) {
            case ST_GROUND:
                if (c == 0x1B) { t->state = ST_ESC; break; }
                if (c < 0x20 || c == 0x7F) { control(t, c); break; }
                if (c < 0x80) { put_char(t, c); break; }
                if ((c & 0xE0) == 0xC0) { t->utf8_cp = c & 0x1F; t->utf8_need = 1; }
                else if ((c & 0xF0) == 0xE0) { t->utf8_cp = c & 0x0F; t->utf8_need = 2; }
                else if ((c & 0xF8) == 0xF0) { t->utf8_cp = c & 0x07; t->utf8_need = 3; }
                else put_char(t, 0xFFFD);
                break;
            case ST_ESC:
                if (c == '[') { t->state = ST_CSI; t->nparams = 0; t->params[0] = 0; t->priv = false; t->inter = 0; break; }
                if (c == ']') { t->state = ST_OSC; t->osclen = 0; break; }
                if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%') { t->state = ST_CHARSET; break; }
                if (c == 'P' || c == '^' || c == '_') { t->state = ST_OSC; t->osclen = 0; break; }  // DCS/PM/APC: swallow until ST
                t->state = ST_GROUND;
                if (c < 0x20) control(t, c); else esc_dispatch(t, (char)c);
                break;
            case ST_CSI:
                if (c >= '0' && c <= '9') { if (t->nparams == 0) t->nparams = 1; int* p = &t->params[t->nparams - 1]; *p = *p * 10 + (c - '0'); if (*p > 32767) *p = 32767; break; }
                if (c == ';') { if (t->nparams == 0) t->nparams = 1; if (t->nparams < 16) { t->params[t->nparams] = 0; t->nparams++; } break; }
                if (c == '?' || c == '>' || c == '<' || c == '=') { t->priv = (c == '?'); break; }
                if (c == ':') { if (t->nparams == 0) t->nparams = 1; if (t->nparams < 16) { t->params[t->nparams] = 0; t->nparams++; } break; }  // treat ':' like ';'
                if (c >= 0x20 && c <= 0x2F) { t->inter = (char)c; break; }
                if (c < 0x20) { control(t, c); break; }
                t->state = ST_GROUND;
                if (c >= 0x40 && c <= 0x7E) { if (t->nparams == 0) t->params[0] = 0; csi_dispatch(t, (char)c); }
                break;
            case ST_OSC:
                if (c == 0x07) { t->state = ST_GROUND; t->osc[t->osclen] = 0; if (t->osclen > 2 && (t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') snprintf(t->title, sizeof(t->title), "%s", t->osc + 2); break; }
                if (c == 0x1B) { t->state = ST_OSC_ESC; break; }
                if (t->osclen < (int)sizeof(t->osc) - 1) t->osc[t->osclen++] = (char)c;
                break;
            case ST_OSC_ESC:
                t->state = ST_GROUND;
                t->osc[t->osclen] = 0;
                if (c == '\\' && t->osclen > 2 && (t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') snprintf(t->title, sizeof(t->title), "%s", t->osc + 2);
                break;
            case ST_CHARSET:
                t->state = ST_GROUND;  // ignore the designator
                break;
            default:
                t->state = ST_GROUND;
                break;
        }
    }
}

void vt_row_text(const vt_screen_t* t, int row, char* buf, size_t n) {
    size_t k = 0;
    int last = -1;
    for (int c = 0; c < t->cols; c++) { uint32_t cp = vt_cell(t, row, c)->cp; if (cp && cp != ' ') last = c; }
    for (int c = 0; c <= last && k + 5 < n; c++) {
        uint32_t cp = vt_cell(t, row, c)->cp;
        if (cp == 0) cp = ' ';
        if (cp < 0x80) buf[k++] = (char)cp;
        else if (cp < 0x800) { buf[k++] = (char)(0xC0 | (cp >> 6)); buf[k++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { buf[k++] = (char)(0xE0 | (cp >> 12)); buf[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[k++] = (char)(0x80 | (cp & 0x3F)); }
        else { buf[k++] = (char)(0xF0 | (cp >> 18)); buf[k++] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[k++] = (char)(0x80 | (cp & 0x3F)); }
    }
    buf[k] = 0;
}
