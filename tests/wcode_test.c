/*
 * wcode_test.c -- W-CODE self-test.
 *
 * Exercises the core both frontends share: buffer, editor, syntax, file tree,
 * build/diagnostics, and the agent JSON protocol.  Run with `make test`.
 */

#include "wcode.h"
#include "wcode_tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); passed++; } \
} while (0)

static int failures = 0;
static int passed = 0;

/* Shared IDE state, defined in wcode_agent.c. */
void wcode_init(const char *root);
void wcode_shutdown(void);
WCodeBuf *wcode_buffer(void);
WCodeNode *wcode_tree(void);

static char *write_temp(const char *name, const char *content) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/wcode_test_%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fputs(content, f);
    fclose(f);
    return strdup(path);
}

/* ---- Buffer --------------------------------------------------------- */

static void test_buffer_basics(void) {
    printf("-- buffer basics\n");
    WCodeBuf *b = wcode_buf_new();
    CHECK(b != NULL, "buffer allocates");
    CHECK(b->n_lines == 1, "new buffer has one empty line");
    CHECK(strcmp(wcode_buf_get(b, 0), "") == 0, "empty buffer line is empty");

    wcode_buf_insert_text(b, 0, 0, "hello");
    CHECK(strcmp(wcode_buf_get(b, 0), "hello") == 0, "insert at 0,0");
    CHECK(b->modified, "insert marks modified");

    wcode_buf_insert_text(b, 0, 5, " world");
    CHECK(strcmp(wcode_buf_get(b, 0), "hello world") == 0, "append at end");

    wcode_buf_insert_text(b, 0, 11, "\nsecond");
    CHECK(b->n_lines == 2, "newline splits into two lines");
    CHECK(strcmp(wcode_buf_get(b, 0), "hello world") == 0, "insert at EOL keeps the head");
    CHECK(strcmp(wcode_buf_get(b, 1), "second") == 0, "tail lands on the new line");

    wcode_buf_delete_char(b, 0, 5);
    CHECK(strcmp(wcode_buf_get(b, 0), "hell world") == 0, "backspace deletes before cursor");

    wcode_buf_join_lines(b, 0);
    CHECK(b->n_lines == 1, "join_lines merges");
    CHECK(strcmp(wcode_buf_get(b, 0), "hell worldsecond") == 0, "join concatenates");

    wcode_buf_delete_line(b, 0);
    CHECK(b->n_lines == 1, "deleting the last line leaves one empty line");
    CHECK(strcmp(wcode_buf_get(b, 0), "") == 0, "and it is empty");

    wcode_buf_free(b);
}

static void test_buffer_roundtrip(void) {
    printf("-- buffer load/save roundtrip\n");
    const char *content = "line one\nline two\r\nline three\n";
    char *path = write_temp("rt.c", content);
    CHECK(path != NULL, "temp file written");

    WCodeBuf *b = wcode_buf_new();
    CHECK(wcode_buf_load(b, path) == 0, "load succeeds");
    CHECK(b->n_lines == 3, "three lines parsed");
    CHECK(strcmp(wcode_buf_get(b, 1), "line two") == 0, "CRLF stripped");
    CHECK(strcmp(b->path, path) == 0, "path recorded");
    CHECK(!b->modified, "load clears modified");

    wcode_buf_insert_text(b, 0, 0, "X");
    CHECK(b->modified, "edit sets modified");
    CHECK(wcode_buf_save_self(b) == 0, "save succeeds");
    CHECK(!b->modified, "save clears modified");

    /* Read it back. */
    WCodeBuf *c = wcode_buf_new();
    wcode_buf_load(c, path);
    CHECK(strcmp(wcode_buf_get(c, 0), "Xline one") == 0, "saved edit persisted");

    wcode_buf_free(b);
    wcode_buf_free(c);
    unlink(path);
    free(path);
}

static void test_buffer_clamp(void) {
    printf("-- buffer clamping\n");
    WCodeBuf *b = wcode_buf_new();
    wcode_buf_insert_text(b, 0, 0, "abc");
    /* Out-of-range coordinates must not corrupt anything. */
    wcode_buf_insert_text(b, 99, 0, "X");
    CHECK(b->n_lines == 1, "insert past the last line is a no-op");
    wcode_buf_insert_text(b, 0, 99, "Z");
    CHECK(strcmp(wcode_buf_get(b, 0), "abcZ") == 0, "column past EOL clamps to EOL");
    wcode_buf_delete_char(b, 0, 99);
    CHECK(strcmp(wcode_buf_get(b, 0), "abcZ") == 0, "delete past EOL is a no-op");
    wcode_buf_free(b);
}

/* ---- Editor --------------------------------------------------------- */

static void test_editor_motion(void) {
    printf("-- editor motion\n");
    WCodeBuf *b = wcode_buf_new();
    wcode_buf_insert_text(b, 0, 0, "abc\ndef\nghi");

    WCodeEd ed;
    wcode_ed_init(&ed);
    CHECK(ed.cy == 0 && ed.cx == 0, "cursor starts at 0,0");

    wcode_ed_key(&ed, b, "end");
    CHECK(ed.cx == 3, "end moves to line end");
    wcode_ed_key(&ed, b, "down");
    CHECK(ed.cy == 1 && ed.cx == 3, "down keeps the column");
    wcode_ed_key(&ed, b, "home");
    CHECK(ed.cx == 0, "home moves to column 0");
    wcode_ed_key(&ed, b, "left");
    CHECK(ed.cy == 0 && ed.cx == 3, "left at column 0 wraps to previous line end");
    wcode_ed_key(&ed, b, "right");
    CHECK(ed.cy == 1 && ed.cx == 0, "right at EOL wraps to next line start");

    wcode_buf_free(b);
}

static void test_editor_insert_delete(void) {
    printf("-- editor insert/delete\n");
    WCodeBuf *b = wcode_buf_new();
    wcode_buf_insert_text(b, 0, 0, "hello");

    WCodeEd ed;
    wcode_ed_init(&ed);
    wcode_ed_key(&ed, b, "end");
    wcode_ed_key(&ed, b, "enter");
    CHECK(b->n_lines == 2, "enter splits the line");
    CHECK(ed.cy == 1 && ed.cx == 0, "cursor moves to the new line");

    wcode_ed_insert_string(&ed, b, "world");
    CHECK(strcmp(wcode_buf_get(b, 1), "world") == 0, "insert_string writes at the cursor");

    wcode_ed_key(&ed, b, "backspace");
    CHECK(strcmp(wcode_buf_get(b, 1), "worl") == 0, "backspace deletes before the cursor");

    wcode_ed_key(&ed, b, "up");
    CHECK(ed.cy == 0, "up moves a line");
    wcode_ed_key(&ed, b, "down");
    wcode_ed_key(&ed, b, "home");
    wcode_ed_key(&ed, b, "backspace");
    CHECK(b->n_lines == 1, "backspace at column 0 joins lines");

    wcode_buf_free(b);
}

static void test_editor_tab(void) {
    printf("-- editor tab expansion\n");
    WCodeBuf *b = wcode_buf_new();
    WCodeEd ed;
    wcode_ed_init(&ed);
    wcode_ed_key(&ed, b, "tab");
    CHECK(strcmp(wcode_buf_get(b, 0), "    ") == 0, "tab inserts four spaces");
    CHECK(ed.cx == 4, "cursor advances by the tab stop");
    wcode_ed_key(&ed, b, "tab");
    CHECK(strcmp(wcode_buf_get(b, 0), "        ") == 0, "second tab completes the stop");
    wcode_buf_free(b);
}

/* ---- Syntax --------------------------------------------------------- */

static void test_syntax(void) {
    printf("-- syntax classification\n");
    CHECK(strcmp(wcode_lang_for_path("a.c"), "c") == 0, ".c is C");
    CHECK(strcmp(wcode_lang_for_path("a.h"), "c") == 0, ".h is C");
    CHECK(strcmp(wcode_lang_for_path("a.hc"), "holyc") == 0, ".hc is HolyC");
    /* .WET applets, VWS forms and Wua scripts are Lua. */
    CHECK(strcmp(wcode_lang_for_path("a.wet"), "lua") == 0, ".wet is Lua");
    CHECK(strcmp(wcode_lang_for_path("a.vws"), "lua") == 0, ".vws is Lua");
    CHECK(strcmp(wcode_lang_for_path("a.lua"), "lua") == 0, ".lua is Lua");
    CHECK(strcmp(wcode_lang_for_path("README.md"), "text") == 0, ".md is text");
    CHECK(strcmp(wcode_lang_for_path("noext"), "c") == 0, "no extension defaults to C");

    CHECK(wcode_tok_classify("return", 6, "c", 0) == WCODE_TOK_KEYWORD, "return is a keyword");
    CHECK(wcode_tok_classify("int", 3, "c", 0) == WCODE_TOK_KEYWORD, "int is a keyword");
    CHECK(wcode_tok_classify("foo", 3, "c", 0) == WCODE_TOK_PLAIN, "foo is plain");
    CHECK(wcode_tok_classify("// c", 4, "c", 0) == WCODE_TOK_COMMENT, "// is a comment");
    CHECK(wcode_tok_classify("/* c", 4, "c", 0) == WCODE_TOK_COMMENT, "/* is a comment");
    CHECK(wcode_tok_classify("\"s", 2, "c", 0) == WCODE_TOK_STRING, "string literal");
    CHECK(wcode_tok_classify("#include", 8, "c", 0) == WCODE_TOK_PREPROC, "preprocessor");
    CHECK(wcode_tok_classify("42", 2, "c", 0) == WCODE_TOK_NUMBER, "number literal");

    /* Lua: its own keywords, its own comment, and no preprocessor. */
    CHECK(wcode_tok_classify("function", 8, "lua", 0) == WCODE_TOK_KEYWORD,
          "function is a Lua keyword");
    CHECK(wcode_tok_classify("wet", 3, "lua", 0) == WCODE_TOK_KEYWORD,
          "wet is a Lua-surface keyword");
    CHECK(wcode_tok_classify("return", 6, "lua", 0) == WCODE_TOK_KEYWORD,
          "return is a Lua keyword");
    CHECK(wcode_tok_classify("foo", 3, "lua", 0) == WCODE_TOK_PLAIN,
          "foo is plain in Lua");
    CHECK(wcode_tok_classify("-- c", 4, "lua", 0) == WCODE_TOK_COMMENT,
          "-- is a Lua comment");
    CHECK(wcode_tok_classify("#x", 2, "lua", 0) != WCODE_TOK_PREPROC,
          "Lua has no preprocessor");
    {
        WCodeRun lr[32];
        int ln = wcode_syntax_line("local x = 1 -- note", "lua", lr, 32);
        int lk = 0, lc = 0;
        for (int i = 0; i < ln; i++) {
            if (lr[i].kind == WCODE_TOK_KEYWORD) lk = 1;
            if (lr[i].kind == WCODE_TOK_COMMENT) lc = 1;
        }
        CHECK(lk, "Lua keyword coloured on a line");
        CHECK(lc, "Lua -- comment coloured on a line");
    }

    WCodeRun runs[64];
    int n = wcode_syntax_line("int x = 42; // hi", "c", runs, 64);
    CHECK(n >= 4, "line splits into runs");
    int found_kw = 0, found_num = 0, found_comment = 0;
    for (int i = 0; i < n; i++) {
        if (runs[i].kind == WCODE_TOK_KEYWORD) found_kw = 1;
        if (runs[i].kind == WCODE_TOK_NUMBER) found_num = 1;
        if (runs[i].kind == WCODE_TOK_COMMENT) found_comment = 1;
    }
    CHECK(found_kw, "keyword coloured");
    CHECK(found_num, "number coloured");
    CHECK(found_comment, "comment coloured");
}

/* ---- File tree ------------------------------------------------------ */

static void test_tree(void) {
    printf("-- file tree\n");
    char dir[] = "/tmp/wcode_test_tree";
    char sub[256], f1[256], f2[256];
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(f1, sizeof(f1), "%s/a.c", dir);
    snprintf(f2, sizeof(f2), "%s/b.h", dir);

    if (mkdir(dir, 0755) != 0) { /* may already exist */ }
    if (mkdir(sub, 0755) != 0) { }
    FILE *f = fopen(f1, "w"); if (f) { fputs("a", f); fclose(f); }
    f = fopen(f2, "w"); if (f) { fputs("b", f); fclose(f); }

    WCodeNode *root = wcode_tree_load(dir);
    CHECK(root != NULL, "tree loads");
    CHECK(root->child != NULL, "root has children");
    CHECK(wcode_tree_count(root) >= 3, "tree counts entries");

    /* Directories sort before files. */
    CHECK(root->child->is_dir, "directory sorts first");
    CHECK(strcmp(root->child->name, "sub") == 0, "subdirectory named");

    WCodeNode *found = wcode_tree_find(root, f1);
    CHECK(found != NULL, "find locates a file");
    CHECK(!found->is_dir, "found node is a file");

    wcode_tree_toggle(root, sub);
    CHECK(wcode_tree_count(root) >= 3, "toggle does not lose entries");

    wcode_tree_free(root);

    unlink(f1); unlink(f2); rmdir(sub); rmdir(dir);
}

/* ---- Build + diagnostics -------------------------------------------- */

static void test_run(void) {
    printf("-- process run\n");
    char *out = NULL;
    size_t len = 0;
    const char *argv[] = { "printf", "hello\n", NULL };
    int rc = wcode_run("/tmp", (char *const *)argv, &out, &len);
    CHECK(rc == 0, "exit status captured");
    CHECK(out != NULL, "output captured");
    CHECK(len == 6, "output length correct");
    CHECK(out && strncmp(out, "hello\n", 6) == 0, "output content correct");
    free(out);

    const char *bad[] = { "false", NULL };
    rc = wcode_run("/tmp", (char *const *)bad, &out, &len);
    CHECK(rc == 1, "non-zero exit captured");
    free(out);
}

static void test_diag_parse(void) {
    printf("-- diagnostic parsing\n");
    WCodeDiags dg;
    wcode_diags_init(&dg);

    const char *gcc_out =
        "main.c:12:5: error: expected ';' before '}' token\n"
        "main.c:18:1: warning: unused variable 'x'\n"
        "from /usr/include/stdio.h:28:10: note: included from here\n"
        "make: *** [Makefile:5: all] Error 1\n";

    wcode_diags_parse_gcc(&dg, gcc_out);
    CHECK(dg.n == 3, "three diagnostics parsed");
    CHECK(dg.errors == 1, "one error");
    CHECK(dg.warnings == 1, "one warning");
    if (dg.n >= 1) {
        CHECK(strcmp(dg.d[0].path, "main.c") == 0, "path parsed");
        CHECK(dg.d[0].line == 12, "line parsed");
        CHECK(dg.d[0].col == 5, "column parsed");
        CHECK(strcmp(dg.d[0].severity, "error") == 0, "severity parsed");
        CHECK(strstr(dg.d[0].message, "expected") != NULL, "message parsed");
    }
    wcode_diags_free(&dg);
}

/* ---- JSON ----------------------------------------------------------- */

static void test_json(void) {
    printf("-- JSON helpers\n");
    char out[256];
    wcode_json_escape("a\"b\\c\nd", out, sizeof(out));
    CHECK(strcmp(out, "a\\\"b\\\\c\\nd") == 0, "escape quotes, backslash, newline");

    char *s = wcode_json_str("{\"op\": \"status\"}", "op");
    CHECK(s != NULL, "field extracted");
    CHECK(strcmp(s, "status") == 0, "field value correct");
    free(s);

    s = wcode_json_str("{\"text\": \"a\\nb\"}", "text");
    CHECK(s != NULL && strcmp(s, "a\nb") == 0, "escaped newline decoded");
    free(s);

    CHECK(wcode_json_str("{\"x\":1}", "missing") == NULL, "missing field returns NULL");
}

/* ---- Agent protocol -------------------------------------------------- */

static void test_agent(void) {
    printf("-- agent protocol\n");
    wcode_init("/tmp");

    char resp[70000];
    wcode_agent_handle("{\"op\":\"help\"}", resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"ok\"") != NULL, "help responds ok");
    CHECK(strstr(resp, "\"ops\"") != NULL, "help lists ops");

    wcode_agent_handle("{\"op\":\"status\"}", resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"ok\"") != NULL, "status responds ok");
    CHECK(strstr(resp, "\"version\"") != NULL, "status reports version");

    wcode_agent_handle("{\"op\":\"bogus\"}", resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"error\"") != NULL, "unknown op errors");

    /* Write a file through the agent, then read it back. */
    wcode_agent_handle(
        "{\"op\":\"write\",\"path\":\"/tmp/wcode_agent_test.c\","
        "\"text\":\"int main(void){return 0;}\\n\"}", resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"ok\"") != NULL, "write ok");

    wcode_agent_handle("{\"op\":\"read\",\"path\":\"/tmp/wcode_agent_test.c\"}",
                       resp, sizeof(resp));
    CHECK(strstr(resp, "int main") != NULL, "read returns the file text");

    /* Open into the buffer and edit it. */
    wcode_agent_handle("{\"op\":\"open\",\"path\":\"/tmp/wcode_agent_test.c\"}",
                       resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"ok\"") != NULL, "open ok");

    wcode_agent_handle("{\"op\":\"edit\",\"line\":1,\"col\":1,\"insert\":\"/* hi */\"}",
                       resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"ok\"") != NULL, "edit ok");

    wcode_agent_handle("{\"op\":\"save\"}", resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"ok\"") != NULL, "save ok");

    FILE *f = fopen("/tmp/wcode_agent_test.c", "r");
    if (f) {
        char buf[256] = {0};
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(strstr(buf, "/* hi */") != NULL, "agent edit persisted to disk");
    }

    /* Build a real command and read the diagnostics back. */
    wcode_agent_handle(
        "{\"op\":\"build\",\"cwd\":\"/tmp\",\"argv\":[\"sh\",\"-c\",\"echo 'x.c:1:1: error: boom'\"]}",
        resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"ok\"") != NULL, "build ok");
    CHECK(strstr(resp, "\"exit\":0") != NULL, "build exit status");

    wcode_agent_handle("{\"op\":\"diags\"}", resp, sizeof(resp));
    CHECK(strstr(resp, "\"errors\":1") != NULL, "diagnostics recorded");
    CHECK(strstr(resp, "boom") != NULL, "diagnostic message recorded");

    unlink("/tmp/wcode_agent_test.c");
    wcode_shutdown();
}

/* ---- The studio (VWS) ------------------------------------------------ */

/*
 * The IDE reaches the studio by shelling out, so the plumbing is testable with
 * a stub binary -- no need for the real VWS build to be present.  This proves
 * the ops; vws_templates_test proves the templates themselves.
 */
static void make_stub_vws(void) {
    const char *path = "/tmp/wcode_stub_vws";
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("#!/bin/sh\n"
          "case \"$1\" in\n"
          "  --list)\n"
          "    echo 'blank        Blank Applet   320x200   An empty applet'\n"
          "    echo 'dialog       Dialog         360x160   A prompt and OK/Cancel'\n"
          "    exit 0 ;;\n"
          "  --new)\n"
          "    case \"$2\" in\n"
          "      blank|dialog) ;;\n"
          "      *) echo \"vws: unknown template '$2'\" >&2; exit 2 ;;\n"
          "    esac\n"
          "    printf 'wet.applet {\\n  title = \"x\"\\n}\\n' > \"$3\"\n"
          "    echo \"vws: wrote $3\"\n"
          "    exit 0 ;;\n"
          "esac\n"
          "exit 2\n", f);
    fclose(f);
    chmod(path, 0755);
}

static void test_studio(void) {
    printf("-- studio (VWS) ops\n");
    char resp[70000];
    make_stub_vws();

    /* templates -- the picker table comes back as text. */
    wcode_agent_handle("{\"op\":\"templates\",\"vws\":\"/tmp/wcode_stub_vws\"}",
                       resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"ok\"") != NULL, "templates ok");
    CHECK(strstr(resp, "blank") != NULL, "templates lists blank");
    CHECK(strstr(resp, "dialog") != NULL, "templates lists dialog");

    /* template -- creates the file and opens it. */
    wcode_agent_handle(
        "{\"op\":\"template\",\"name\":\"dialog\",\"path\":\"/tmp/wcode_stub.vws\","
        "\"vws\":\"/tmp/wcode_stub_vws\"}", resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"ok\"") != NULL, "template ok");
    CHECK(strstr(resp, "/tmp/wcode_stub.vws") != NULL, "template returns the path");

    FILE *f = fopen("/tmp/wcode_stub.vws", "r");
    CHECK(f != NULL, "template wrote the file");
    if (f) {
        char buf[256] = {0};
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(strstr(buf, "wet.applet") != NULL, "written file is a .wet applet");
    }

    /* An unknown template is an error, not a crash. */
    wcode_agent_handle(
        "{\"op\":\"template\",\"name\":\"nope\","
        "\"vws\":\"/tmp/wcode_stub_vws\"}", resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"error\"") != NULL, "unknown template errors");

    /* A missing name is an error too. */
    wcode_agent_handle("{\"op\":\"template\",\"vws\":\"/tmp/wcode_stub_vws\"}",
                       resp, sizeof(resp));
    CHECK(strstr(resp, "\"status\":\"error\"") != NULL, "missing name errors");
}

int main(void) {
    printf("=== W-CODE self-test ===\n");
    test_buffer_basics();
    test_buffer_roundtrip();
    test_buffer_clamp();
    test_editor_motion();
    test_editor_insert_delete();
    test_editor_tab();
    test_syntax();
    test_tree();
    test_run();
    test_diag_parse();
    test_json();
    test_agent();
    test_studio();
    printf("\n=== wcode: %d passed, %d failed ===\n", passed, failures);
    return failures ? 1 : 0;
}
