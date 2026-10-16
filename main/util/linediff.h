// Line diffs for render_diff: parse a unified diff, or compute one from old/new text (LCS).
// Host-tested (host_tests/test_diff.c).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DL_FILE = 0, DL_HUNK, DL_CONTEXT, DL_ADD, DL_DEL, DL_META } diff_kind_t;
typedef struct { diff_kind_t kind; char* text; } diff_line_t;
typedef struct { diff_line_t* lines; int n; int adds, dels; } diff_result_t;

bool diff_parse_unified(const char* diff, diff_result_t* out);
// 3 lines of context; path used for the --- / +++ header (may be NULL).
bool diff_compute(const char* old_text, const char* new_text, const char* path, diff_result_t* out);
void diff_free(diff_result_t* r);

#ifdef __cplusplus
}
#endif
