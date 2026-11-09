/*
 * wcode_fs.c -- file tree.
 *
 * A lazy tree: the root's immediate children are read eagerly, and a
 * directory's children are only read when it is expanded.  That keeps
 * opening a large tree cheap and means the TUI never blocks on a deep
 * listing the user has not asked for.
 */

#include "wcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

static WCodeNode *node_new(const char *root, const char *name, int depth) {
    WCodeNode *n = (WCodeNode *)calloc(1, sizeof(WCodeNode));
    if (!n) return NULL;
    snprintf(n->name, sizeof(n->name), "%s", name);
    if (strcmp(root, ".") == 0 && depth == 0)
        snprintf(n->path, sizeof(n->path), "%s", name);
    else
        snprintf(n->path, sizeof(n->path), "%s/%s", root, name);
    n->depth = depth;
    return n;
}

static int cmp_name(const void *a, const void *b) {
    /* Directories first, then case-insensitive name. */
    const WCodeNode *x = *(const WCodeNode *const *)a;
    const WCodeNode *y = *(const WCodeNode *const *)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

/* Populate the immediate children of `dir`.  Returns count. */
static int read_children(WCodeNode *dir) {
    if (!dir || !dir->is_dir || dir->child) return 0;

    DIR *d = opendir(dir->path);
    if (!d) return -1;

    WCodeNode **tmp = NULL;
    int n = 0, cap = 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        /* Skip editor/OS droppings. */
        size_t l = strlen(e->d_name);
        if (l && e->d_name[0] == '.') continue;
        if (strcmp(e->d_name, "node_modules") == 0) continue;
        if (l > 1 && e->d_name[l - 1] == '~') continue;

        char full[WCODE_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir->path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (n == cap) {
            int nc = cap ? cap * 2 : 16;
            WCodeNode **nt = (WCodeNode **)realloc(tmp, (size_t)nc * sizeof(WCodeNode *));
            if (!nt) break;
            tmp = nt; cap = nc;
        }
        WCodeNode *c = node_new(dir->path, e->d_name, dir->depth + 1);
        if (!c) break;
        c->is_dir = S_ISDIR(st.st_mode);
        tmp[n++] = c;
    }
    closedir(d);

    if (n > 1) qsort(tmp, (size_t)n, sizeof(WCodeNode *), cmp_name);

    /* Link into a chain. */
    WCodeNode head = {0};
    WCodeNode *tail = &head;
    for (int i = 0; i < n; i++) {
        tail->next = tmp[i];
        tail = tmp[i];
    }
    dir->child = head.next;
    free(tmp);
    return n;
}

WCodeNode *wcode_tree_load(const char *root) {
    WCodeNode *r = (WCodeNode *)calloc(1, sizeof(WCodeNode));
    if (!r) return NULL;
    snprintf(r->name, sizeof(r->name), "%s", root);
    snprintf(r->path, sizeof(r->path), "%s", root);
    r->is_dir = true;
    r->expanded = true;
    r->depth = -1;
    read_children(r);
    return r;
}

void wcode_tree_free(WCodeNode *n) {
    if (!n) return;
    wcode_tree_free(n->child);
    wcode_tree_free(n->next);
    free(n);
}

WCodeNode *wcode_tree_find(WCodeNode *root, const char *path) {
    if (!root || !path) return NULL;
    for (WCodeNode *n = root; n; n = n->next) {
        if (strcmp(n->path, path) == 0) return n;
        if (n->is_dir && n->child) {
            WCodeNode *r = wcode_tree_find(n->child, path);
            if (r) return r;
        }
    }
    return NULL;
}

WCodeNode *wcode_tree_toggle(WCodeNode *root, const char *path) {
    WCodeNode *n = wcode_tree_find(root, path);
    if (!n || !n->is_dir) return NULL;
    if (!n->child && !n->expanded) read_children(n);
    n->expanded = !n->expanded;
    return n;
}

/* Count every node reachable with directories expanded as marked. */
static int count_rec(const WCodeNode *n) {
    int c = 0;
    for (; n; n = n->next) {
        if (n->depth < 0) { c += count_rec(n->child); continue; }
        c++;
        if (n->is_dir && n->expanded) c += count_rec(n->child);
    }
    return c;
}

int wcode_tree_count(WCodeNode *root) {
    return root ? count_rec(root) : 0;
}

/* Public wrapper so the agent module can read a directory without building a
 * full tree node itself. */
int wcode_fs_read_children_pub(WCodeNode *dir) {
    return read_children(dir);
}
