/*
 * main.c -- W-CODE entry point.
 *
 *   wcode                 open the TUI in the current directory
 *   wcode <path>          open the TUI with <path> loaded
 *   wcode --agent         read newline-delimited JSON commands from stdin and
 *                         write one JSON response per line to stdout
 *   wcode --agent <file>  same, but read the commands from a file
 *   wcode --version
 *   wcode --help
 *
 * The agent mode is how an AI drives the same editor a human sees.
 */

#include "wcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void wcode_init(const char *root);
void wcode_shutdown(void);
int  wcode_tui_run(void);
void wcode_tui_stop(void);
void wcode_tui_init_editor(void);
WCodeBuf *wcode_buffer(void);

static void usage(void) {
    printf("W-CODE " WCODE_VERSION " -- the WuBuOS agentic IDE\n\n"
           "usage:\n"
           "  wcode [path]        open the editor (path optional)\n"
           "  wcode --agent       agent mode: JSON lines on stdin/stdout\n"
           "  wcode --agent FILE  agent mode reading commands from FILE\n"
           "  wcode --version\n"
           "  wcode --help\n\n"
           "system code: C11 and HolyC.  agent ops: status list read open\n"
           "write save edit keys build diags help\n");
}

static int run_agent(FILE *in, FILE *out) {
    char line[16384];
    char resp[70000];
    int n = 0;
    while (fgets(line, sizeof(line), in)) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (l == 0) continue;
        wcode_agent_handle(line, resp, sizeof(resp));
        fprintf(out, "%s\n", resp);
        fflush(out);
        n++;
    }
    return n;
}

int main(int argc, char **argv) {
    const char *root = ".";
    const char *start_path = NULL;
    int agent = 0;
    const char *agent_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--agent") == 0) {
            agent = 1;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                agent_file = argv[++i];
            }
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("W-CODE " WCODE_VERSION "\n");
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "wcode: unknown option %s\n", argv[i]);
            usage();
            return 2;
        } else {
            start_path = argv[i];
        }
    }

    wcode_init(root);
    wcode_tui_init_editor();

    if (start_path) {
        if (wcode_buf_load(wcode_buffer(), start_path) != 0)
            fprintf(stderr, "wcode: cannot open %s\n", start_path);
    }

    if (agent) {
        FILE *in = stdin;
        if (agent_file) {
            in = fopen(agent_file, "r");
            if (!in) {
                fprintf(stderr, "wcode: cannot open agent script %s\n", agent_file);
                wcode_shutdown();
                return 2;
            }
        }
        int n = run_agent(in, stdout);
        if (in != stdin) fclose(in);
        wcode_shutdown();
        return n > 0 ? 0 : 1;
    }

    wcode_tui_run();
    wcode_tui_stop();
    wcode_shutdown();
    return 0;
}
