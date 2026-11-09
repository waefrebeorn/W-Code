/*
 * wcode.h -- W-CODE: the WuBuOS agentic IDE
 *
 * Two frontends share one core:
 *   - a human TUI  (raw termios, panels, syntax colour)
 *   - an agent API (--agent: newline-delimited JSON, stdin/stdout)
 *
 * Both drive the same buffer, file-tree, build and diagnostic modules, so
 * what an agent does and what a human does cannot drift apart.
 *
 * C11, no dependencies beyond libc.
 */

#ifndef WCODE_H
#define WCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WCODE_VERSION   "0.1.0"
#define WCODE_MAX_PATH  512
#define WCODE_MAX_LINES 65536
#define WCODE_MAX_LINE  4096
#define WCODE_TABSTOP   4
#define WCODE_MAX_UNDO  256

/* ---- Text buffer -------------------------------------------------- */

typedef struct {
    char  **line;          /* array of NUL-terminated lines */
    int     n_lines;
    int     cap;
    char    path[WCODE_MAX_PATH];
    bool    modified;
} WCodeBuf;

WCodeBuf *wcode_buf_new(void);
void      wcode_buf_free(WCodeBuf *b);

int  wcode_buf_load(WCodeBuf *b, const char *path);   /* 0 = ok */
int  wcode_buf_save(WCodeBuf *b, const char *path);   /* 0 = ok */
int  wcode_buf_save_self(WCodeBuf *b);

void wcode_buf_clear(WCodeBuf *b);

/* Line access.  Never returns NULL -- out-of-range yields "". */
const char *wcode_buf_get(const WCodeBuf *b, int line);

/* Editing.  All coordinates are clamped. */
void wcode_buf_insert_text(WCodeBuf *b, int line, int col, const char *text);
void wcode_buf_insert_char(WCodeBuf *b, int line, int col, char ch);
void wcode_buf_delete_char(WCodeBuf *b, int line, int col);      /* before col */
void wcode_buf_delete_forward(WCodeBuf *b, int line, int col);   /* at col */
void wcode_buf_split_line(WCodeBuf *b, int line, int col);
void wcode_buf_join_lines(WCodeBuf *b, int line);                /* line + line+1 */
void wcode_buf_insert_line(WCodeBuf *b, int at, const char *text);
void wcode_buf_delete_line(WCodeBuf *b, int line);

/* ---- Editor state ------------------------------------------------- */

typedef struct {
    int  cx, cy;           /* cursor: line, column (col in BYTES) */
    int  sel_line, sel_col;/* selection anchor, -1 when no selection */
    int  rows, cols;       /* viewport size in cells */
    int  top;              /* first visible line */
} WCodeEd;

void wcode_ed_init(WCodeEd *ed);
void wcode_ed_attach(WCodeEd *ed, WCodeBuf *b);

void wcode_ed_key(WCodeEd *ed, WCodeBuf *b, const char *key);
/* key is a logical name: "up","down","left","right","home","end","pgup",
 * "pgdn","enter","tab","backspace","delete","ctrl-s","ctrl-z","ctrl-y",
 * "ctrl-a","ctrl-c","ctrl-x","ctrl-v","ctrl-f","ctrl-n","ctrl-o". */

void wcode_ed_insert_string(WCodeEd *ed, WCodeBuf *b, const char *s);
void wcode_ed_goto(WCodeEd *ed, WCodeBuf *b, int line, int col);

/* Selection, in buffer order.  Returns malloc'd text or NULL. */
char *wcode_ed_selection(const WCodeEd *ed, WCodeBuf *b);

/* ---- Syntax ------------------------------------------------------- */

typedef enum {
    WCODE_TOK_PLAIN = 0,
    WCODE_TOK_KEYWORD,
    WCODE_TOK_TYPE,
    WCODE_TOK_STRING,
    WCODE_TOK_COMMENT,
    WCODE_TOK_NUMBER,
    WCODE_TOK_PREPROC,
} WCodeTokKind;

/* Classify one token in `lang` ("c" or "holyc"). */
WCodeTokKind wcode_tok_classify(const char *s, size_t n, const char *lang,
                                int prev_kind);
/* Language detection from a path. */
const char  *wcode_lang_for_path(const char *path);

/* ---- Line scanning ------------------------------------------------ */
/* One coloured run of a line.  The TUI tokenises once per repaint. */
typedef struct {
    int          col;
    int          len;
    WCodeTokKind kind;
} WCodeRun;

int wcode_syntax_line(const char *line, const char *lang, WCodeRun *runs,
                      int max_runs);

/* ---- File tree ---------------------------------------------------- */

typedef struct WCodeNode {
    char   name[WCODE_MAX_PATH];
    char   path[WCODE_MAX_PATH];
    bool   is_dir;
    int    depth;
    bool   expanded;
    struct WCodeNode *child;   /* first child (dirs) */
    struct WCodeNode *next;    /* sibling */
} WCodeNode;

WCodeNode *wcode_tree_load(const char *root);
void       wcode_tree_free(WCodeNode *n);
WCodeNode *wcode_tree_toggle(WCodeNode *root, const char *path);
WCodeNode *wcode_tree_find(WCodeNode *root, const char *path);
int        wcode_tree_count(WCodeNode *root);

/* ---- Diagnostics + build ------------------------------------------- */

typedef struct {
    char  path[WCODE_MAX_PATH];
    int   line;
    int   col;
    char  severity[8];        /* "error" | "warning" | "note" */
    char  message[1024];
} WCodeDiag;

typedef struct {
    WCodeDiag *d;
    int        n;
    int        cap;
    int        errors;
    int        warnings;
} WCodeDiags;

void wcode_diags_init(WCodeDiags *dg);
void wcode_diags_free(WCodeDiags *dg);
void wcode_diags_add(WCodeDiags *dg, const char *path, int line, int col,
                     const char *sev, const char *msg);

/* Parse GCC/Clang style "path:line:col: severity: message" lines. */
void wcode_diags_parse_gcc(WCodeDiags *dg, const char *out);

/* Run `argv` in `cwd`, capture combined output.  Returns exit status. */
int  wcode_run(const char *cwd, char *const argv[], char **out, size_t *out_len);

/* ---- Agent interface ----------------------------------------------- */

/* Execute one agent command (a single JSON object on one line).
 * Emits exactly one JSON response line. */
void wcode_agent_handle(const char *req, char *resp, size_t resp_cap);

/* ---- Utility ------------------------------------------------------- */

/* JSON string escape into `out`. Returns chars written. */
int  wcode_json_escape(const char *in, char *out, size_t cap);
/* Extract a string field: {"name": "value"} -> malloc'd value. */
char *wcode_json_str(const char *json, const char *field);

#endif /* WCODE_H */