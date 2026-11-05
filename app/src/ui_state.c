#include "ui_state.h"

#include <windows.h>
#include <commctrl.h>

/* Win32 pattern: one instance of the whole UI lives in file-scope globals
 * (single window, single player). Everything below is UI-thread-only, so no
 * lock is needed here - the only cross-thread object is g_player, which has
 * its own critical section. */
Controls g_controls;
HWND g_hwnd;
Player *g_player;
HFONT g_font;
int g_font_loaded;
HBRUSH g_brushFace;
HBRUSH g_brushBg;
HBRUSH g_brushBorde;
ThemeChoice g_theme_choice = THEME_SYSTEM;
int g_ui_theme_dark;
int g_active_mode;
LibraryState g_library = { NULL, 0, 0, -1 };
PlayerSnapshot g_player_snapshot;
uint32_t g_last_reported_error;
int g_decode_reported;
int g_selftest_mode;
FILE *g_selftest_report;
wchar_t g_selftest_invalid_path[1024];
int g_has_invalid_path;
wchar_t g_nameFull[512];
uint64_t g_progress_position;
uint64_t g_progress_total;
uint32_t g_progress_loop_start;
int g_progress_has_loop;
int g_progress_dragging;
int g_progress_drag_x;
HBRUSH g_brushProgFill;   /* played portion */
HBRUSH g_brushProgLoop;   /* loop region */
HBRUSH g_brushProgMarker; /* loop start line */
SettingsStatus g_settings_status = SETTINGS_OK;

void move_ctl(HWND h, int x, int y, int w, int hh)
{
    if (h)
        SetWindowPos(h, NULL, x, y, w, hh,
                     SWP_NOZORDER | SWP_NOACTIVATE);
}

void set_status(const wchar_t *text)
{
    if (g_controls.status)
        SendMessageW(g_controls.status, SB_SETTEXTW, 0, (LPARAM)text);
}

void ctl_show(HWND h, int show)
{
    if (h)
        ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

/* Apply a speed button: send it to the engine and update the label at once. */
void apply_speed(uint32_t ppm)
{
    wchar_t buf[40];
    player_set_speed(g_player, ppm);
    format_speed(buf, 40, ppm);
    SetWindowTextW(g_controls.valSpeed, buf);
}
