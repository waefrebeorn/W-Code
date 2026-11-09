/*
 * wcode_build.c -- run a build command and turn its output into diagnostics.
 *
 * Output capture uses fork + execvp + a pipe so a build that hangs cannot
 * wedge the editor: wcode_run() is synchronous but the caller (the TUI) can
 * pass a timeout through the argv it builds.
 *
 * Diagnostics are parsed from GCC/Clang's "path:line:col: severity: message"
 * format, which both `gcc` and `holyd` (the HolyC compiler) emit.
 */

#include "wcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- Diagnostics --------------------------------------------------- */

void wcode_diags_init(WCodeDiags *dg) {
    if (!dg) return;
    memset(dg, 0, sizeof(*dg));
}

void wcode_diags_free(WCodeDiags *dg) {
    if (!dg) return;
    free(dg->d);
    dg->d = NULL;
    dg->n = dg->cap = 0;
    dg->errors = dg->warnings = 0;
}

void wcode_diags_add(WCodeDiags *dg, const char *path, int line, int col,
                     const char *sev, const char *msg) {
    if (!dg || !msg) return;
    if (dg->n == dg->cap) {
        int nc = dg->cap ? dg->cap * 2 : 16;
        WCodeDiag *nd = (WCodeDiag *)realloc(dg->d, (size_t)nc * sizeof(WCodeDiag));
        if (!nd) return;
        dg->d = nd;
        dg->cap = nc;
    }
    WCodeDiag *d = &dg->d[dg->n++];
    memset(d, 0, sizeof(*d));
    if (path) snprintf(d->path, sizeof(d->path), "%s", path);
    d->line = line;
    d->col = col;
    snprintf(d->severity, sizeof(d->severity), "%s", sev ? sev : "note");
    snprintf(d->message, sizeof(d->message), "%s", msg);
    if (strcmp(d->severity, "error") == 0) dg->errors++;
    else if (strcmp(d->severity, "warning") == 0) dg->warnings++;
}

/* Parse one "path:line:col: severity: message" line. */
static void parse_one(WCodeDiags *dg, const char *s) {
    /* Skip leading whitespace. */
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0' || *s == '\n') return;

    /* GCC prefixes include-trace lines: "In file included from X:1:2," and
     * "from X:1:2:".  The path follows the prefix, not the line start. */
    if (strncmp(s, "In file included from ", 21) == 0) s += 21;
    else if (strncmp(s, "from ", 5) == 0) s += 5;

    /* Path up to the first ':' that is followed by digits. */
    const char *p = s;
    const char *path_end = NULL;
    int line = 0, col = 0;
    const char *q = s;
    while (*q) {
        if (*q == ':') {
            /* Try to read line[:col] after it. */
            const char *r = q + 1;
            if (*r >= '0' && *r <= '9') {
                line = atoi(r);
                while (*r >= '0' && *r <= '9') r++;
                col = 0;
                if (*r == ':') {
                    r++;
                    if (*r >= '0' && *r <= '9') {
                        col = atoi(r);
                        while (*r >= '0' && *r <= '9') r++;
                    }
                }
                path_end = q;
                q = r;
                break;
            }
        }
        q++;
    }
    if (!path_end) return;

    /* Expect ": severity: message". */
    while (*q == ' ' || *q == '\t') q++;
    if (*q == ':') q++;
    while (*q == ' ' || *q == '\t') q++;

    char sev[16] = "note";
    if (strncmp(q, "error", 5) == 0) { snprintf(sev, sizeof(sev), "error"); q += 5; }
    else if (strncmp(q, "warning", 7) == 0) { snprintf(sev, sizeof(sev), "warning"); q += 7; }
    else if (strncmp(q, "note", 4) == 0) { snprintf(sev, sizeof(sev), "note"); q += 4; }

    while (*q == ' ' || *q == '\t' || *q == ':') q++;

    char path[WCODE_MAX_PATH];
    size_t plen = (size_t)(path_end - p);
    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
    memcpy(path, p, plen);
    path[plen] = '\0';
    /* Trim trailing spaces from the path. */
    while (plen > 0 && (path[plen - 1] == ' ' || path[plen - 1] == '\t'))
        path[--plen] = '\0';

    /* A real compiler path never contains a space, a '*' or the shell
     * preamble words -- reject "make: *** [Makefile:5: all] Error 1". */
    if (strchr(path, ' ') || strchr(path, '*') || strcmp(path, "make") == 0)
        return;

    wcode_diags_add(dg, path, line, col, sev, q);
}

void wcode_diags_parse_gcc(WCodeDiags *dg, const char *out) {
    if (!dg || !out) return;
    const char *p = out;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[2048];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        parse_one(dg, line);
        if (!nl) break;
        p = nl + 1;
    }
}

/* ---- Run ----------------------------------------------------------- */

int wcode_run(const char *cwd, char *const argv[], char **out, size_t *out_len) {
    if (!argv || !argv[0]) return -1;

    int fds[2];
    if (pipe(fds) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: stdout+stderr -> pipe. */
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        if (cwd && chdir(cwd) != 0) { /* keep going; the shell reports it */ }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(fds[1]);
    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fds[0]); return -1; }

    for (;;) {
        if (len + 4096 > cap) {
            size_t nc = cap * 2;
            char *nb = (char *)realloc(buf, nc);
            if (!nb) break;
            buf = nb; cap = nc;
        }
        ssize_t r = read(fds[0], buf + len, cap - len - 1);
        if (r <= 0) break;
        len += (size_t)r;
    }
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    buf[len] = '\0';
    if (out) *out = buf; else free(buf);
    if (out_len) *out_len = len;

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}
