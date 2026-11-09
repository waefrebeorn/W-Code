/*
 * wcode_tui.c -- the human frontend.
 *
 * Panels:  file tree | editor
 *          -------------+---------------------------
 *          build output (toggle with F5)
 *          status bar
 *
 * Both frontends drive the same WCode state (see wcode_agent.c), so a key
 * binding and an agent op can never disagree about what the buffer is.
 *
 * Raw termios, no ncurses: W-CODE must run over a serial console and inside a
 * WuBuOS container without a terminfo database.
 */

#include "wcode.h"
#include "wcode_tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#define WCODE_UI_TREE_W   30
#define WCODE_UI_OUT_H    9

/* ---- Shared state (owned by wcode_agent.c) -------------------------- */

void wcode_init(const char *root);
void wcode_shutdown(void);
WCodeBuf *wcode_buffer(void);
WCodeNode *wcode_tree(void);

typedef struct {
    struct termios saved;
    int     raw;
    int     rows, cols;
    int     tree_sel;        /* index into the visible tree rows */
    int     out_scroll;
    bool    show_output;
    bool    show_help;
    char    message[256];
    int     message_ttl;
} TUI;

static TUI g_tui;
static WCodeEd g_ed;
static WCodeDiags g_diags;
static char *g_build_out;
static size_t g_build_out_len;
static int g_build_status;
static bool g_has_build;

/* ---- Terminal ------------------------------------------------------ */

static void term_raw_on(void) {
    if (g_tui.raw) return;
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) return;
    g_tui.saved = t;
    t.c_lflag &= ~(ECHO | ICANON | ISIG);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_tui.raw = 1;
    printf("\x1b[?25l");          /* hide cursor: we draw our own */
    fflush(stdout);
}

static void term_raw_off(void) {
    if (!g_tui.raw) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_tui.saved);
    g_tui.raw = 0;
    printf("\x1b[?25h\x1b[0m");
    fflush(stdout);
}

static void term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        g_tui.rows = ws.ws_row;
        g_tui.cols = ws.ws_col;
    } else {
        g_tui.rows = 24;
        g_tui.cols = 80;
    }
}

/* ---- Drawing ------------------------------------------------------- */

static const char *tok_colour(WCodeTokKind k) {
    switch (k) {
        case WCODE_TOK_KEYWORD: return "\x1b[35m";   /* magenta */
        case WCODE_TOK_TYPE:    return "\x1b[36m";   /* cyan */
        case WCODE_TOK_STRING:  return "\x1b[32m";   /* green */
        case WCODE_TOK_COMMENT: return "\x1b[90m";   /* grey */
        case WCODE_TOK_NUMBER:  return "\x1b[33m";   /* yellow */
        case WCODE_TOK_PREPROC: return "\x1b[34m";   /* blue */
        default:                return "\x1b[0m";
    }
}

/* Flatten the visible tree into `rows` (directories only when expanded). */
static void flatten_tree(WCodeNode *node, WCodeNode **rows, int cap, int *n) {
    for (WCodeNode *p = node; p && *n < cap; p = p->next) {
        if (p->depth >= 0) rows[(*n)++] = p;
        if (p->is_dir && p->expanded) flatten_tree(p->child, rows, cap, n);
    }
}

static void draw_tree(int x, int y, int w, int h) {
    WCodeNode *root = wcode_tree();
    if (!root) return;
    const char *lang = wcode_lang_for_path("x.c");
    (void)lang;

    /* Flatten the visible tree. */
    WCodeNode *rows[512];
    int n = 0;
    flatten_tree(root->child, rows, 512, &n);

    if (g_tui.tree_sel >= n) g_tui.tree_sel = n - 1;
    if (g_tui.tree_sel < 0) g_tui.tree_sel = 0;

    printf("\x1b[0m");
    for (int i = 0; i < h; i++) {
        int idx = i + (g_tui.tree_sel > h - 1 ? g_tui.tree_sel - h + 1 : 0);
        printf("\x1b[%d;%dH", y + i, x);
        if (idx >= n) { printf("%*s", w, ""); continue; }
        WCodeNode *p = rows[idx];
        char line[256];
        int ind = p->depth * 2;
        snprintf(line, sizeof(line), "%*s%s%s", ind, "",
                 p->is_dir ? (p->expanded ? "\xe2\x96\x91 " : "\xe2\x96\xb6 ") : "  ",
                 p->name);
        int len = (int)strlen(line);
        if (len > w) line[w] = '\0';
        if (idx == g_tui.tree_sel) {
            printf("\x1b[7m%-*s\x1b[0m", w, line);
        } else {
            printf("%-*s", w, line);
        }
    }
}

static void draw_editor(int x, int y, int w, int h) {
    WCodeBuf *b = wcode_buffer();
    if (!b) return;
    const char *lang = wcode_lang_for_path(b->path);

    if (g_ed.rows != h) g_ed.rows = h;
    if (g_ed.cy < g_ed.top) g_ed.top = g_ed.cy;
    if (g_ed.cy >= g_ed.top + h) g_ed.top = g_ed.cy - h + 1;

    for (int i = 0; i < h; i++) {
        int ln = g_ed.top + i;
        printf("\x1b[%d;%dH", y + i, x);
        if (ln >= b->n_lines) { printf("%*s", w, ""); continue; }

        const char *text = wcode_buf_get(b, ln);
        char cells[WCODE_UI_LINE_MAX + 1];
        int nc = 0;
        for (const char *p = text; *p && nc < w; p++)
            cells[nc++] = *p == '\t' ? ' ' : *p;
        cells[nc] = '\0';

        /* Line number gutter. */
        printf("\x1b[90m%4d \x1b[0m", ln + 1);

        /* Colourised body. */
        WCodeRun runs[256];
        int nr = wcode_syntax_line(text, lang, runs, 256);
        int prev = 0;
        for (int r = 0; r < nr; r++) {
            int from = runs[r].col;
            int to = from + runs[r].len;
            if (from > prev) {
                int k = from - prev;
                if (k > w) k = w;
                printf("%.*s", k, cells + prev);
            }
            int k = to - from;
            if (k > w - from) k = w - from;
            if (k > 0) {
                printf("%s", tok_colour(runs[r].kind));
                printf("%.*s", k, cells + from);
                prev = to;
            }
        }
        if (prev < nc) {
            printf("\x1b[0m");
            printf("%.*s", nc - prev < w - prev ? nc - prev : w - prev, cells + prev);
        }
        printf("\x1b[0m");
    }

    /* Cursor. */
    int cx = x + 5 + g_ed.cx;
    int cy = y + (g_ed.cy - g_ed.top);
    if (cy >= y && cy < y + h && cx < x + w) {
        printf("\x1b[%d;%dH\x1b[7m \x1b[0m", cy + 1, cx + 1);
    }
}

static void draw_output(int x, int y, int w, int h) {
    printf("\x1b[0m");
    if (!g_has_build) {
        for (int i = 0; i < h; i++) {
            printf("\x1b[%d;%dH%*s", y + i, x, w, "");
        }
        printf("\x1b[%d;%dH\x1b[90m  No build yet -- F5 to build\x1b[0m", y, x);
        return;
    }
    char *copy = strdup(g_build_out ? g_build_out : "");
    if (!copy) return;
    int total = 0;
    for (char *p = copy; *p; p++) if (*p == '\n') total++;
    total++;
    int max_scroll = total > h ? total - h : 0;
    if (g_tui.out_scroll > max_scroll) g_tui.out_scroll = max_scroll;
    if (g_tui.out_scroll < 0) g_tui.out_scroll = 0;

    char *lines[512];
    int n = 0;
    char *save = NULL;
    for (char *p = strtok_r(copy, "\n", &save); p && n < 512;
         p = strtok_r(NULL, "\n", &save))
        lines[n++] = p;

    for (int i = 0; i < h; i++) {
        printf("\x1b[%d;%dH", y + i, x);
        int idx = i + g_tui.out_scroll;
        if (idx >= n) { printf("%*s", w, ""); continue; }
        char buf[WCODE_UI_LINE_MAX + 1];
        snprintf(buf, sizeof(buf), "%.*s", w, lines[idx]);
        if (strstr(lines[idx], "error:") || strstr(lines[idx], "error :"))
            printf("\x1b[31m%-*s\x1b[0m", w, buf);
        else if (strstr(lines[idx], "warning:"))
            printf("\x1b[33m%-*s\x1b[0m", w, buf);
        else
            printf("%-*s", w, buf);
    }
    free(copy);
}

static void draw_status(int y) {
    WCodeBuf *b = wcode_buffer();
    printf("\x1b[%d;1H\x1b[7m", y);
    char left[256];
    snprintf(left, sizeof(left), " W-CODE %s  %s%s  Ln %d, Col %d  %d lines ",
             WCODE_VERSION,
             b->path[0] ? b->path : "[new]",
             b->modified ? " *" : "",
             g_ed.cy + 1, g_ed.cx + 1, b->n_lines);
    char right[128];
    if (g_has_build) {
        snprintf(right, sizeof(right), "%s  err %d  warn %d ",
                 g_build_status == 0 ? "BUILD OK" : "BUILD FAIL",
                 g_diags.errors, g_diags.warnings);
    } else {
        snprintf(right, sizeof(right), "no build ");
    }
    int lw = (int)strlen(left), rw = (int)strlen(right);
    int pad = g_tui.cols - lw - rw;
    if (pad < 0) pad = 0;
    printf("%s%*s%s", left, pad, "", right);
    printf("\x1b[0m");
    if (g_tui.message[0] && g_tui.message_ttl > 0) {
        printf("\x1b[%d;1H\x1b[33m%s\x1b[0m", y + 1, g_tui.message);
    }
}

static void draw_help(int y) {
    printf("\x1b[%d;1H\x1b[7m", y);
    const char *h = " F1 help  Ctrl-S save  Ctrl-O open  Ctrl-N new  F5 build  "
                    "F6 run  Tab complete  Ctrl-Q quit ";
    char buf[WCODE_UI_LINE_MAX + 1];
    snprintf(buf, sizeof(buf), "%-*.*s", g_tui.cols, g_tui.cols, h);
    printf("%s\x1b[0m", buf);
}

static void draw_full(void) {
    term_size();
    printf("\x1b[2J\x1b[H");
    int tree_w = WCODE_UI_TREE_W;
    if (g_tui.cols < tree_w + 20) tree_w = g_tui.cols / 3;
    int out_h = g_tui.show_output ? WCODE_UI_OUT_H : 0;
    int edit_h = g_tui.rows - 2 - out_h;
    if (edit_h < 3) edit_h = 3;

    draw_tree(1, 1, tree_w, edit_h);
    draw_editor(tree_w + 2, 1, g_tui.cols - tree_w - 2, edit_h);
    if (out_h) draw_output(1, edit_h + 1, g_tui.cols, out_h);
    draw_status(g_tui.rows - 1);
    draw_help(g_tui.rows);
    fflush(stdout);
}

/* ---- Prompts ------------------------------------------------------- */

static void prompt(const char *label, char *out, size_t cap) {
    term_raw_off();
    printf("\x1b[%d;1H\x1b[7m%s", g_tui.rows - 1, label);
    int lw = (int)strlen(label);
    printf("%*s\x1b[0m", g_tui.cols - lw > 0 ? g_tui.cols - lw : 0, "");
    fflush(stdout);
    if (fgets(out, (int)cap, stdin)) {
        size_t n = strlen(out);
        while (n && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = '\0';
    }
    term_raw_on();
}

/* ---- Build --------------------------------------------------------- */

static void do_build(void) {
    WCodeBuf *b = wcode_buffer();
    char cwd[WCODE_MAX_PATH];
    if (b->path[0]) {
        snprintf(cwd, sizeof(cwd), "%s", b->path);
        char *slash = strrchr(cwd, '/');
        if (slash) *slash = '\0';
        else snprintf(cwd, sizeof(cwd), ".");
    } else {
        snprintf(cwd, sizeof(cwd), ".");
    }

    const char *argv[] = { "make", "-j4", NULL };
    char *out = NULL;
    size_t len = 0;
    int status = wcode_run(cwd, (char *const *)argv, &out, &len);

    free(g_build_out);
    g_build_out = out;
    g_build_out_len = len;
    g_build_status = status;
    g_has_build = true;

    wcode_diags_free(&g_diags);
    wcode_diags_init(&g_diags);
    if (out) wcode_diags_parse_gcc(&g_diags, out);

    snprintf(g_tui.message, sizeof(g_tui.message),
             "build exit %d -- %d errors, %d warnings",
             status, g_diags.errors, g_diags.warnings);
    g_tui.message_ttl = 60;
    g_tui.show_output = true;
}

static void do_run(void) {
    /* Run the built artifact if the project produced one. */
    const char *argv[] = { "make", "run", NULL };
    char *out = NULL;
    size_t len = 0;
    int status = wcode_run(".", (char *const *)argv, &out, &len);

    free(g_build_out);
    g_build_out = out;
    g_build_out_len = len;
    g_build_status = status;
    g_has_build = true;

    wcode_diags_free(&g_diags);
    wcode_diags_init(&g_diags);
    if (out) wcode_diags_parse_gcc(&g_diags, out);

    snprintf(g_tui.message, sizeof(g_tui.message), "run exit %d", status);
    g_tui.message_ttl = 60;
    g_tui.show_output = true;
}

/* ---- Key handling -------------------------------------------------- */

static void handle_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return;

    /* Escape sequences. */
    if (c == 0x1b) {
        unsigned char seq[4];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return;
        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return;
            switch (seq[1]) {
                case 'A': wcode_ed_key(&g_ed, wcode_buffer(), "up"); return;
                case 'B': wcode_ed_key(&g_ed, wcode_buffer(), "down"); return;
                case 'C': wcode_ed_key(&g_ed, wcode_buffer(), "right"); return;
                case 'D': wcode_ed_key(&g_ed, wcode_buffer(), "left"); return;
                case 'H': wcode_ed_key(&g_ed, wcode_buffer(), "home"); return;
                case 'F': wcode_ed_key(&g_ed, wcode_buffer(), "end"); return;
                case '5':
                    if (read(STDIN_FILENO, &seq[2], 1) != 1) return;
                    if (seq[2] == '~') wcode_ed_key(&g_ed, wcode_buffer(), "pgup");
                    return;
                case '6':
                    if (read(STDIN_FILENO, &seq[2], 1) != 1) return;
                    if (seq[2] == '~') wcode_ed_key(&g_ed, wcode_buffer(), "pgdn");
                    return;
                case '3':
                    if (read(STDIN_FILENO, &seq[2], 1) != 1) return;
                    if (seq[2] == '~') wcode_ed_key(&g_ed, wcode_buffer(), "delete");
                    return;
                case '1':
                    if (read(STDIN_FILENO, &seq[2], 1) != 1) return;
                    if (seq[2] == '~') wcode_ed_key(&g_ed, wcode_buffer(), "home");
                    if (seq[2] == ';') { /* F1 is ESC[11~ */ }
                    return;
                case '2':
                    /* F5 = ESC[15~, F6 = ESC[17~ */
                    if (read(STDIN_FILENO, &seq[2], 1) != 1) return;
                    if (seq[2] == '1') {
                        unsigned char t;
                        if (read(STDIN_FILENO, &t, 1) == 1 && t == '5') {
                            if (read(STDIN_FILENO, &t, 1) == 1 && t == '~') do_build();
                        }
                    } else if (seq[2] == '3') {
                        unsigned char t;
                        if (read(STDIN_FILENO, &t, 1) == 1 && t == '7') {
                            if (read(STDIN_FILENO, &t, 1) == 1 && t == '~') do_run();
                        }
                    }
                    return;
                default: return;
            }
        }
        if (seq[0] == 'O') {
            /* Application cursor keys / F-keys. */
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return;
            switch (seq[1]) {
                case 'A': wcode_ed_key(&g_ed, wcode_buffer(), "up"); return;
                case 'B': wcode_ed_key(&g_ed, wcode_buffer(), "down"); return;
                case 'C': wcode_ed_key(&g_ed, wcode_buffer(), "right"); return;
                case 'D': wcode_ed_key(&g_ed, wcode_buffer(), "left"); return;
                case 'H': wcode_ed_key(&g_ed, wcode_buffer(), "home"); return;
                case 'F': wcode_ed_key(&g_ed, wcode_buffer(), "end"); return;
                case 'P': g_tui.show_help = !g_tui.show_help; return;   /* F1 */
                default: return;
            }
        }
        return;
    }

    switch (c) {
        case 0x01:  /* Ctrl-A: select all (no-op marker) */
            g_ed.sel_line = 0;
            g_ed.sel_col = 0;
            return;
        case 0x03:  /* Ctrl-C */
            return;
        case 0x0e:  /* Ctrl-N: new file */
            wcode_buf_clear(wcode_buffer());
            wcode_ed_init(&g_ed);
            return;
        case 0x06:  /* Ctrl-F: find (no-op marker) */
            snprintf(g_tui.message, sizeof(g_tui.message), "find: not bound");
            g_tui.message_ttl = 30;
            return;
        case 0x0f: { /* Ctrl-O: open */
            char path[WCODE_MAX_PATH];
            prompt(" Open: ", path, sizeof(path));
            if (path[0]) {
                wcode_buf_load(wcode_buffer(), path);
                wcode_ed_init(&g_ed);
                snprintf(g_tui.message, sizeof(g_tui.message), "opened %s", path);
                g_tui.message_ttl = 30;
            }
            return;
        }
        case 0x13: { /* Ctrl-S: save */
            int rc = wcode_buf_save_self(wcode_buffer());
            snprintf(g_tui.message, sizeof(g_tui.message),
                     rc == 0 ? "saved" : "save failed");
            g_tui.message_ttl = 30;
            return;
        }
        case 0x11:  /* Ctrl-Q */
            g_tui.show_help = false;
            return;
        case 0x7f:
        case 0x08:
            wcode_ed_key(&g_ed, wcode_buffer(), "backspace");
            return;
        case '\r':
        case '\n':
            wcode_ed_key(&g_ed, wcode_buffer(), "enter");
            return;
        case '\t':
            wcode_ed_key(&g_ed, wcode_buffer(), "tab");
            return;
        default:
            if (c >= 0x20) {
                char s[2] = { (char)c, '\0' };
                wcode_ed_insert_string(&g_ed, wcode_buffer(), s);
            }
            return;
    }
}

/* ---- Main loop ----------------------------------------------------- */

int wcode_tui_run(void) {
    term_raw_on();
    term_size();
    g_tui.tree_sel = 0;
    g_tui.show_output = false;

    for (;;) {
        draw_full();
        handle_key();
        if (g_tui.message_ttl > 0) g_tui.message_ttl--;
    }
    /* not reached */
    return 0;
}

void wcode_tui_stop(void) {
    term_raw_off();
}

void wcode_tui_init_editor(void) {
    wcode_ed_init(&g_ed);
}
