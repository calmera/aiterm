// render_code: line-numbered, scrollable, simple keyword/string/comment highlighting.
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "render_tools.h"
#include "util/jsonbuf.h"
#include "util/platform.h"
#include "view_text.h"
#include "views.h"

static ui_view_t   s_view;
static textview_t* s_tv = NULL;

typedef struct { const char* lang; const char* const* kw; const char* line_comment; bool hash_comment; bool slash_comment; } lang_t;

static const char* const kw_c[] = {"auto","break","case","char","const","continue","default","do","double","else","enum","extern","float","for","goto","if","inline","int","long","register","return","short","signed","sizeof","static","struct","switch","typedef","union","unsigned","void","volatile","while","bool","true","false","NULL","uint8_t","uint16_t","uint32_t","uint64_t","int8_t","int16_t","int32_t","int64_t","size_t","#include","#define","#if","#ifdef","#ifndef","#endif","#else","#pragma",NULL};
static const char* const kw_cpp[] = {"auto","break","case","char","class","const","constexpr","continue","default","delete","do","double","else","enum","explicit","extern","float","for","friend","if","inline","int","long","namespace","new","nullptr","operator","override","private","protected","public","return","short","signed","sizeof","static","struct","switch","template","this","throw","true","false","try","catch","typedef","typename","union","unsigned","using","virtual","void","volatile","while","bool","std","#include","#define","#if","#ifdef","#endif",NULL};
static const char* const kw_py[] = {"and","as","assert","async","await","break","class","continue","def","del","elif","else","except","False","finally","for","from","global","if","import","in","is","lambda","None","nonlocal","not","or","pass","raise","return","True","try","while","with","yield","self","print",NULL};
static const char* const kw_js[] = {"async","await","break","case","catch","class","const","continue","debugger","default","delete","do","else","export","extends","finally","for","function","if","import","in","instanceof","let","new","null","undefined","return","super","switch","this","throw","true","false","try","typeof","var","void","while","with","yield","of",NULL};
static const char* const kw_ts[] = {"async","await","break","case","catch","class","const","continue","default","delete","do","else","enum","export","extends","finally","for","function","if","implements","import","in","instanceof","interface","let","new","null","undefined","return","super","switch","this","throw","true","false","try","type","typeof","var","void","while","yield","string","number","boolean","any","readonly","public","private","protected","of","as",NULL};
static const char* const kw_go[] = {"break","case","chan","const","continue","default","defer","else","fallthrough","for","func","go","goto","if","import","interface","map","package","range","return","select","struct","switch","type","var","nil","true","false","string","int","int64","uint8","byte","error","bool","float64",NULL};
static const char* const kw_rs[] = {"as","async","await","break","const","continue","crate","dyn","else","enum","extern","false","fn","for","if","impl","in","let","loop","match","mod","move","mut","pub","ref","return","self","Self","static","struct","super","trait","true","type","unsafe","use","where","while","i32","i64","u8","u32","u64","usize","f32","f64","bool","str","String","Vec","Option","Result","Some","None","Ok","Err",NULL};
static const char* const kw_sh[] = {"if","then","else","elif","fi","for","in","do","done","while","until","case","esac","function","return","local","export","echo","exit","set","source","read","test",NULL};
static const char* const kw_json[] = {"true","false","null",NULL};
static const char* const kw_yaml[] = {"true","false","null","yes","no",NULL};

static const lang_t k_langs[] = {
    {"c", kw_c, "//", false, true}, {"h", kw_c, "//", false, true}, {"cpp", kw_cpp, "//", false, true}, {"c++", kw_cpp, "//", false, true}, {"cc", kw_cpp, "//", false, true},
    {"python", kw_py, "#", true, false}, {"py", kw_py, "#", true, false},
    {"javascript", kw_js, "//", false, true}, {"js", kw_js, "//", false, true}, {"typescript", kw_ts, "//", false, true}, {"ts", kw_ts, "//", false, true},
    {"go", kw_go, "//", false, true}, {"rust", kw_rs, "//", false, true}, {"rs", kw_rs, "//", false, true},
    {"bash", kw_sh, "#", true, false}, {"sh", kw_sh, "#", true, false}, {"shell", kw_sh, "#", true, false}, {"zsh", kw_sh, "#", true, false},
    {"json", kw_json, NULL, false, false}, {"yaml", kw_yaml, "#", true, false}, {"yml", kw_yaml, "#", true, false},
};

static const lang_t* find_lang(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(k_langs) / sizeof(k_langs[0]); i++) if (strcasecmp(k_langs[i].lang, name) == 0) return &k_langs[i];
    return NULL;
}
static bool is_kw(const lang_t* l, const char* w, size_t n) {
    for (int i = 0; l->kw[i]; i++) if (strlen(l->kw[i]) == n && strncmp(l->kw[i], w, n) == 0) return true;
    return false;
}
static bool is_word(char c) { return isalnum((unsigned char)c) || c == '_' || c == '#'; }

// Highlight one source line into recolor markup.
char* code_highlight_line(const char* src, const lang_t* l, const ui_theme_t* th) {
    char kw[7], str[7], cm[7]; textview_hex(th->kw, kw); textview_hex(th->str, str); textview_hex(th->comment, cm);
    jsonbuf_t j; jb_init(&j);
    size_t n = strlen(src), i = 0;
    while (i < n) {
        char c = src[i];
        // comment to end of line
        if (l && ((l->hash_comment && c == '#' && !(i + 1 < n && (src[i + 1] == 'i' || src[i + 1] == 'd' || src[i + 1] == 'e' || src[i + 1] == 'p'))) || (l->slash_comment && c == '/' && i + 1 < n && src[i + 1] == '/'))) {
            char* e = textview_escape(src + i);
            jb_appendf(&j, "#%s %s#", cm, e); plat_free(e);
            break;
        }
        if (l && c == '/' && i + 1 < n && src[i + 1] == '*') {  // block comment (single line only)
            const char* end = strstr(src + i + 2, "*/");
            size_t len = end ? (size_t)(end + 2 - (src + i)) : n - i;
            char* e = plat_strndup(src + i, len); char* esc = textview_escape(e); plat_free(e);
            jb_appendf(&j, "#%s %s#", cm, esc); plat_free(esc);
            i += len; continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            size_t s = i; i++;
            while (i < n && src[i] != c) { if (src[i] == '\\' && i + 1 < n) i++; i++; }
            if (i < n) i++;
            char* e = plat_strndup(src + s, i - s); char* esc = textview_escape(e); plat_free(e);
            jb_appendf(&j, "#%s %s#", str, esc); plat_free(esc);
            continue;
        }
        if (l && is_word(c) && (i == 0 || !is_word(src[i - 1]))) {
            size_t s = i; while (i < n && is_word(src[i])) i++;
            if (is_kw(l, src + s, i - s)) { char* e = plat_strndup(src + s, i - s); char* esc = textview_escape(e); plat_free(e); jb_appendf(&j, "#%s %s#", kw, esc); plat_free(esc); }
            else { char* e = plat_strndup(src + s, i - s); char* esc = textview_escape(e); plat_free(e); jb_append_str(&j, esc); plat_free(esc); }
            continue;
        }
        if (c == '#') jb_append_str(&j, "##"); else jb_append_char(&j, c);
        i++;
    }
    return jb_detach(&j);
}

static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (k->is_nav && k->nav == UI_NAV_ESC) { render_tool_finish("{\"status\":\"shown\"}"); ui_pop(); return; }
    if (s_tv) textview_key(s_tv, k);
}
static void on_close(ui_view_t* v) { (void)v; render_tool_finish("{\"status\":\"shown\"}"); textview_destroy(s_tv); s_tv = NULL; }

void view_code_show(const char* code, const char* language, const char* title) {
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "code"; s_view.on_key = on_key; s_view.on_close = on_close;
    char status[160];
    snprintf(status, sizeof(status), "%s%s%s · ↑↓ PgUp PgDn scroll · ←→ pan · Esc close", title ? title : "Code", language ? " · " : "", language ? language : "");
    lv_obj_t* c = ui_view_create_root(&s_view, status);
    const lang_t* l = find_lang(language);
    const ui_theme_t* th = ui_theme();
    // split lines
    int n = 0; for (const char* p = code; *p; p++) if (*p == '\n') n++;
    if (code[0] && code[strlen(code) - 1] != '\n') n++;
    if (n == 0) n = 1;
    char** lines = (char**)plat_calloc(n, sizeof(char*));
    char mu[7]; textview_hex(th->muted, mu);
    int width = n >= 1000 ? 4 : n >= 100 ? 3 : 2;
    const char* p = code; int idx = 0;
    while (idx < n) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char* raw = plat_strndup(p, len);
        // expand tabs to 4 spaces
        jsonbuf_t ex; jb_init(&ex);
        for (char* q = raw; *q; q++) { if (*q == '\t') jb_append_str(&ex, "    "); else if (*q != '\r') jb_append_char(&ex, *q); }
        plat_free(raw);
        char* hl = code_highlight_line(jb_cstr(&ex), l, th);
        jb_free(&ex);
        jsonbuf_t j; jb_init(&j);
        jb_appendf(&j, "#%s %*d# %s", mu, width, idx + 1, hl);
        plat_free(hl);
        lines[idx++] = jb_detach(&j);
        if (!nl) break;
        p = nl + 1;
    }
    s_tv = textview_create(&s_view, c, 4);
    textview_set_lines(s_tv, lines, NULL, idx);
    ui_push(&s_view);
}
