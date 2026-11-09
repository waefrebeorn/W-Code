/*
 * wcode_tui.h -- the human frontend's interface.
 */

#ifndef WCODE_TUI_H
#define WCODE_TUI_H

#define WCODE_UI_LINE_MAX 4096

int  wcode_tui_run(void);
void wcode_tui_stop(void);
void wcode_tui_init_editor(void);

#endif /* WCODE_TUI_H */
