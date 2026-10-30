#ifndef UI_SELFTEST_H
#define UI_SELFTEST_H

#include <stddef.h>
#include <stdio.h>
#include <windows.h>

#include "player.h"
#include "ui_types.h"

/* The selftest uses the same library-loading path as a user action. The path
 * is borrowed for the callback duration; implementations copy it if needed. */
typedef int (*UiSelfTestAddTrack)(void *context, const wchar_t *path,
                                  wchar_t *error, size_t error_length);

/* Borrowed application state used while the hidden window is alive. The
 * selftest updates only checks, fails and resource baselines; the remaining
 * pointers refer to state owned by main.c and must stay valid for the run. */
typedef struct {
    HWND window;
    Controls *controls;
    Player *player;
    PlayerSnapshot *snapshot;
    int *row_count;
    int *selected;
    const wchar_t *invalid_path;
    int has_invalid_path;
    FILE *report;                 /* borrowed; closed by the selftest */
    UiSelfTestAddTrack add_track;
    void *add_track_context;
    int checks;                   /* output: total assertions */
    int fails;                    /* output: failed assertions */
    DWORD baseline_handles;       /* output: first-play handle count */
    SIZE_T baseline_memory;       /* output: first-play working set */
    LibraryState *library;        /* borrowed: removal checks read its rows */
    int font_loaded;              /* 1 when the bundled UI font was loaded */
    HFONT ui_font;                /* borrowed: the font the controls use */
} UiSelfTestContext;

/* Run the real window handlers and return 0 only when every check passes. */
int ui_selftest_run(UiSelfTestContext *context);

#endif /* UI_SELFTEST_H */
