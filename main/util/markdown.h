// Minimal Markdown -> block list (§6.3). Host-tested. Supports headings (#..###), paragraphs,
// bullet/numbered lists (with indent levels), fenced code, inline code, bold/italic,
// horizontal rules, links (text (url)), block quotes and pipe tables.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MD_HEADING, MD_PARAGRAPH, MD_BULLET, MD_NUMBERED, MD_CODE, MD_HR, MD_TABLE, MD_QUOTE } md_block_type_t;

#define MD_RUN_BOLD   0x01
#define MD_RUN_ITALIC 0x02
#define MD_RUN_CODE   0x04

typedef struct { char* text; uint8_t flags; } md_run_t;

typedef struct md_block {
    md_block_type_t  type;
    int              level;    // heading level 1..6, list indent level 0.., quote depth
    int              number;   // numbered list item number
    md_run_t*        runs;     // inline runs (heading/paragraph/list/quote)
    int              nruns;
    char*            code;     // MD_CODE: raw text
    char*            lang;     // MD_CODE: language tag or ""
    char**           cells;    // MD_TABLE: nrows*ncols cell strings (row-major), row 0 = header
    int              nrows, ncols;
    struct md_block* next;
} md_block_t;

md_block_t* md_parse(const char* text);
char* md_inline_plain(const char* s);  // inline markup -> plain text (caller frees)
void        md_free(md_block_t* blocks);
int         md_count(const md_block_t* blocks);
// Concatenate the plain text of a block's runs (caller frees with plat_free).
char*       md_runs_plain(const md_block_t* b);

#ifdef __cplusplus
}
#endif
