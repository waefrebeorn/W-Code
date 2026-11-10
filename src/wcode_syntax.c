/*
 * wcode_syntax.c -- token classification for C11 and HolyC.
 *
 * A deliberately small lexer: it walks the line once, splits it into tokens
 * and classifies each.  It is not a parser -- it only needs to pick the right
 * colour for a run of text, so a lightweight scanner is enough and a real
 * parser would be wasted work.
 */

#include "wcode.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>

/* C11 keywords + common library types. */
static const char *g_c_keywords[] = {
    "auto","break","case","const","continue","default","do","else","enum",
    "extern","for","goto","if","inline","register","restrict","return","sizeof",
    "static","struct","switch","typedef","union","volatile","while","_Alignas",
    "_Alignof","_Atomic","_Bool","_Complex","_Generic","_Imaginary","_Noreturn",
    "_Static_assert","_Thread_local",
    /* Types that read as keywords in practice. */
    "int","long","short","char","float","double","void","unsigned","signed",
    "size_t","ssize_t","bool","true","false","NULL",
    /* WuBuOS/HolyC surface. */
    "HolyC","U8","U16","U32","U64","I8","I16","I32","I64","Bool",
    NULL
};

/* Lua keywords -- the .WET / Wua surface, which is what a .wet author types. */
static const char *g_lua_keywords[] = {
    "and","break","do","else","elseif","end","false","for","function","goto",
    "if","in","local","nil","not","or","repeat","return","then","true","until",
    "while",
    /* The .WET element names and the calls a .wet script makes constantly. */
    "wet","self","applet","stack","dock","canvas","grid","label","button",
    "textbox","checkbox","listbox","progress","slider","groupbox","image",
    "separator","find","messagebox","create","attach","add","checked","clicks",
    NULL
};

static bool in_list(const char *s, size_t n, const char **list) {
    for (int i = 0; list[i]; i++) {
        if (strlen(list[i]) == n && strncmp(list[i], s, n) == 0)
            return true;
    }
    return false;
}

const char *wcode_lang_for_path(const char *path) {
    if (!path) return "c";
    const char *dot = strrchr(path, '.');
    if (!dot) return "c";
    dot++;
    if (strcasecmp(dot, "c") == 0 || strcasecmp(dot, "h") == 0 ||
        strcasecmp(dot, "cpp") == 0 || strcasecmp(dot, "hpp") == 0 ||
        strcasecmp(dot, "cc") == 0 || strcasecmp(dot, "cxx") == 0)
        return "c";
    if (strcasecmp(dot, "hc") == 0 || strcasecmp(dot, "holyd") == 0 ||
        strcasecmp(dot, "temple") == 0)
        return "holyc";
    /* .WET applets, VWS forms and Wua scripts are Lua.  A .vws file IS a .wet
     * file, so both extensions get the same colouring. */
    if (strcasecmp(dot, "wet") == 0 || strcasecmp(dot, "vws") == 0 ||
        strcasecmp(dot, "lua") == 0)
        return "lua";
    if (strcasecmp(dot, "md") == 0 || strcasecmp(dot, "txt") == 0)
        return "text";
    return "c";
}

WCodeTokKind wcode_tok_classify(const char *s, size_t n, const char *lang,
                                int prev_kind) {
    (void)prev_kind;
    if (n == 0 || !s) return WCODE_TOK_PLAIN;

    /* Comment bodies. */
    if (lang && strcmp(lang, "text") == 0) return WCODE_TOK_PLAIN;

    if (s[0] == '/' && n >= 2 && s[1] == '/') return WCODE_TOK_COMMENT;
    if (s[0] == '/' && n >= 2 && s[1] == '*') return WCODE_TOK_COMMENT;
    if (lang && strcmp(lang, "lua") == 0 && s[0] == '-' && n >= 2 && s[1] == '-')
        return WCODE_TOK_COMMENT;   /* Lua's line comment */
    if (s[0] == '#' && !(lang && strcmp(lang, "lua") == 0))
        return WCODE_TOK_PREPROC;   /* Lua has no preprocessor */
    if (s[0] == '"' || s[0] == '\'') return WCODE_TOK_STRING;

    if (isdigit((unsigned char)s[0])) return WCODE_TOK_NUMBER;

    /* Identifier. */
    if (isalpha((unsigned char)s[0]) || s[0] == '_') {
        if (lang && strcmp(lang, "lua") == 0)
            return in_list(s, n, g_lua_keywords) ? WCODE_TOK_KEYWORD
                                                 : WCODE_TOK_PLAIN;
        return in_list(s, n, g_c_keywords) ? WCODE_TOK_KEYWORD
                                           : WCODE_TOK_PLAIN;
    }
    return WCODE_TOK_PLAIN;
}

/* -- Line scanning ----------------------------------------------------
 * Split one line into runs of (column, length, kind).  Used by the TUI to
 * colourise without re-tokenising on every repaint. */

int wcode_syntax_line(const char *line, const char *lang, WCodeRun *runs,
                      int max_runs) {
    int n = 0;
    int i = 0;
    int len = (int)strlen(line);

    /* Whole-line comment. */
    if (len >= 2 && line[0] == '/' && (line[1] == '/' || line[1] == '*')) {
        runs[0].col = 0;
        runs[0].len = len;
        runs[0].kind = WCODE_TOK_COMMENT;
        return 1;
    }
    if (lang && strcmp(lang, "lua") == 0 && len >= 2 && line[0] == '-' && line[1] == '-') {
        runs[0].col = 0;
        runs[0].len = len;
        runs[0].kind = WCODE_TOK_COMMENT;
        return 1;
    }

    while (i < len && n < max_runs) {
        if (isspace((unsigned char)line[i])) {
            int s = i;
            while (i < len && isspace((unsigned char)line[i])) i++;
            runs[n].col = s;
            runs[n].len = i - s;
            runs[n].kind = WCODE_TOK_PLAIN;
            n++;
            continue;
        }
        if (line[i] == '/' && i + 1 < len && (line[i+1] == '/' || line[i+1] == '*')) {
            runs[n].col = i;
            runs[n].len = len - i;
            runs[n].kind = WCODE_TOK_COMMENT;
            n++;
            break;
        }
        /* Lua's line comment. */
        if (lang && strcmp(lang, "lua") == 0 &&
            line[i] == '-' && i + 1 < len && line[i+1] == '-') {
            runs[n].col = i;
            runs[n].len = len - i;
            runs[n].kind = WCODE_TOK_COMMENT;
            n++;
            break;
        }
        if (line[i] == '"') {
            int s = i++;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (line[i++] == '"') break;
            }
            runs[n].col = s;
            runs[n].len = i - s;
            runs[n].kind = WCODE_TOK_STRING;
            n++;
            continue;
        }
        if (line[i] == '\'') {
            int s = i++;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (line[i++] == '\'') break;
            }
            runs[n].col = s;
            runs[n].len = i - s;
            runs[n].kind = WCODE_TOK_STRING;
            n++;
            continue;
        }
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            int s = i;
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            WCodeTokKind k = wcode_tok_classify(line + s, (size_t)(i - s), lang, 0);
            runs[n].col = s;
            runs[n].len = i - s;
            runs[n].kind = k;
            n++;
            continue;
        }
        if (isdigit((unsigned char)line[i])) {
            int s = i;
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '.')) i++;
            runs[n].col = s;
            runs[n].len = i - s;
            runs[n].kind = WCODE_TOK_NUMBER;
            n++;
            continue;
        }
        runs[n].col = i;
        runs[n].len = 1;
        runs[n].kind = WCODE_TOK_PLAIN;
        n++;
        i++;
    }
    return n;
}
