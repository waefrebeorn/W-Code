/*
 * wcode_agent.c -- the machine-readable half of W-CODE.
 *
 * One JSON object per line in, one JSON object per line out.  An agent drives
 * exactly the same buffer, tree, build and diagnostic modules the human TUI
 * uses, so there is no second code path to drift.
 *
 * Supported ops:
 *   status  -- version, root, open path, modified, line/col, counts
 *   list    -- {path?} directory entries
 *   read    -- {path?} file contents (defaults to the open buffer)
 *   open    -- {path} load into the buffer
 *   write   -- {path, text} write a file from scratch
 *   save    -- {path?} save the buffer
 *   edit    -- {line, col, insert?, delete?} buffer edit at a position
 *   keys    -- {keys:[...]} replay logical keys through the editor
 *   build   -- {cwd, argv:[...]} run a build, return output + diagnostics
 *   diags   -- last build's diagnostics
 *   help    -- op list
 */

#include "wcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Shared IDE state ---------------------------------------------- */

typedef struct {
    WCodeBuf   buf;
    WCodeEd    ed;
    WCodeNode *tree;
    char       root[WCODE_MAX_PATH];
    WCodeDiags diags;
    char      *last_output;
    size_t     last_output_len;
    int        last_status;
    bool       has_build;
} WCode;

static WCode g_wc;

void wcode_init(const char *root) {
    memset(&g_wc, 0, sizeof(g_wc));
    wcode_ed_init(&g_wc.ed);
    snprintf(g_wc.root, sizeof(g_wc.root), "%s", root ? root : ".");
    g_wc.tree = wcode_tree_load(g_wc.root);
    wcode_diags_init(&g_wc.diags);
    wcode_buf_clear(&g_wc.buf);
}

void wcode_shutdown(void) {
    wcode_tree_free(g_wc.tree);
    wcode_diags_free(&g_wc.diags);
    free(g_wc.last_output);
    for (int i = 0; i < g_wc.buf.n_lines; i++) free(g_wc.buf.line[i]);
    free(g_wc.buf.line);
    memset(&g_wc, 0, sizeof(g_wc));
}

WCodeBuf *wcode_buffer(void) { return &g_wc.buf; }
WCodeNode *wcode_tree(void)  { return g_wc.tree; }

/* ---- JSON helpers -------------------------------------------------- */

int wcode_json_escape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = in; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        const char *rep = NULL;
        char tmp[8];
        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            case '\b': rep = "\\b";  break;
            case '\f': rep = "\\f";  break;
            default:
                if (c < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    rep = tmp;
                }
                break;
        }
        if (rep) {
            size_t l = strlen(rep);
            if (n + l + 1 > cap) return -1;
            memcpy(out + n, rep, l);
            n += l;
        } else {
            if (n + 2 > cap) return -1;
            out[n++] = (char)c;
        }
    }
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    return (int)n;
}

/* Extract a string field.  Handles the simple {"a": "b"} subset we emit. */
char *wcode_json_str(const char *json, const char *field) {
    if (!json || !field) return NULL;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    size_t cap = 256, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                case '/': c = '/';  break;
                default: c = e; break;
            }
        }
        if (n + 2 > cap) {
            size_t nc = cap * 2;
            char *nb = (char *)realloc(out, nc);
            if (!nb) { free(out); return NULL; }
            out = nb; cap = nc;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return out;
}

/* Extract an integer field. */
static int json_int(const char *json, const char *field, int def) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    const char *p = strstr(json, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return def;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

/* Extract an array-of-strings field into a malloc'd NULL-terminated array. */
static char **json_str_array(const char *json, const char *field, int *count) {
    *count = 0;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p && *p != '[') {
        if (*p == '}') return NULL;
        p++;
    }
    if (*p != '[') return NULL;
    p++;
    int cap = 8, n = 0;
    char **arr = (char **)calloc((size_t)cap, sizeof(char *));
    if (!arr) return NULL;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == '"') {
            char *s = wcode_json_str(p, "");   /* reuse the escaper on this string */
            /* wcode_json_str looks up a named field, so do it by hand here. */
            free(s);
            p++;
            size_t sc = 128, sn = 0;
            char *sv = (char *)malloc(sc);
            if (!sv) break;
            while (*p && *p != '"') {
                char c = *p++;
                if (c == '\\' && *p) {
                    char e = *p++;
                    switch (e) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case '"': c = '"';  break;
                        case '\\': c = '\\'; break;
                        case '/': c = '/';  break;
                        default: c = e; break;
                    }
                }
                if (sn + 2 > sc) {
                    size_t nc = sc * 2;
                    char *nb = (char *)realloc(sv, nc);
                    if (!nb) { free(sv); sv = NULL; break; }
                    sv = nb; sc = nc;
                }
                if (sv) sv[sn++] = c;
            }
            if (*p == '"') p++;
            if (sv) {
                sv[sn] = '\0';
                if (n == cap) {
                    int nc = cap * 2;
                    char **na = (char **)realloc(arr, (size_t)nc * sizeof(char *));
                    if (!na) { free(sv); break; }
                    arr = na; cap = nc;
                }
                arr[n++] = sv;
            }
            continue;
        }
        p++;
    }
    *count = n;
    return arr;
}

static void free_str_array(char **arr, int n) {
    if (!arr) return;
    for (int i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

/* ---- Emitters ------------------------------------------------------ */

static void emit_obj(char *out, size_t cap, const char *status, const char *extra) {
    /* `extra` is the comma-led fragment, e.g. ",\"a\":1". */
    snprintf(out, cap, "{\"status\":\"%s\"%s}", status,
             (extra && extra[0]) ? extra : "");
}

static void op_status(char *out, size_t cap, const char *req) {
    (void)req;
    char extra[1024];
    char esc[WCODE_MAX_PATH * 2];
    wcode_json_escape(g_wc.buf.path[0] ? g_wc.buf.path : "", esc, sizeof(esc));
    snprintf(extra, sizeof(extra),
             ",\"version\":\"%s\",\"root\":\"%s\",\"path\":\"%s\","
             "\"modified\":%s,\"line\":%d,\"col\":%d,\"lines\":%d,"
             "\"errors\":%d,\"warnings\":%d,\"tree_nodes\":%d",
             WCODE_VERSION, g_wc.root, esc,
             g_wc.buf.modified ? "true" : "false",
             g_wc.ed.cy + 1, g_wc.ed.cx + 1, g_wc.buf.n_lines,
             g_wc.diags.errors, g_wc.diags.warnings,
             wcode_tree_count(g_wc.tree));
    emit_obj(out, cap, "ok", extra);
}

static void op_list(char *out, size_t cap, const char *req) {
    char *path = wcode_json_str(req, "path");
    char target[WCODE_MAX_PATH];
    if (path && path[0]) {
        snprintf(target, sizeof(target), "%s", path);
    } else {
        snprintf(target, sizeof(target), "%s", g_wc.root);
    }
    free(path);

    WCodeNode tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.path, sizeof(tmp.path), "%s", target);
    tmp.is_dir = true;
    tmp.expanded = true;

    /* Use the tree module's reader via a temporary node. */
    extern int wcode_fs_read_children_pub(WCodeNode *dir);
    wcode_fs_read_children_pub(&tmp);

    char body[8192];
    size_t n = 0;
    n += (size_t)snprintf(body + n, sizeof(body) - n, ",\"entries\":[");
    int first = 1;
    for (WCodeNode *c = tmp.child; c && n < sizeof(body) - 256; c = c->next) {
        char esc[WCODE_MAX_PATH * 2];
        wcode_json_escape(c->name, esc, sizeof(esc));
        n += (size_t)snprintf(body + n, sizeof(body) - n,
                              "%s{\"name\":\"%s\",\"dir\":%s}",
                              first ? "" : ",", esc, c->is_dir ? "true" : "false");
        first = 0;
    }
    n += (size_t)snprintf(body + n, sizeof(body) - n, "]");
    body[n] = '\0';
    wcode_tree_free(tmp.child);

    emit_obj(out, cap, "ok", body);
}

static void op_read(char *out, size_t cap, const char *req) {
    char *path = wcode_json_str(req, "path");
    char esc[65536];
    if (path && path[0]) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            char e[512];
            snprintf(e, sizeof(e), ",\"error\":\"cannot open\"");
            emit_obj(out, cap, "error", e);
            free(path);
            return;
        }
        size_t n = fread(esc, 1, sizeof(esc) - 1, f);
        fclose(f);
        esc[n] = '\0';
        char *p = esc;
        char body[70000];
        size_t bn = 0;
        bn += (size_t)snprintf(body + bn, sizeof(body) - bn, ",\"path\":\"%s\",\"text\":\"", path);
        bn += (size_t)wcode_json_escape(p, body + bn, sizeof(body) - bn - 4);
        snprintf(body + bn, sizeof(body) - bn, "\"");
        emit_obj(out, cap, "ok", body);
        free(path);
        return;
    }
    free(path);
    /* Buffer contents. */
    char body[70000];
    size_t bn = 0;
    bn += (size_t)snprintf(body + bn, sizeof(body) - bn, ",\"text\":\"");
    for (int i = 0; i < g_wc.buf.n_lines && bn < sizeof(body) - 8; i++) {
        bn += (size_t)wcode_json_escape(g_wc.buf.line[i], body + bn,
                                        sizeof(body) - bn - 4);
        if (i + 1 < g_wc.buf.n_lines && bn < sizeof(body) - 8)
            body[bn++] = '\\', body[bn++] = 'n';
    }
    snprintf(body + bn, sizeof(body) - bn, "\"");
    emit_obj(out, cap, "ok", body);
}

static void op_open(char *out, size_t cap, const char *req) {
    char *path = wcode_json_str(req, "path");
    if (!path || !path[0]) {
        emit_obj(out, cap, "error", ",\"error\":\"path required\"");
        free(path);
        return;
    }
    int rc = wcode_buf_load(&g_wc.buf, path);
    free(path);
    if (rc != 0) {
        emit_obj(out, cap, "error", ",\"error\":\"load failed\"");
        return;
    }
    wcode_ed_init(&g_wc.ed);
    char extra[512];
    snprintf(extra, sizeof(extra), ",\"lines\":%d", g_wc.buf.n_lines);
    emit_obj(out, cap, "ok", extra);
}

static void op_write(char *out, size_t cap, const char *req) {
    char *path = wcode_json_str(req, "path");
    char *text = wcode_json_str(req, "text");
    if (!path || !text) {
        emit_obj(out, cap, "error", ",\"error\":\"path and text required\"");
        free(path); free(text);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        emit_obj(out, cap, "error", ",\"error\":\"cannot write\"");
        free(path); free(text);
        return;
    }
    fputs(text, f);
    fclose(f);
    free(path); free(text);
    emit_obj(out, cap, "ok", "");
}

static void op_save(char *out, size_t cap, const char *req) {
    char *path = wcode_json_str(req, "path");
    int rc;
    if (path && path[0]) rc = wcode_buf_save(&g_wc.buf, path);
    else               rc = wcode_buf_save_self(&g_wc.buf);
    free(path);
    if (rc != 0) {
        emit_obj(out, cap, "error", ",\"error\":\"save failed\"");
        return;
    }
    emit_obj(out, cap, "ok", "");
}

static void op_edit(char *out, size_t cap, const char *req) {
    int line = json_int(req, "line", g_wc.ed.cy + 1);
    int col  = json_int(req, "col",  g_wc.ed.cx + 1);
    char *insert = wcode_json_str(req, "insert");
    char *remove = wcode_json_str(req, "remove");

    wcode_ed_goto(&g_wc.ed, &g_wc.buf, line - 1, col - 1);

    if (remove && remove[0]) {
        /* Remove N chars forward. */
        int n = atoi(remove);
        if (remove[0] >= '0' && remove[0] <= '9') {
            for (int i = 0; i < n; i++)
                wcode_buf_delete_forward(&g_wc.buf, g_wc.ed.cy, g_wc.ed.cx);
        } else {
            wcode_ed_insert_string(&g_wc.ed, &g_wc.buf, remove);
        }
    }
    if (insert && insert[0])
        wcode_ed_insert_string(&g_wc.ed, &g_wc.buf, insert);

    free(insert);
    free(remove);

    char extra[256];
    snprintf(extra, sizeof(extra), ",\"line\":%d,\"col\":%d",
             g_wc.ed.cy + 1, g_wc.ed.cx + 1);
    emit_obj(out, cap, "ok", extra);
}

static void op_keys(char *out, size_t cap, const char *req) {
    int n = 0;
    char **keys = json_str_array(req, "keys", &n);
    if (!keys) {
        emit_obj(out, cap, "error", ",\"error\":\"keys required\"");
        return;
    }
    for (int i = 0; i < n; i++)
        wcode_ed_key(&g_wc.ed, &g_wc.buf, keys[i]);
    free_str_array(keys, n);
    char extra[256];
    snprintf(extra, sizeof(extra), ",\"line\":%d,\"col\":%d",
             g_wc.ed.cy + 1, g_wc.ed.cx + 1);
    emit_obj(out, cap, "ok", extra);
}

static void op_build(char *out, size_t cap, const char *req) {
    int n = 0;
    char **argv = json_str_array(req, "argv", &n);
    if (!argv || n == 0) {
        free_str_array(argv, n);
        emit_obj(out, cap, "error", ",\"error\":\"argv required\"");
        return;
    }
    char *cwd = wcode_json_str(req, "cwd");

    free(g_wc.last_output);
    g_wc.last_output = NULL;
    g_wc.last_output_len = 0;
    wcode_diags_free(&g_wc.diags);
    wcode_diags_init(&g_wc.diags);

    int status = wcode_run((cwd && cwd[0]) ? cwd : g_wc.root,
                           argv, &g_wc.last_output, &g_wc.last_output_len);
    g_wc.last_status = status;
    g_wc.has_build = true;

    if (g_wc.last_output)
        wcode_diags_parse_gcc(&g_wc.diags, g_wc.last_output);

    free_str_array(argv, n);
    free(cwd);

    char extra[512];
    snprintf(extra, sizeof(extra),
             ",\"exit\":%d,\"errors\":%d,\"warnings\":%d,\"output_len\":%zu",
             status, g_wc.diags.errors, g_wc.diags.warnings,
             g_wc.last_output_len);
    emit_obj(out, cap, "ok", extra);
}

static void op_diags(char *out, size_t cap, const char *req) {
    (void)req;
    char body[16384];
    size_t n = 0;
    n += (size_t)snprintf(body + n, sizeof(body) - n,
                          ",\"errors\":%d,\"warnings\":%d,\"diags\":[",
                          g_wc.diags.errors, g_wc.diags.warnings);
    for (int i = 0; i < g_wc.diags.n && n < sizeof(body) - 512; i++) {
        WCodeDiag *d = &g_wc.diags.d[i];
        char ep[WCODE_MAX_PATH * 2], em[2048];
        wcode_json_escape(d->path, ep, sizeof(ep));
        wcode_json_escape(d->message, em, sizeof(em));
        n += (size_t)snprintf(body + n, sizeof(body) - n,
                              "%s{\"path\":\"%s\",\"line\":%d,\"col\":%d,"
                              "\"severity\":\"%s\",\"message\":\"%s\"}",
                              i ? "," : "", ep, d->line, d->col,
                              d->severity, em);
    }
    snprintf(body + n, sizeof(body) - n, "]");
    emit_obj(out, cap, "ok", body);
}

static void op_help(char *out, size_t cap) {
    emit_obj(out, cap, "ok",
             ",\"ops\":[\"status\",\"list\",\"read\",\"open\",\"write\","
             "\"save\",\"edit\",\"keys\",\"build\",\"diags\",\"help\"]");
}

/* ---- Dispatch ------------------------------------------------------ */

void wcode_agent_handle(const char *req, char *resp, size_t resp_cap) {
    if (!req || !resp || resp_cap == 0) return;
    resp[0] = '\0';
    if (resp_cap < 64) return;

    char *op = wcode_json_str(req, "op");
    if (!op) {
        emit_obj(resp, resp_cap, "error", ",\"error\":\"op required\"");
        return;
    }
    if (strcmp(op, "status") == 0) op_status(resp, resp_cap, req);
    else if (strcmp(op, "list")  == 0) op_list(resp, resp_cap, req);
    else if (strcmp(op, "read")  == 0) op_read(resp, resp_cap, req);
    else if (strcmp(op, "open")  == 0) op_open(resp, resp_cap, req);
    else if (strcmp(op, "write") == 0) op_write(resp, resp_cap, req);
    else if (strcmp(op, "save")  == 0) op_save(resp, resp_cap, req);
    else if (strcmp(op, "edit")  == 0) op_edit(resp, resp_cap, req);
    else if (strcmp(op, "keys")  == 0) op_keys(resp, resp_cap, req);
    else if (strcmp(op, "build") == 0) op_build(resp, resp_cap, req);
    else if (strcmp(op, "diags") == 0) op_diags(resp, resp_cap, req);
    else if (strcmp(op, "help")  == 0) op_help(resp, resp_cap);
    else {
        char e[256];
        snprintf(e, sizeof(e), ",\"error\":\"unknown op\"}");
        emit_obj(resp, resp_cap, "error", e);
        resp[strlen(resp) - 1] = '\0';   /* drop the trailing brace */
        strcat(resp, ",\"op\":\"");
        strncat(resp, op, 32);
        strcat(resp, "\"}");
    }
    free(op);
}
