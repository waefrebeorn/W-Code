/*
 * wcode_buf.c -- text buffer + editor operations.
 *
 * The buffer is a growable array of NUL-terminated lines.  Coordinates are
 * (line, byte-column); all entry points clamp, so a caller can pass a stale
 * cursor after a buffer change without corrupting anything.
 */

#include "wcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- Growth helpers -------------------------------------------------- */

static int buf_reserve(WCodeBuf *b, int need) {
    if (need <= b->cap) return 0;
    int cap = b->cap ? b->cap : 8;
    while (cap < need) cap *= 2;
    char **nl = (char **)realloc(b->line, (size_t)cap * sizeof(char *));
    if (!nl) return -1;
    b->line = nl;
    b->cap = cap;
    return 0;
}

static char *dup_line(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* -- Lifecycle ------------------------------------------------------- */

WCodeBuf *wcode_buf_new(void) {
    WCodeBuf *b = (WCodeBuf *)calloc(1, sizeof(WCodeBuf));
    if (!b) return NULL;
    b->n_lines = 1;
    b->cap = 8;
    b->line = (char **)calloc((size_t)b->cap, sizeof(char *));
    if (!b->line) { free(b); return NULL; }
    b->line[0] = dup_line("");
    return b;
}

void wcode_buf_free(WCodeBuf *b) {
    if (!b) return;
    for (int i = 0; i < b->n_lines; i++) free(b->line[i]);
    free(b->line);
    free(b);
}

void wcode_buf_clear(WCodeBuf *b) {
    if (!b) return;
    /* The buffer may be zero-initialised (the shared IDE state in
     * wcode_agent.c is memset, not wcode_buf_new). */
    if (!b->line) {
        b->cap = 8;
        b->line = (char **)calloc((size_t)b->cap, sizeof(char *));
        if (!b->line) return;
    }
    for (int i = 0; i < b->n_lines; i++) free(b->line[i]);
    b->n_lines = 1;
    b->line[0] = dup_line("");
    b->path[0] = '\0';
    b->modified = false;
}

const char *wcode_buf_get(const WCodeBuf *b, int line) {
    if (!b || line < 0 || line >= b->n_lines) return "";
    return b->line[line] ? b->line[line] : "";
}

/* -- Load / save ----------------------------------------------------- */

int wcode_buf_load(WCodeBuf *b, const char *path) {
    if (!b || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    for (int i = 0; i < b->n_lines; i++) free(b->line[i]);
    b->n_lines = 0;

    char *buf = NULL;
    size_t cap = 0, len = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (len + 2 > cap) {
            size_t nc = cap ? cap * 2 : 4096;
            char *nb = (char *)realloc(buf, nc);
            if (!nb) { free(buf); fclose(f); return -1; }
            buf = nb; cap = nc;
        }
        buf[len++] = (char)ch;
    }
    fclose(f);
    if (!buf) buf = (char *)malloc(1);
    if (!buf) return -1;
    buf[len] = '\0';

    /* Split on '\n', stripping a trailing '\r'. */
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            /* A trailing newline terminates the last line; it does not
             * create an extra empty one. */
            if (i == len && len > 0 && buf[len - 1] == '\n') break;
            size_t n = i - start;
            if (n > 0 && buf[start + n - 1] == '\r') n--;
            char save = buf[start + n];
            buf[start + n] = '\0';
            if (buf_reserve(b, b->n_lines + 1) != 0) { free(buf); return -1; }
            b->line[b->n_lines++] = dup_line(buf + start);
            buf[start + n] = save;
            start = i + 1;
        }
    }
    free(buf);

    if (b->n_lines == 0) {
        b->n_lines = 1;
        b->line[0] = dup_line("");
    }
    snprintf(b->path, sizeof(b->path), "%s", path);
    b->modified = false;
    return 0;
}

int wcode_buf_save(WCodeBuf *b, const char *path) {
    if (!b || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (int i = 0; i < b->n_lines; i++) {
        fputs(b->line[i], f);
        if (i + 1 < b->n_lines) fputc('\n', f);
    }
    if (fclose(f) != 0) return -1;
    snprintf(b->path, sizeof(b->path), "%s", path);
    b->modified = false;
    return 0;
}

int wcode_buf_save_self(WCodeBuf *b) {
    if (!b || b->path[0] == '\0') return -1;
    return wcode_buf_save(b, b->path);
}

/* -- Line operations ------------------------------------------------- */

void wcode_buf_insert_line(WCodeBuf *b, int at, const char *text) {
    if (!b || at < 0) return;
    if (at > b->n_lines) at = b->n_lines;
    if (b->n_lines >= WCODE_MAX_LINES) return;
    if (buf_reserve(b, b->n_lines + 1) != 0) return;
    memmove(&b->line[at + 1], &b->line[at],
            (size_t)(b->n_lines - at) * sizeof(char *));
    b->line[at] = dup_line(text ? text : "");
    b->n_lines++;
    b->modified = true;
}

void wcode_buf_delete_line(WCodeBuf *b, int line) {
    if (!b || line < 0 || line >= b->n_lines) return;
    free(b->line[line]);
    memmove(&b->line[line], &b->line[line + 1],
            (size_t)(b->n_lines - line - 1) * sizeof(char *));
    b->n_lines--;
    if (b->n_lines == 0) {
        b->line[0] = dup_line("");
        b->n_lines = 1;
    }
    b->modified = true;
}

/* -- Character operations -------------------------------------------- */

void wcode_buf_insert_char(WCodeBuf *b, int line, int col, char ch) {
    wcode_buf_insert_text(b, line, col, (char[]){ ch, '\0' });
}

void wcode_buf_insert_text(WCodeBuf *b, int line, int col, const char *text) {
    if (!b || !text) return;
    if (line < 0 || line >= b->n_lines) return;
    const char *cur = wcode_buf_get(b, line);
    int len = (int)strlen(cur);
    if (col < 0) col = 0;
    if (col > len) col = len;

    size_t ins = strlen(text);
    /* Strip '\r'; split embedded newlines into separate lines. */
    char *tmp = (char *)malloc(ins + 1);
    if (!tmp) return;
    size_t n = 0;
    for (size_t i = 0; i < ins; i++)
        if (text[i] != '\r') tmp[n++] = text[i];
    tmp[n] = '\0';

    char *nl = strchr(tmp, '\n');
    if (!nl) {
        size_t total = (size_t)len + n + 1;
        if (total > WCODE_MAX_LINE) { free(tmp); return; }
        char *out = (char *)malloc(total);
        if (!out) { free(tmp); return; }
        memcpy(out, cur, (size_t)col);
        memcpy(out + col, tmp, n);
        memcpy(out + col + n, cur + col, (size_t)(len - col));
        out[total - 1] = '\0';
        free(b->line[line]);
        b->line[line] = out;
        b->modified = true;
        free(tmp);
        return;
    }

    /* Multi-line insert: split at the first newline.
     * The original tail must be copied out BEFORE the line is freed -- it
     * lives inside b->line[line]. */
    int first_len = (int)(nl - tmp);

    size_t tail_len = (size_t)(len - col);
    char *tail = (char *)malloc(tail_len + 1);
    if (!tail) { free(tmp); return; }
    memcpy(tail, cur + col, tail_len);
    tail[tail_len] = '\0';

    size_t total = (size_t)col + (size_t)first_len + 1;
    char *out = (char *)malloc(total + 1);
    if (!out) { free(tail); free(tmp); return; }
    memcpy(out, cur, (size_t)col);
    memcpy(out + col, tmp, (size_t)first_len);
    out[col + first_len] = '\0';
    free(b->line[line]);
    b->line[line] = out;

    int at = line;
    /* Middle lines. */
    char *p = nl + 1;
    while (1) {
        char *nx = strchr(p, '\n');
        if (!nx) break;
        *nx = '\0';
        wcode_buf_insert_line(b, ++at, p);
        p = nx + 1;
    }
    /* Last line: remainder + the original tail. */
    size_t rem = strlen(p);
    char *tl = (char *)malloc(rem + tail_len + 2);
    if (!tl) { free(tail); free(tmp); return; }
    memcpy(tl, p, rem);
    memcpy(tl + rem, tail, tail_len);
    tl[rem + tail_len] = '\0';
    wcode_buf_insert_line(b, ++at, tl);
    free(tl);
    free(tail);

    b->modified = true;
    free(tmp);
}

void wcode_buf_delete_char(WCodeBuf *b, int line, int col) {
    if (!b || line < 0 || line >= b->n_lines) return;
    const char *cur = wcode_buf_get(b, line);
    int len = (int)strlen(cur);
    if (col <= 0 || col > len) return;
    char *out = (char *)malloc((size_t)len);
    if (!out) return;
    memcpy(out, cur, (size_t)(col - 1));
    memcpy(out + col - 1, cur + col, (size_t)(len - col));
    out[len - 1] = '\0';
    free(b->line[line]);
    b->line[line] = out;
    b->modified = true;
}

void wcode_buf_delete_forward(WCodeBuf *b, int line, int col) {
    if (!b || line < 0 || line >= b->n_lines) return;
    const char *cur = wcode_buf_get(b, line);
    int len = (int)strlen(cur);
    if (col < 0 || col >= len) return;
    char *out = (char *)malloc((size_t)len);
    if (!out) return;
    memcpy(out, cur, (size_t)col);
    memcpy(out + col, cur + col + 1, (size_t)(len - col - 1));
    out[len - 1] = '\0';
    free(b->line[line]);
    b->line[line] = out;
    b->modified = true;
}

void wcode_buf_split_line(WCodeBuf *b, int line, int col) {
    if (!b || line < 0 || line >= b->n_lines) return;
    const char *cur = wcode_buf_get(b, line);
    int len = (int)strlen(cur);
    if (col < 0) col = 0;
    if (col > len) col = len;
    char *tail = dup_line(cur + col);
    if (!tail) return;
    char *head = (char *)malloc((size_t)col + 1);
    if (!head) { free(tail); return; }
    memcpy(head, cur, (size_t)col);
    head[col] = '\0';
    free(b->line[line]);
    b->line[line] = head;
    wcode_buf_insert_line(b, line + 1, tail);
    free(tail);
}

void wcode_buf_join_lines(WCodeBuf *b, int line) {
    if (!b || line < 0 || line + 1 >= b->n_lines) return;
    const char *a = b->line[line];
    const char *c = b->line[line + 1];
    size_t la = strlen(a), lc = strlen(c);
    if (la + lc + 1 > WCODE_MAX_LINE) return;
    char *out = (char *)malloc(la + lc + 1);
    if (!out) return;
    memcpy(out, a, la);
    memcpy(out + la, c, lc);
    out[la + lc] = '\0';
    free(b->line[line]);
    b->line[line] = out;
    wcode_buf_delete_line(b, line + 1);
}

/* ---- Editor state -------------------------------------------------- */

void wcode_ed_init(WCodeEd *ed) {
    if (!ed) return;
    memset(ed, 0, sizeof(*ed));
    ed->rows = 24;
    ed->cols = 80;
    ed->sel_line = ed->sel_col = -1;
}

void wcode_ed_attach(WCodeEd *ed, WCodeBuf *b) {
    (void)b;
    wcode_ed_init(ed);
}

static void clamp_cursor(WCodeEd *ed, WCodeBuf *b) {
    if (ed->cy < 0) ed->cy = 0;
    if (ed->cy >= b->n_lines) ed->cy = b->n_lines - 1;
    int len = (int)strlen(wcode_buf_get(b, ed->cy));
    if (ed->cx < 0) ed->cx = 0;
    if (ed->cx > len) ed->cx = len;
}

void wcode_ed_goto(WCodeEd *ed, WCodeBuf *b, int line, int col) {
    if (!ed || !b) return;
    ed->cy = line;
    ed->cx = col;
    ed->sel_line = ed->sel_col = -1;
    clamp_cursor(ed, b);
}

void wcode_ed_insert_string(WCodeEd *ed, WCodeBuf *b, const char *s) {
    if (!ed || !b || !s) return;
    const char *p = s;
    while (*p) {
        if (*p == '\n') {
            wcode_buf_split_line(b, ed->cy, ed->cx);
            ed->cy++;
            ed->cx = 0;
            p++;
            continue;
        }
        if (*p == '\t') {
            int pad = WCODE_TABSTOP - (ed->cx % WCODE_TABSTOP);
            for (int i = 0; i < pad; i++)
                wcode_buf_insert_char(b, ed->cy, ed->cx++, ' ');
            p++;
            continue;
        }
        wcode_buf_insert_char(b, ed->cy, ed->cx, *p);
        ed->cx++;
        p++;
    }
    clamp_cursor(ed, b);
}

void wcode_ed_key(WCodeEd *ed, WCodeBuf *b, const char *key) {
    if (!ed || !b || !key) return;

    if (strcmp(key, "up") == 0) {
        if (ed->cy > 0) ed->cy--;
    } else if (strcmp(key, "down") == 0) {
        if (ed->cy + 1 < b->n_lines) ed->cy++;
    } else if (strcmp(key, "left") == 0) {
        if (ed->cx > 0) ed->cx--;
        else if (ed->cy > 0) { ed->cy--; ed->cx = (int)strlen(wcode_buf_get(b, ed->cy)); }
    } else if (strcmp(key, "right") == 0) {
        int len = (int)strlen(wcode_buf_get(b, ed->cy));
        if (ed->cx < len) ed->cx++;
        else if (ed->cy + 1 < b->n_lines) { ed->cy++; ed->cx = 0; }
    } else if (strcmp(key, "home") == 0) {
        ed->cx = 0;
    } else if (strcmp(key, "end") == 0) {
        ed->cx = (int)strlen(wcode_buf_get(b, ed->cy));
    } else if (strcmp(key, "pgup") == 0) {
        ed->cy -= ed->rows > 0 ? ed->rows : 1;
    } else if (strcmp(key, "pgdn") == 0) {
        ed->cy += ed->rows > 0 ? ed->rows : 1;
    } else if (strcmp(key, "enter") == 0) {
        wcode_buf_split_line(b, ed->cy, ed->cx);
        ed->cy++;
        ed->cx = 0;
    } else if (strcmp(key, "tab") == 0) {
        int pad = WCODE_TABSTOP - (ed->cx % WCODE_TABSTOP);
        for (int i = 0; i < pad; i++)
            wcode_buf_insert_char(b, ed->cy, ed->cx++, ' ');
    } else if (strcmp(key, "backspace") == 0) {
        if (ed->cx > 0) {
            wcode_buf_delete_char(b, ed->cy, ed->cx);
            ed->cx--;
        } else if (ed->cy > 0) {
            int prev_len = (int)strlen(wcode_buf_get(b, ed->cy - 1));
            wcode_buf_join_lines(b, ed->cy - 1);
            ed->cy--;
            ed->cx = prev_len;
        }
    } else if (strcmp(key, "delete") == 0) {
        int len = (int)strlen(wcode_buf_get(b, ed->cy));
        if (ed->cx < len) {
            wcode_buf_delete_forward(b, ed->cy, ed->cx);
        } else if (ed->cy + 1 < b->n_lines) {
            wcode_buf_join_lines(b, ed->cy);
        }
    } else if (strcmp(key, "ctrl-n") == 0) {
        wcode_buf_insert_line(b, b->n_lines, "");
    } else {
        return;   /* editing keys keep the selection; motion clears it */
    }

    clamp_cursor(ed, b);
    if (strcmp(key, "up") == 0 || strcmp(key, "down") == 0 ||
        strcmp(key, "left") == 0 || strcmp(key, "right") == 0 ||
        strcmp(key, "home") == 0 || strcmp(key, "end") == 0 ||
        strcmp(key, "pgup") == 0 || strcmp(key, "pgdn") == 0) {
        ed->sel_line = ed->sel_col = -1;
    }

    /* Keep the cursor in view. */
    if (ed->cy < ed->top) ed->top = ed->cy;
    if (ed->rows > 0 && ed->cy >= ed->top + ed->rows) ed->top = ed->cy - ed->rows + 1;
    if (ed->top < 0) ed->top = 0;
}

char *wcode_ed_selection(const WCodeEd *ed, WCodeBuf *b) {
    if (!ed || !b || ed->sel_line < 0) return NULL;
    int l0 = ed->sel_line, c0 = ed->sel_col;
    int l1 = ed->cy, c1 = ed->cx;
    if (l0 > l1 || (l0 == l1 && c0 > c1)) {
        int t = l0; l0 = l1; l1 = t;
        t = c0; c0 = c1; c1 = t;
    }
    size_t cap = 1;
    for (int l = l0; l <= l1; l++) cap += strlen(wcode_buf_get(b, l)) + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    for (int l = l0; l <= l1; l++) {
        const char *s = wcode_buf_get(b, l);
        int len = (int)strlen(s);
        int from = (l == l0) ? c0 : 0;
        int to = (l == l1) ? c1 : len;
        if (from < 0) from = 0;
        if (to > len) to = len;
        if (to > from) {
            memcpy(out + n, s + from, (size_t)(to - from));
            n += (size_t)(to - from);
        }
        if (l != l1) out[n++] = '\n';
    }
    out[n] = '\0';
    return out;
}
