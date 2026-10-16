#include "linediff.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"

static bool push(diff_result_t* r, diff_kind_t k, const char* s, size_t n) {
    diff_line_t* nl = (diff_line_t*)plat_realloc(r->lines, sizeof(diff_line_t) * (r->n + 1));
    if (!nl) return false;
    r->lines = nl;
    r->lines[r->n].kind = k;
    r->lines[r->n].text = plat_strndup(s, n);
    r->n++;
    if (k == DL_ADD) r->adds++;
    if (k == DL_DEL) r->dels++;
    return true;
}

bool diff_parse_unified(const char* diff, diff_result_t* out) {
    memset(out, 0, sizeof(*out));
    const char* p = diff ? diff : "";
    bool in_hunk = false;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        if (n >= 4 && (strncmp(p, "--- ", 4) == 0 || strncmp(p, "+++ ", 4) == 0)) { push(out, DL_FILE, p, n); in_hunk = false; }
        else if (n >= 2 && strncmp(p, "@@", 2) == 0) { push(out, DL_HUNK, p, n); in_hunk = true; }
        else if (n >= 1 && p[0] == '+' && (in_hunk || out->n)) push(out, DL_ADD, p + 1, n - 1);
        else if (n >= 1 && p[0] == '-' && (in_hunk || out->n)) push(out, DL_DEL, p + 1, n - 1);
        else if (n >= 1 && p[0] == ' ' && in_hunk) push(out, DL_CONTEXT, p + 1, n - 1);
        else if (n == 0 && in_hunk) push(out, DL_CONTEXT, "", 0);
        else if (n >= 1 && p[0] == '\\') { /* "\ No newline at end of file" */ }
        else if (n > 0) push(out, DL_META, p, n);  // diff --git, index, etc.
        p = nl ? nl + 1 : p + n;
    }
    return out->n > 0;
}

static char** split_lines(const char* s, int* count) {
    int n = 0;
    char** arr = NULL;
    const char* p = s ? s : "";
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char** na = (char**)plat_realloc(arr, sizeof(char*) * (n + 1));
        if (!na) break;
        arr = na;
        arr[n++] = plat_strndup(p, len);
        p = nl ? nl + 1 : p + len;
    }
    *count = n;
    return arr;
}
static void free_lines(char** a, int n) { for (int i = 0; i < n; i++) plat_free(a[i]); plat_free(a); }

#define DIFF_MAX_LINES 1200

bool diff_compute(const char* old_text, const char* new_text, const char* path, diff_result_t* out) {
    memset(out, 0, sizeof(*out));
    int na = 0, nb = 0;
    char** a = split_lines(old_text, &na);
    char** b = split_lines(new_text, &nb);
    char hdr[300];
    snprintf(hdr, sizeof(hdr), "--- a/%s", path && *path ? path : "file"); push(out, DL_FILE, hdr, strlen(hdr));
    snprintf(hdr, sizeof(hdr), "+++ b/%s", path && *path ? path : "file"); push(out, DL_FILE, hdr, strlen(hdr));
    if (na > DIFF_MAX_LINES || nb > DIFF_MAX_LINES) {
        // too large for the DP table: whole-file replacement
        snprintf(hdr, sizeof(hdr), "@@ -1,%d +1,%d @@", na, nb); push(out, DL_HUNK, hdr, strlen(hdr));
        for (int i = 0; i < na; i++) push(out, DL_DEL, a[i], strlen(a[i]));
        for (int j = 0; j < nb; j++) push(out, DL_ADD, b[j], strlen(b[j]));
        free_lines(a, na); free_lines(b, nb);
        return true;
    }
    // LCS table (na+1)*(nb+1) of uint16
    size_t W = (size_t)nb + 1;
    uint16_t* L = (uint16_t*)plat_calloc(((size_t)na + 1) * W, sizeof(uint16_t));
    if (!L) { free_lines(a, na); free_lines(b, nb); return false; }
    for (int i = na - 1; i >= 0; i--) {
        for (int j = nb - 1; j >= 0; j--) {
            if (strcmp(a[i], b[j]) == 0) L[i * W + j] = (uint16_t)(L[(i + 1) * W + j + 1] + 1);
            else { uint16_t x = L[(i + 1) * W + j], y = L[i * W + j + 1]; L[i * W + j] = x > y ? x : y; }
        }
    }
    // Build the edit script: kinds per step
    typedef struct { char op; int ai, bi; } step_t;
    step_t* steps = (step_t*)plat_malloc(sizeof(step_t) * (size_t)(na + nb + 1));
    int ns = 0;
    int i = 0, j = 0;
    while (i < na || j < nb) {
        if (i < na && j < nb && strcmp(a[i], b[j]) == 0) { steps[ns++] = (step_t){' ', i, j}; i++; j++; }
        else if (i < na && (j >= nb || L[(i + 1) * W + j] >= L[i * W + j + 1])) { steps[ns++] = (step_t){'-', i, j}; i++; }
        else { steps[ns++] = (step_t){'+', i, j}; j++; }
    }
    plat_free(L);
    // Emit hunks with 3 lines of context
    const int C = 3;
    int k = 0;
    while (k < ns) {
        // find next change
        while (k < ns && steps[k].op == ' ') k++;
        if (k >= ns) break;
        int start = k - C; if (start < 0) start = 0;
        // extend end: last change such that gaps of context <= 2*C
        int end = k;
        int m = k;
        while (m < ns) {
            if (steps[m].op != ' ') { end = m; m++; continue; }
            // count context run
            int run = 0; int q = m;
            while (q < ns && steps[q].op == ' ') { run++; q++; }
            if (q >= ns || run > 2 * C) break;
            m = q;
        }
        int stop = end + C + 1; if (stop > ns) stop = ns;
        int a0 = steps[start].ai, b0 = steps[start].bi;
        int acount = 0, bcount = 0;
        for (int q = start; q < stop; q++) { if (steps[q].op != '+') acount++; if (steps[q].op != '-') bcount++; }
        snprintf(hdr, sizeof(hdr), "@@ -%d,%d +%d,%d @@", acount ? a0 + 1 : a0, acount, bcount ? b0 + 1 : b0, bcount);
        push(out, DL_HUNK, hdr, strlen(hdr));
        for (int q = start; q < stop; q++) {
            if (steps[q].op == ' ') push(out, DL_CONTEXT, a[steps[q].ai], strlen(a[steps[q].ai]));
            else if (steps[q].op == '-') push(out, DL_DEL, a[steps[q].ai], strlen(a[steps[q].ai]));
            else push(out, DL_ADD, b[steps[q].bi], strlen(b[steps[q].bi]));
        }
        k = stop;
    }
    plat_free(steps);
    free_lines(a, na); free_lines(b, nb);
    return true;
}

void diff_free(diff_result_t* r) {
    for (int i = 0; i < r->n; i++) plat_free(r->lines[i].text);
    plat_free(r->lines);
    memset(r, 0, sizeof(*r));
}
