#include "tools.h"
#include <stdio.h>
#include <string.h>
#include "util/platform.h"

static tool_entry_t s_tools[TOOLS_MAX];
static int          s_n = 0;
static tool_def_t   s_defs[TOOLS_MAX];
static int          s_render_n = 0;

#define S_STR "{\"type\":\"string\"}"
#define S_OPT_STR "{\"type\":\"string\"}"

static const struct { const char* name; const char* desc; const char* schema; } k_render[] = {
    {"render_markdown",
     "Show a long or structured answer (more than ~10 lines, or anything with headings/lists/code) full-screen on the device instead of printing it in the chat. Prefer this over long chat replies. Returns when the user closes the view.",
     "{\"type\":\"object\",\"properties\":{\"markdown\":{\"type\":\"string\",\"description\":\"Markdown text: headings, paragraphs, bullet/numbered lists, inline and fenced code, bold, horizontal rules, links, tables.\"},\"title\":{\"type\":\"string\"}},\"required\":[\"markdown\"]}"},
    {"render_code",
     "Show source code full-screen with line numbers and syntax highlighting. Use for any code longer than a couple of lines.",
     "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"language\":{\"type\":\"string\",\"description\":\"c, cpp, python, javascript, typescript, go, rust, bash, json, yaml\"},\"title\":{\"type\":\"string\"}},\"required\":[\"code\"]}"},
    {"render_diff",
     "Show a proposed code change as a diff and ask the user to accept or reject it before applying. Pass either a unified diff in `diff`, or `old` and `new` text (the device computes the diff). Returns {\"decision\":\"accepted\"} or {\"decision\":\"rejected\"}; only apply the change when accepted.",
     "{\"type\":\"object\",\"properties\":{\"diff\":{\"type\":\"string\",\"description\":\"Unified diff text\"},\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}}}"},
    {"render_table",
     "Show tabular data full-screen as a grid with a header row. Use instead of writing tables in chat.",
     "{\"type\":\"object\",\"properties\":{\"columns\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"rows\":{\"type\":\"array\",\"items\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"title\":{\"type\":\"string\"}},\"required\":[\"columns\",\"rows\"]}"},
    {"render_choice",
     "Ask the user to pick between options with a selectable list. Use this instead of asking in prose whenever there is a small set of choices. Returns {\"selected\":[indices]} or {\"cancelled\":true}.",
     "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"options\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"allow_multiple\":{\"type\":\"boolean\"}},\"required\":[\"prompt\",\"options\"]}"},
    {"render_image",
     "Show a PNG or JPEG image full-screen, from a URL or base64 data (max 2048x2048).",
     "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"},\"base64\":{\"type\":\"string\"},\"format\":{\"type\":\"string\",\"enum\":[\"png\",\"jpeg\"]},\"title\":{\"type\":\"string\"}}}"},
    {"render_chart",
     "Plot numeric data full-screen as a line, bar or pie chart.",
     "{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"line\",\"bar\",\"pie\"]},\"series\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"values\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}}},\"required\":[\"values\"]}},\"labels\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"title\":{\"type\":\"string\"},\"y_label\":{\"type\":\"string\"}},\"required\":[\"type\",\"series\"]}"},
};

static const char* k_prefix =
    "You are running on a small handheld device with a 720x720 screen and a physical keyboard. "
    "Keep chat replies short. For anything longer than a few lines, or anything structured "
    "(explanations, lists, code, tables, data), call the appropriate render_* tool instead of "
    "writing it in chat. Before changing code, show the change with render_diff and wait for "
    "acceptance. When you need the user to pick between options, use render_choice instead of "
    "asking in prose.";

const char* tools_system_prompt_prefix(void) { return k_prefix; }

static void rebuild_defs(void) {
    for (int i = 0; i < s_n; i++) {
        s_defs[i].name = s_tools[i].name;
        s_defs[i].description = s_tools[i].description;
        s_defs[i].schema_json = s_tools[i].schema;
    }
}

void tools_init(void) {
    s_n = 0;
    for (size_t i = 0; i < sizeof(k_render) / sizeof(k_render[0]); i++) {
        tool_entry_t* t = &s_tools[s_n++];
        t->name = (char*)k_render[i].name;
        t->description = (char*)k_render[i].desc;
        t->schema = (char*)k_render[i].schema;
        t->server = -1;
        t->remote_name = NULL;
    }
    s_render_n = s_n;
    rebuild_defs();
}
void tools_clear_mcp(void) {
    for (int i = s_render_n; i < s_n; i++) {
        plat_free(s_tools[i].name); plat_free(s_tools[i].description); plat_free(s_tools[i].schema); plat_free(s_tools[i].remote_name);
    }
    s_n = s_render_n;
    rebuild_defs();
}
bool tools_add_mcp(int server, const char* server_name, const char* tool, const char* description, const char* schema_json) {
    if (s_n >= TOOLS_MAX || !tool) return false;
    char name[160];
    // Sanitise the server name: letters, digits, _ and - only (OpenAI restricts tool names).
    char sname[64]; int k = 0;
    for (const char* p = server_name ? server_name : "mcp"; *p && k < 40; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        sname[k++] = ok ? c : '_';
    }
    if (k == 0) sname[k++] = 'm';
    sname[k] = 0;
    snprintf(name, sizeof(name), "%s__%s", sname, tool);
    for (int i = 0; i < s_n; i++) if (strcmp(s_tools[i].name, name) == 0) return false;  // duplicate
    tool_entry_t* t = &s_tools[s_n];
    t->name = plat_strdup(name);
    t->description = plat_strdup(description ? description : "");
    t->schema = plat_strdup(schema_json && *schema_json ? schema_json : "{\"type\":\"object\",\"properties\":{}}");
    t->server = server;
    t->remote_name = plat_strdup(tool);
    s_n++;
    rebuild_defs();
    return true;
}
int tools_count(void) { return s_n; }
int tools_mcp_count(void) { return s_n - s_render_n; }
const tool_def_t* tools_defs(int* n) { *n = s_n; return s_defs; }
const tool_entry_t* tools_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < s_n; i++) if (strcmp(s_tools[i].name, name) == 0) return &s_tools[i];
    return NULL;
}
bool tools_is_render(const char* name) { return name && strncmp(name, "render_", 7) == 0 && tools_find(name) && tools_find(name)->server == -1; }
