#include "test_util.h"
#include "util/markdown.h"
#include "util/platform.h"

static const md_block_t* nth(const md_block_t* b, int i) { while (b && i-- > 0) b = b->next; return b; }

int main(void) {
    const char* doc =
        "# Title\n"
        "\n"
        "Para one with **bold**, `code` and *it* and a [link](http://x.y).\n"
        "Second line of same para.\n"
        "\n"
        "- item a\n"
        "- item **b**\n"
        "  - nested\n"
        "1. first\n"
        "2. second\n"
        "\n"
        "```python\n"
        "print('hi')\n"
        "x = 1\n"
        "```\n"
        "---\n"
        "## Table\n"
        "| a | b |\n"
        "|---|---|\n"
        "| 1 | 2 |\n"
        "| 3 | 4 |\n"
        "> quoted\n"
        "### Done\n";
    md_block_t* b = md_parse(doc);
    CHECK_INT(md_count(b), 13);
    const md_block_t* x = nth(b, 0);
    CHECK_INT(x->type, MD_HEADING); CHECK_INT(x->level, 1);
    char* t = md_runs_plain(x); CHECK_STR(t, "Title"); plat_free(t);
    x = nth(b, 1);
    CHECK_INT(x->type, MD_PARAGRAPH);
    t = md_runs_plain(x); CHECK_STR(t, "Para one with bold, code and it and a link (http://x.y). Second line of same para."); plat_free(t);
    // runs: "Para one with " | bold | ", " | code | " and " | italic | ...
    CHECK(x->nruns >= 6);
    CHECK_STR(x->runs[1].text, "bold"); CHECK_INT(x->runs[1].flags, MD_RUN_BOLD);
    CHECK_STR(x->runs[3].text, "code"); CHECK_INT(x->runs[3].flags, MD_RUN_CODE);
    CHECK_STR(x->runs[5].text, "it"); CHECK_INT(x->runs[5].flags, MD_RUN_ITALIC);
    x = nth(b, 2); CHECK_INT(x->type, MD_BULLET); CHECK_INT(x->level, 0);
    x = nth(b, 3); CHECK_INT(x->type, MD_BULLET); CHECK_INT(x->runs[1].flags, MD_RUN_BOLD);
    x = nth(b, 4); CHECK_INT(x->type, MD_BULLET); CHECK_INT(x->level, 1);
    t = md_runs_plain(x); CHECK_STR(t, "nested"); plat_free(t);
    x = nth(b, 5); CHECK_INT(x->type, MD_NUMBERED); CHECK_INT(x->number, 1);
    x = nth(b, 6); CHECK_INT(x->type, MD_NUMBERED); CHECK_INT(x->number, 2);
    x = nth(b, 7); CHECK_INT(x->type, MD_CODE); CHECK_STR(x->lang, "python"); CHECK_STR(x->code, "print('hi')\nx = 1\n");
    x = nth(b, 8); CHECK_INT(x->type, MD_HR);
    x = nth(b, 9); CHECK_INT(x->type, MD_HEADING); CHECK_INT(x->level, 2);
    x = nth(b, 10); CHECK_INT(x->type, MD_TABLE); CHECK_INT(x->ncols, 2); CHECK_INT(x->nrows, 3);
    CHECK_STR(x->cells[0], "a"); CHECK_STR(x->cells[1], "b"); CHECK_STR(x->cells[2], "1"); CHECK_STR(x->cells[5], "4");
    x = nth(b, 11); CHECK_INT(x->type, MD_QUOTE);
    t = md_runs_plain(x); CHECK_STR(t, "quoted"); plat_free(t);
    x = nth(b, 12); CHECK_INT(x->type, MD_HEADING); CHECK_INT(x->level, 3);
    md_free(b);
    // unterminated fence and escaped chars
    b = md_parse("```\nraw\n");
    CHECK_INT(md_count(b), 1); CHECK_INT(b->type, MD_CODE); CHECK_STR(b->code, "raw\n");
    md_free(b);
    b = md_parse("a \\*not italic\\* 2*3");
    t = md_runs_plain(b); CHECK_STR(t, "a *not italic* 2*3"); plat_free(t);
    CHECK_INT(b->nruns, 1);
    md_free(b);
    b = md_parse("");
    CHECK(b == NULL);
    TEST_MAIN_END("test_markdown");
}
