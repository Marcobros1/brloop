#ifndef UI_STATE_H
#define UI_STATE_H

#include <stdio.h>

#include "ui_types.h"
#include "library.h"
#include "settings.h"
#include "player.h"

/* Shared UI state (was file-scope static in main.c). One instance of the UI
 * lives here; every module accesses it through these extern declarations. */
extern Controls g_controls;         /* every child control handle, filled once */
extern HWND g_hwnd;                 /* main window */
extern Player *g_player;            /* engine */
extern HFONT g_font;                /* shared UI font for all controls */
extern int g_font_loaded;
extern HBRUSH g_brushFace;          /* panel background (group boxes, labels) */
extern HBRUSH g_brushBg;            /* client area background */
extern HBRUSH g_brushBorde;         /* control borders */
extern ThemeChoice g_theme_choice;  /* saved theme choice */
extern int g_ui_theme_dark;         /* 1 = dark theme in force */
extern int g_active_mode;           /* 0 = Biblioteca, 2 = Ajustes */
extern LibraryState g_library;
extern PlayerSnapshot g_player_snapshot;
extern uint32_t g_last_reported_error;
extern int g_decode_reported;
extern int g_selftest_mode;
extern FILE *g_selftest_report;
extern wchar_t g_selftest_invalid_path[1024];
extern int g_has_invalid_path;
extern wchar_t g_nameFull[512];     /* track name shown in the banner box */
extern uint64_t g_progress_position;
extern uint64_t g_progress_total;
extern uint32_t g_progress_loop_start;
extern int g_progress_has_loop;
extern int g_progress_dragging;
extern int g_progress_drag_x;
extern HBRUSH g_brushProgFill;   /* played portion */
extern HBRUSH g_brushProgLoop;   /* loop region */
extern HBRUSH g_brushProgMarker; /* loop start line */

/* Font asset shipped with the app (SIL OFL 1.1, see app/assets/fonts/OFL.txt). */
#define UI_FONT_FACE L"Pixelify Sans"
#define UI_FONT_FILE L"assets\\fonts\\PixelifySans.ttf"

/* Small utilities. */
void move_ctl(HWND h, int x, int y, int w, int hh);
void set_status(const wchar_t *text);
void ctl_show(HWND h, int show);

/* Declared in main.c, used by other modules. */
int narrow_path(const wchar_t *w, char *out, size_t outlen);
const wchar_t *file_name_of(const wchar_t *path);
void set_system_message(const wchar_t *text);
void format_time(wchar_t *out, size_t n, double sec);
void apply_speed(uint32_t ppm);

#endif /* UI_STATE_H */
