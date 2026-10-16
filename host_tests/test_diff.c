#include "test_util.h"
#include "util/linediff.h"

int main(void) {
    diff_result_t r;
    const char* ud =
        "diff --git a/x.c b/x.c\n"
        "--- a/x.c\n"
        "+++ b/x.c\n"
        "@@ -1,3 +1,3 @@\n"
        " int a;\n"
        "-int b = 1;\n"
        "+int b = 2;\n"
        " int c;\n"
        "\\ No newline at end of file\n";
    CHECK(diff_parse_unified(ud, &r));
    CHECK_INT(r.n, 8);
    CHECK_INT(r.lines[0].kind, DL_META);
    CHECK_INT(r.lines[1].kind, DL_FILE); CHECK_STR(r.lines[1].text, "--- a/x.c");
    CHECK_INT(r.lines[3].kind, DL_HUNK);
    CHECK_INT(r.lines[4].kind, DL_CONTEXT); CHECK_STR(r.lines[4].text, "int a;");
    CHECK_INT(r.lines[5].kind, DL_DEL); CHECK_STR(r.lines[5].text, "int b = 1;");
    CHECK_INT(r.lines[6].kind, DL_ADD); CHECK_STR(r.lines[6].text, "int b = 2;");
    CHECK_INT(r.adds, 1); CHECK_INT(r.dels, 1);
    diff_free(&r);

    const char* old_t = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n";
    const char* new_t = "a\nb\nc\nX\ne\nf\ng\nh\ni\nj\nk\n";
    CHECK(diff_compute(old_t, new_t, "t.txt", &r));
    CHECK_STR(r.lines[0].text, "--- a/t.txt");
    CHECK_STR(r.lines[1].text, "+++ b/t.txt");
    CHECK_INT(r.lines[2].kind, DL_HUNK);
    CHECK_STR(r.lines[2].text, "@@ -1,10 +1,11 @@");  // gap of 6 context lines merges the two changes (like GNU diff)
    CHECK_INT(r.adds, 2); CHECK_INT(r.dels, 1);
    // d -> X replaced; k appended (second hunk)
    int hunks = 0; for (int i = 0; i < r.n; i++) if (r.lines[i].kind == DL_HUNK) hunks++;
    CHECK_INT(hunks, 1);
    // order inside first hunk: a b c -d +X e f g
    CHECK_INT(r.lines[3].kind, DL_CONTEXT); CHECK_STR(r.lines[3].text, "a");
    CHECK_INT(r.lines[6].kind, DL_DEL); CHECK_STR(r.lines[6].text, "d");
    CHECK_INT(r.lines[7].kind, DL_ADD); CHECK_STR(r.lines[7].text, "X");
    diff_free(&r);
    // identical -> only headers
    CHECK(diff_compute("same\n", "same\n", NULL, &r));
    CHECK_INT(r.n, 2); CHECK_INT(r.adds + r.dels, 0);
    diff_free(&r);
    // empty old
    CHECK(diff_compute("", "one\ntwo\n", "n", &r));
    CHECK_INT(r.adds, 2); CHECK_INT(r.dels, 0);
    CHECK_STR(r.lines[2].text, "@@ -0,0 +1,2 @@");
    diff_free(&r);
    TEST_MAIN_END("test_diff");
}
