#include "markdown.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "jsonbuf.h"
#include "platform.h"

static md_block_t* new_block(md_block_type_t t) { md_block_t* b = (md_block_t*)plat_calloc(1, sizeof(md_block_t)); if (b) b->type = t; return b; }

static void add_run(md_block_t* b, const char* s, size_t n, uint8_t flags) {
    if (n == 0) return;
    // merge with previous run if same flags
    if (b->nruns > 0 && b->runs[b->nruns - 1].flags == flags) {
        char* prev = b->runs[b->nruns - 1].text;
        size_t pl = strlen(prev);
        char* nt = (char*)plat_realloc(prev, pl + n + 1);
        if (!nt) return;
        memcpy(nt + pl, s, n); nt[pl + n] = 0;
        b->runs[b->nruns - 1].text = nt;
        return;
    }
    md_run_t* nr = (md_run_t*)plat_realloc(b->runs, sizeof(md_run_t) * (b->nruns + 1));
    if (!nr) return;
    b->runs = nr;
    b->runs[b->nruns].text = plat_strndup(s, n);
    b->runs[b->nruns].flags = flags;
    b->nruns++;
}

// Parse inline markup of `s` (length n) into runs.
static void parse_inline(md_block_t* b, const char* s, size_t n) {
    uint8_t flags = 0;
    size_t i = 0, start = 0;
    jsonbuf_t cur; jb_init(&cur);
    #define FLUSH() do { if (cur.len) { add_run(b, cur.data, cur.len, flags); jb_clear(&cur); } } while (0)
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) { jb_append_char(&cur, s[i + 1]); i += 2; continue; }
        if (c == '`') {
            // inline code: up to the next backtick
            const char* end = memchr(s + i + 1, '`', n - i - 1);
            if (end) {
                FLUSH();
                add_run(b, s + i + 1, (size_t)(end - (s + i + 1)), MD_RUN_CODE);
                i = (size_t)(end - s) + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {
            // bold toggle (needs a closing pair somewhere ahead)
            bool closing = (flags & MD_RUN_BOLD) != 0;
            bool has_close = false;
            if (!closing) { for (size_t j = i + 2; j + 1 < n; j++) if (s[j] == c && s[j + 1] == c) { has_close = true; break; } }
            if (closing || has_close) { FLUSH(); flags ^= MD_RUN_BOLD; i += 2; continue; }
        }
        if ((c == '*' || c == '_') && !(c == '_' && i > 0 && isalnum((unsigned char)s[i - 1]))) {
            bool closing = (flags & MD_RUN_ITALIC) != 0;
            bool has_close = false;
            if (!closing) { for (size_t j = i + 1; j < n; j++) if (s[j] == c && (j + 1 >= n || s[j + 1] != c)) { has_close = true; break; } }
            if ((closing || has_close) && !(i + 1 < n && s[i + 1] == ' ' && !closing)) { FLUSH(); flags ^= MD_RUN_ITALIC; i += 1; continue; }
        }
        if (c == '[') {
            const char* rb = memchr(s + i, ']', n - i);
            if (rb && (size_t)(rb - s) + 1 < n && rb[1] == '(') {
                const char* rp = memchr(rb + 2, ')', n - (size_t)(rb + 2 - s));
                if (rp) {
                    jb_append(&cur, s + i + 1, (size_t)(rb - (s + i + 1)));
                    jb_append_str(&cur, " (");
                    jb_append(&cur, rb + 2, (size_t)(rp - (rb + 2)));
                    jb_append_char(&cur, ')');
                    i = (size_t)(rp - s) + 1;
                    continue;
                }
            }
        }
        if (c == '!' && i + 1 < n && s[i + 1] == '[') {  // image: ![alt](url) -> [image: alt]
            const char* rb = memchr(s + i, ']', n - i);
            if (rb && (size_t)(rb - s) + 1 < n && rb[1] == '(') {
                const char* rp = memchr(rb + 2, ')', n - (size_t)(rb + 2 - s));
                if (rp) { jb_append_str(&cur, "[image: "); jb_append(&cur, s + i + 2, (size_t)(rb - (s + i + 2))); jb_append_char(&cur, ']'); i = (size_t)(rp - s) + 1; continue; }
            }
        }
        jb_append_char(&cur, c);
        i++;
    }
    FLUSH();
    jb_free(&cur);
    (void)start;
    #undef FLUSH
}

// Inline markup -> plain text (code spans without backticks, links as "text (url)"), for
// places that render a single monospace string such as table cells. Caller frees.
char* md_inline_plain(const char* s) {
    md_block_t tmp; memset(&tmp, 0, sizeof(tmp));
    parse_inline(&tmp, s, strlen(s));
    jsonbuf_t out; jb_init(&out);
    for (int i = 0; i < tmp.nruns; i++) jb_append_str(&out, tmp.runs[i].text);
    for (int i = 0; i < tmp.nruns; i++) plat_free(tmp.runs[i].text);
    plat_free(tmp.runs);
    char* r = plat_strdup(out.len ? out.data : "");
    jb_free(&out);
    return r;
}

static bool is_hr(const char* l, size_t n) {
    int cnt = 0; char ch = 0;
    for (size_t i = 0; i < n; i++) {
        if (l[i] == ' ') continue;
        if (l[i] == '-' || l[i] == '*' || l[i] == '_') { if (ch && l[i] != ch) return false; ch = l[i]; cnt++; }
        else return false;
    }
    return cnt >= 3;
}
static bool is_table_sep(const char* l, size_t n) {
    bool has_dash = false;
    for (size_t i = 0; i < n; i++) {
        char c = l[i];
        if (c == '-') has_dash = true;
        else if (c != '|' && c != ':' && c != ' ') return false;
    }
    return has_dash && memchr(l, '|', n) != NULL;
}
static int split_cells(const char* l, size_t n, char*** out) {
    // trim leading/trailing pipes
    while (n && (l[0] == ' ')) { l++; n--; }
    while (n && (l[n - 1] == ' ')) n--;
    if (n && l[0] == '|') { l++; n--; }
    if (n && l[n - 1] == '|') n--;
    int count = 0;
    char** cells = NULL;
    size_t st = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || (l[i] == '|' && (i == 0 || l[i - 1] != '\\'))) {
            size_t a = st, b = i;
            while (a < b && l[a] == ' ') a++;
            while (b > a && l[b - 1] == ' ') b--;
            char** nc = (char**)plat_realloc(cells, sizeof(char*) * (count + 1));
            if (!nc) break;
            cells = nc;
            cells[count++] = plat_strndup(l + a, b - a);
            st = i + 1;
        }
    }
    *out = cells;
    return count;
}

md_block_t* md_parse(const char* text) {
    md_block_t* head = NULL; md_block_t* tail = NULL;
    #define APPEND(b) do { md_block_t* _b = (b); if (!_b) break; if (tail) tail->next = _b; else head = _b; tail = _b; } while (0)
    const char* p = text ? text : "";
    jsonbuf_t para; jb_init(&para);
    md_block_t* code = NULL;   // open fenced block
    char fence_ch = 0; int fence_len = 0;
    jsonbuf_t code_buf; jb_init(&code_buf);
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        const char* line = p;
        if (n && line[n - 1] == '\r') n--;
        p = nl ? nl + 1 : p + n;
        // inside a fence
        if (code) {
            size_t k = 0; while (k < n && line[k] == ' ') k++;
            int fl = 0; while (k + fl < n && line[k + fl] == fence_ch) fl++;
            if (fl >= fence_len && k + fl == n) { code->code = jb_detach(&code_buf); jb_init(&code_buf); APPEND(code); code = NULL; continue; }
            jb_append(&code_buf, line, n); jb_append_char(&code_buf, '\n');
            continue;
        }
        // leading spaces
        size_t ind = 0; while (ind < n && line[ind] == ' ') ind++;
        const char* l = line + ind; size_t ln = n - ind;
        // blank line: ends paragraph
        if (ln == 0) {
            if (para.len) { md_block_t* b = new_block(MD_PARAGRAPH); parse_inline(b, para.data, para.len); APPEND(b); jb_clear(&para); }
            continue;
        }
        // fence start
        if (ln >= 3 && (l[0] == '`' || l[0] == '~') && l[1] == l[0] && l[2] == l[0]) {
            if (para.len) { md_block_t* b = new_block(MD_PARAGRAPH); parse_inline(b, para.data, para.len); APPEND(b); jb_clear(&para); }
            fence_ch = l[0]; fence_len = 0; while ((size_t)fence_len < ln && l[fence_len] == fence_ch) fence_len++;
            code = new_block(MD_CODE);
            size_t a = fence_len; while (a < ln && l[a] == ' ') a++;
            size_t e = a; while (e < ln && l[e] != ' ' && l[e] != '{') e++;
            code->lang = plat_strndup(l + a, e - a);
            jb_clear(&code_buf);
            continue;
        }
        // heading
        if (l[0] == '#') {
            int lvl = 0; while ((size_t)lvl < ln && l[lvl] == '#') lvl++;
            if (lvl <= 6 && ((size_t)lvl == ln || l[lvl] == ' ')) {
                if (para.len) { md_block_t* b = new_block(MD_PARAGRAPH); parse_inline(b, para.data, para.len); APPEND(b); jb_clear(&para); }
                md_block_t* b = new_block(MD_HEADING); b->level = lvl;
                size_t a = lvl; while (a < ln && l[a] == ' ') a++;
                size_t e = ln; while (e > a && (l[e - 1] == '#' || l[e - 1] == ' ')) e--;
                parse_inline(b, l + a, e - a);
                APPEND(b);
                continue;
            }
        }
        // hr
        if (is_hr(l, ln) && !(ln > 1 && l[0] == '-' && l[1] == ' ')) {
            if (para.len) { md_block_t* b = new_block(MD_PARAGRAPH); parse_inline(b, para.data, para.len); APPEND(b); jb_clear(&para); }
            APPEND(new_block(MD_HR));
            continue;
        }
        // table: a line with | followed by a separator line
        if (memchr(l, '|', ln) && p[0]) {
            const char* nl2 = strchr(p, '\n');
            size_t n2 = nl2 ? (size_t)(nl2 - p) : strlen(p);
            if (is_table_sep(p, n2)) {
                if (para.len) { md_block_t* b = new_block(MD_PARAGRAPH); parse_inline(b, para.data, para.len); APPEND(b); jb_clear(&para); }
                md_block_t* b = new_block(MD_TABLE);
                char** hdr = NULL; int ncols = split_cells(l, ln, &hdr);
                b->ncols = ncols; b->nrows = 1;
                b->cells = hdr;
                p = nl2 ? nl2 + 1 : p + n2;  // skip separator
                while (*p) {
                    const char* nl3 = strchr(p, '\n');
                    size_t n3 = nl3 ? (size_t)(nl3 - p) : strlen(p);
                    if (n3 && p[n3 - 1] == '\r') n3--;
                    if (!memchr(p, '|', n3)) break;
                    char** row = NULL; int rc = split_cells(p, n3, &row);
                    char** nc = (char**)plat_realloc(b->cells, sizeof(char*) * (size_t)ncols * (b->nrows + 1));
                    if (!nc) { for (int i = 0; i < rc; i++) plat_free(row[i]); plat_free(row); break; }
                    b->cells = nc;
                    for (int i = 0; i < ncols; i++) b->cells[b->nrows * ncols + i] = i < rc ? row[i] : plat_strdup("");
                    for (int i = ncols; i < rc; i++) plat_free(row[i]);
                    plat_free(row);
                    b->nrows++;
                    p = nl3 ? nl3 + 1 : p + n3;
                }
                APPEND(b);
                continue;
            }
        }
        // quote
        if (l[0] == '>') {
            if (para.len) { md_block_t* b = new_block(MD_PARAGRAPH); parse_inline(b, para.data, para.len); APPEND(b); jb_clear(&para); }
            size_t a = 1; while (a < ln && (l[a] == ' ' || l[a] == '>')) a++;
            md_block_t* b = new_block(MD_QUOTE); b->level = 1;
            parse_inline(b, l + a, ln - a);
            APPEND(b);
            continue;
        }
        // list items
        if (ln >= 2 && (l[0] == '-' || l[0] == '*' || l[0] == '+') && l[1] == ' ') {
            if (para.len) { md_block_t* b = new_block(MD_PARAGRAPH); parse_inline(b, para.data, para.len); APPEND(b); jb_clear(&para); }
            md_block_t* b = new_block(MD_BULLET); b->level = (int)(ind / 2);
            size_t a = 2; while (a < ln && l[a] == ' ') a++;
            // task list markers
            if (ln - a >= 3 && l[a] == '[' && l[a + 2] == ']') { add_run(b, l[a + 1] == ' ' ? "☐ " : "☑ ", 4, 0); a += 3; while (a < ln && l[a] == ' ') a++; }
            parse_inline(b, l + a, ln - a);
            APPEND(b);
            continue;
        }
        {
            size_t d = 0; while (d < ln && isdigit((unsigned char)l[d])) d++;
            if (d > 0 && d < 4 && d + 1 < ln && (l[d] == '.' || l[d] == ')') && l[d + 1] == ' ') {
                if (para.len) { md_block_t* b = new_block(MD_PARAGRAPH); parse_inline(b, para.data, para.len); APPEND(b); jb_clear(&para); }
                md_block_t* b = new_block(MD_NUMBERED); b->level = (int)(ind / 3); b->number = atoi(l);
                size_t a = d + 2; while (a < ln && l[a] == ' ') a++;
                parse_inline(b, l + a, ln - a);
                APPEND(b);
                continue;
            }
        }
        // paragraph continuation (soft wrap -> space; two trailing spaces -> newline)
        if (para.len) {
            if (para.len >= 2 && para.data[para.len - 1] == ' ' && para.data[para.len - 2] == ' ') { para.len -= 2; jb_append_char(&para, '\n'); }
            else jb_append_char(&para, ' ');
        }
        jb_append(&para, l, ln);
    }
    if (code) { code->code = jb_detach(&code_buf); APPEND(code); }
    if (para.len) { md_block_t* b = new_block(MD_PARAGRAPH); parse_inline(b, para.data, para.len); APPEND(b); }
    jb_free(&para); jb_free(&code_buf);
    #undef APPEND
    return head;
}

void md_free(md_block_t* b) {
    while (b) {
        md_block_t* n = b->next;
        for (int i = 0; i < b->nruns; i++) plat_free(b->runs[i].text);
        plat_free(b->runs);
        plat_free(b->code); plat_free(b->lang);
        if (b->cells) { for (int i = 0; i < b->nrows * b->ncols; i++) plat_free(b->cells[i]); plat_free(b->cells); }
        plat_free(b);
        b = n;
    }
}
int md_count(const md_block_t* b) { int n = 0; for (; b; b = b->next) n++; return n; }
char* md_runs_plain(const md_block_t* b) {
    jsonbuf_t j; jb_init(&j);
    for (int i = 0; i < b->nruns; i++) jb_append_str(&j, b->runs[i].text);
    return jb_detach(&j);
}
