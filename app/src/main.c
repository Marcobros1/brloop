/* BRLoop desktop prototype (T2): Win32 test UI for the BRSTM
 * playback engine.
 *
 * Layering: this file only handles windows, controls and file listing;
 * every audio/BRSTM decision goes through player.h. The UI thread never
 * reads samples or touches waveOut: it calls the player API (short critical
 * sections) and polls a snapshot from a 100 ms timer.
 *
 * --selftest drives the real window and its real handlers through a
 * scripted session (open, play, pause, resume, speed change, loop toggle,
 * track switch, close) and exits 0/1, so the automated tests exercise the
 * same code paths a user clicks.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <commdlg.h>
#include <psapi.h>
#include <winreg.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#include "player.h"
#include "ui_types.h"
#include "ui_selftest.h"
#include "cli_options.h"
#include "library.h"
#include "transport.h"
#include "strings.h"
#include "settings.h"
#include "icons.h"

#include "ui_state.h"
#include "ui_layout.h"
#include "ui_library.h"
#include "ui_dialogs.h"
#include "panel_slide.h"
#include "ui_panels.h"
#include "anim.h"

/* Forward declarations: functions defined in other modules that main.c calls.
 * ui_state.h externs the shared globals; these are the helpers main.c needs. */
void set_active_mode(int mode);
void start_mode_slide(int mode);
static void set_active_mode_visibility(int mode);

/* ------------------------------------------------------------- app state --
 * All file-scope globals moved to ui_state.c / ui_state.h. main.c keeps only
 * the settings load status, which is defined near load_saved_language. */

/* Active colour palette, filled by palette_load (startup and list picks). */
Palette g_palette;

/* Dark when Windows apps use the dark colour mode. Reads
 * HKCU\...\Themes\Personalize AppsUseLightTheme (0 = dark, 1 = light).
 * A missing key or a read error means light. */
static int system_theme_is_dark(void)
{
    DWORD val = 1;            /* default: light theme */
    DWORD size = sizeof val;
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              L"AppsUseLightTheme",
                              RRF_RT_REG_DWORD, NULL, &val, &size);
    if (st != ERROR_SUCCESS)
        return 0;             /* light when the value cannot be read */
    return val == 0;
}

/* Works out the theme in force from the saved choice. */
static int theme_is_dark_for(ThemeChoice choice)
{
    if (choice == THEME_LIGHT)
        return 0;
    if (choice == THEME_DARK)
        return 1;
    return system_theme_is_dark();
}

/* UTF-16 path -> ANSI path for the core (fopen). Fails clearly when the
 * path cannot be represented in the system code page. */
int narrow_path(const wchar_t *w, char *out, size_t outlen)
{
    BOOL used = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1,
                                out, (int)outlen, NULL, &used);
    return n > 0 && !used;
}

/* File name portion of a path, for display and error messages (handles both
 * separator styles; no allocation, points into `path`). */
const wchar_t *file_name_of(const wchar_t *path)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    const wchar_t *slash2 = wcsrchr(path, L'/');
    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    return slash ? slash + 1 : path;
}

/* "0:00.000" or "1:23:04.567" (hours only when needed). Milliseconds are
 * rounded, then split largest-first so the parts always add up. */
void format_time(wchar_t *out, size_t n, double sec)
{
    int ms, h, m, s;
    if (sec < 0.0)
        sec = 0.0;
    ms = (int)(sec * 1000.0 + 0.5);
    h = ms / 3600000; ms -= h * 3600000;
    m = ms / 60000;   ms -= m * 60000;
    s = ms / 1000;    ms -= s * 1000;
    if (h > 0)
        swprintf(out, n, L"%d:%02d:%02d.%03d", h, m, s, ms);
    else
        swprintf(out, n, L"%d:%02d.%03d", m, s, ms);
}

/* ---------------------------------------------------------------- rows -- */

/* Declared in ui_state.h / ui_library.h; main.c calls them via those headers. */

/* ------------------------------------------------- progress with loop ---- */

/* Custom progress bar: the loop section [loop_start, total) is shaded and a
 * marker line shows the loop start, so both loop points are visible while
 * the song plays. Paint state is UI-thread-only (set in update_ui). */
static void set_language(StrLanguage lang);
static void set_theme(ThemeChoice choice);

/* Seek: click (or drag end) on the bar jumps to that source position. */

/* Map a click/drag x position to a source sample and seek there. The 1px
 * frame on each side of the custom bar is excluded, and the ratio is done
 * in 64-bit to avoid overflow on long files. Seeking while paused or
 * stopped just moves the cursor (player.c contract), so this works in any
 * state. */
static void prog_seek_from_x(HWND hwnd, int x)
{
    RECT rc;
    int span;

    if (!g_player || g_progress_total == 0)
        return;
    GetClientRect(hwnd, &rc);
    span = (rc.right - rc.left) - 2; /* minus the 1px frame on each side */
    if (span < 1)
        return;
    x -= 1;
    if (x < 0) x = 0;
    if (x > span) x = span;
    if (player_seek(g_player, (uint64_t)g_progress_total * (uint64_t)x
                    / (uint64_t)span) == 0)
        update_ui();
}

/* Custom progress bar window procedure:
 *  - WM_PAINT: background, loop region shading, played fill, loop-start
 *    marker and (while dragging) a temporary position line. All paint state
 *    is the g_prog_* globals updated by update_ui on the UI thread.
 *  - button messages: press starts a drag + immediate seek, move updates the
 *    drag line, release seeks to the final x (drag-then-release matches how
 *    a media slider behaves; a plain click already seeks on press).
 * Capture keeps mouse messages coming to this control even outside it. */
static LRESULT CALLBACK progress_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        int w, h;

        GetClientRect(hwnd, &rc);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
        FillRect(dc, &rc, g_brushBg);
        FrameRect(dc, &rc, g_brushBorde);
        if (g_progress_total > 0 && w > 2 && h > 2) {
            int span = w - 2;
            int loop_x = -1;
            int fill_x;

            if (g_progress_has_loop) {
                loop_x = 1 + (int)((uint64_t)span * g_progress_loop_start /
                                   g_progress_total);
                if (loop_x > w - 2)
                    loop_x = w - 2;
                if (loop_x > 1) {
                    RECT lr = { loop_x, 1, w - 1, h - 1 };
                    FillRect(dc, &lr, g_brushProgLoop);
                }
            }
            fill_x = 1 + (int)((uint64_t)span * g_progress_position /
                               g_progress_total);
            if (fill_x > w - 1)
                fill_x = w - 1;
            if (fill_x > 1) {
                RECT fr = { 1, 1, fill_x, h - 1 };
                FillRect(dc, &fr, g_brushProgFill);
            }
            if (loop_x >= 0) {
                RECT mr = { loop_x - 1, 0, loop_x + 1, h };
                FillRect(dc, &mr, g_brushProgMarker);
            }
        }
        if (g_progress_dragging && w > 2) {
            int dx = g_progress_drag_x < 1 ? 1 : g_progress_drag_x;
            if (dx > w - 2) dx = w - 2;
            RECT dr = { dx - 1, 0, dx + 1, h };
            FillRect(dc, &dr, g_brushProgFill);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (g_progress_total > 0) {
            g_progress_dragging = 1;
            g_progress_drag_x = (int)(short)LOWORD(lp);
            SetCapture(hwnd);
            prog_seek_from_x(hwnd, g_progress_drag_x);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (g_progress_dragging) {
            g_progress_drag_x = (int)(short)LOWORD(lp);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_progress_dragging) {
            g_progress_dragging = 0;
            ReleaseCapture();
            prog_seek_from_x(hwnd, (int)(short)LOWORD(lp));
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------ creation -- */

/* Create one child control with the shared font applied. The pattern
 * (CreateWindowExW + WM_SETFONT) repeats for ~40 controls, hence the
 * helper; `id` 0 = static/groupbox with no command traffic. */
static HWND make_ctl(HWND parent, DWORD ex, const wchar_t *cls,
                     const wchar_t *text, DWORD style, int id,
                     int x, int y, int w, int h)
{
    HWND ctl = CreateWindowExW(ex, cls, text,
                               WS_CHILD | WS_VISIBLE | style,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(NULL), NULL);
    if (ctl)
        SendMessageW(ctl, WM_SETFONT, (WPARAM)g_font, TRUE);
    return ctl;
}

/* Build the whole control tree (library, info panel, transport, speed,
 * status bar) and put it in its initial disabled state: no track selected
 * yet, so play/home/next/remove/clear/loop/pair/speed all start disabled and
 * update_ui enables them as state allows. */
static void create_controls(HWND parent)
{
    INITCOMMONCONTROLSEX icc;
    LV_COLUMNW col;
    int i;

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES
              | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_controls.groupLib = make_ctl(parent, 0, L"BUTTON", tr(STR_LIBRARY_GROUP),
                              BS_GROUPBOX, IDC_GROUP_LIB, 0, 0, 0, 0);
    g_controls.btnOpen = make_ctl(parent, 0, L"BUTTON", tr(STR_BUTTON_OPEN),
                             BS_OWNERDRAW, IDC_BTN_OPEN, 0, 0, 0, 0);
    g_controls.btnRemove = make_ctl(parent, 0, L"BUTTON", tr(STR_BUTTON_REMOVE),
                               BS_OWNERDRAW, IDC_BTN_REMOVE, 0, 0, 0, 0);
    g_controls.btnClear = make_ctl(parent, 0, L"BUTTON", tr(STR_BUTTON_CLEAR),
                              BS_OWNERDRAW, IDC_BTN_CLEAR, 0, 0, 0, 0);
    g_controls.list = make_ctl(parent, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SHOWSELALWAYS, IDC_LIST,
                          0, 0, 0, 0);
    ListView_SetExtendedListViewStyle(g_controls.list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    /* Drop the Windows visual theme so the palette colours apply to the rows.
     * The header keeps the Windows style. */
    SetWindowTheme(g_controls.list, L"", L"");
    ListView_SetBkColor(g_controls.list, UI_C_SUPERFICIE);
    ListView_SetTextBkColor(g_controls.list, UI_C_SUPERFICIE);
    ListView_SetTextColor(g_controls.list, UI_C_PRIMARIO);

    memset(&col, 0, sizeof col);
    col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
    col.fmt = LVCFMT_LEFT;
    {
        const wchar_t *names[5] = { tr(STR_COL_NAME), tr(STR_COL_RATE), tr(STR_COL_CHANNELS),
                                    tr(STR_COL_DURATION), tr(STR_COL_LOOP) };
        int widths[5] = { 170, 55, 62, 74, 48 };
        for (i = 0; i < 5; i++) {
            col.pszText = (wchar_t *)names[i];
            col.cx = widths[i];
            col.iSubItem = i;
            ListView_InsertColumn(g_controls.list, i, &col);
        }
    }

    g_controls.groupInfo = make_ctl(parent, 0, L"BUTTON",
                               tr(STR_FILE_INFO_GROUP), BS_GROUPBOX,
                               IDC_GROUP_INFO, 0, 0, 0, 0);
    g_controls.capName = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_NAME), 0,
                             IDC_CAP_NAME, 0, 0, 0, 0);
    g_controls.capCh = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_CHANNELS), 0,
                           IDC_CAP_CH, 0, 0, 0, 0);
    g_controls.capRate = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_RATE), 0,
                             IDC_CAP_RATE, 0, 0, 0, 0);
    g_controls.capDur = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_DURATION), 0,
                            IDC_CAP_DUR, 0, 0, 0, 0);
    g_controls.capSamples = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_SAMPLES), 0,
                                IDC_CAP_SAMPLES, 0, 0, 0, 0);
    g_controls.capLoop = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_LOOP), 0,
                             IDC_CAP_LOOP, 0, 0, 0, 0);
    g_controls.capLoopSt = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_LOOP_START), 0,
                               IDC_CAP_LOOPST, 0, 0, 0, 0);
    g_controls.capLoopEnd = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_LOOP_END), 0,
                                IDC_CAP_LOOPEND, 0, 0, 0, 0);
    g_controls.capPath = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_PATH), 0,
                             IDC_CAP_PATH, 0, 0, 0, 0);
    g_controls.valName = make_ctl(parent, 0, L"STATIC", L"\u2014", 0,
                             IDC_VAL_NAME, 0, 0, 0, 0);
    g_controls.valPath = make_ctl(parent, 0, L"EDIT", L"",
                             ES_READONLY | ES_AUTOHSCROLL, IDC_VAL_PATH,
                             0, 0, 0, 0);
    g_controls.valRate = make_ctl(parent, 0, L"STATIC", L"\u2014", 0,
                             IDC_VAL_RATE, 0, 0, 0, 0);
    g_controls.valCh = make_ctl(parent, 0, L"STATIC", L"\u2014", 0,
                           IDC_VAL_CH, 0, 0, 0, 0);
    g_controls.valSamples = make_ctl(parent, 0, L"STATIC", L"\u2014", 0,
                                IDC_VAL_SAMPLES, 0, 0, 0, 0);
    g_controls.valDur = make_ctl(parent, 0, L"STATIC", L"\u2014", 0,
                            IDC_VAL_DUR, 0, 0, 0, 0);
    g_controls.valLoop = make_ctl(parent, 0, L"STATIC", L"\u2014", 0,
                             IDC_VAL_LOOP, 0, 0, 0, 0);
    g_controls.valLoopSt = make_ctl(parent, 0, L"STATIC", tr(STR_LOOP_UNAVAILABLE), 0,
                               IDC_VAL_LOOPST, 0, 0, 0, 0);
    g_controls.valLoopEnd = make_ctl(parent, 0, L"STATIC", tr(STR_LOOP_UNAVAILABLE), 0,
                                IDC_VAL_LOOPEND, 0, 0, 0, 0);

    g_controls.groupPlay = make_ctl(parent, 0, L"BUTTON", tr(STR_PLAY_GROUP),
                               BS_GROUPBOX, 0, 0, 0, 0, 0);
    /* Transport like an MP3 player: play doubles as pause (its caption says
     * what the next click does), "Anterior" goes back (or restarts a song
     * that is more than 3 s in), "Siguiente" moves on to the next library
     * row. There is deliberately no separate stop button. */
    /* Owner-drawn: the glyph comes from icons.c and the caption stays as the
     * accessible name, so screen readers and the tooltip still get the word. */
    g_controls.btnPrevious = make_ctl(parent, 0, L"BUTTON", tr(STR_BUTTON_PREVIOUS),
                             BS_OWNERDRAW, IDC_BTN_PREVIOUS, 0, 0, 0, 0);
    g_controls.btnPlay = make_ctl(parent, 0, L"BUTTON", tr(STR_BUTTON_PLAY),
                             BS_OWNERDRAW, IDC_BTN_PLAY, 0, 0, 0, 0);
    g_controls.btnNext = make_ctl(parent, 0, L"BUTTON", tr(STR_BUTTON_NEXT),
                             BS_OWNERDRAW, IDC_BTN_NEXT, 0, 0, 0, 0);
    /* Removed: btnLang / btnTheme bottom-bar buttons. Language and theme now
     * live in the Settings panel (Q-121, Q-122). */

    /* Mode bar (A2): owner-drawn buttons; the active one is painted with
     * color and border (Q-078, WCAG 1.4.1). */
    g_controls.btnModeLibrary = make_ctl(parent, 0, L"BUTTON", tr(STR_MODE_LIBRARY),
                                  BS_OWNERDRAW, IDC_BTN_MODE_LIBRARY, 0, 0, 0, 0);
    g_controls.btnModeSettings = make_ctl(parent, 0, L"BUTTON", tr(STR_MODE_SETTINGS),
                                   BS_OWNERDRAW, IDC_BTN_MODE_SETTINGS, 0, 0, 0, 0);
    /* Settings panel controls (Q-121, Q-122): language, theme, palette.
     * Created here, positioned by layout(); shown only in Settings mode.
     */
    g_controls.capLang = make_ctl(parent, 0, L"STATIC", tr(STR_SETTINGS_LANGUAGE),
                               0, IDC_CAP_LANG, 0, 0, 0, 0);
    g_controls.capTheme = make_ctl(parent, 0, L"STATIC", tr(STR_SETTINGS_THEME),
                                0, IDC_CAP_THEME, 0, 0, 0, 0);
    g_controls.capPalette = make_ctl(parent, 0, L"STATIC", tr(STR_SETTINGS_PALETTE),
                                  0, IDC_CAP_PALETTE, 0, 0, 0, 0);
    g_controls.btnLangEn = make_ctl(parent, 0, L"BUTTON", tr(STR_MENU_ENGLISH),
                                 BS_OWNERDRAW, IDC_BTN_LANG_EN, 0, 0, 0, 0);
    g_controls.btnLangEs = make_ctl(parent, 0, L"BUTTON", tr(STR_MENU_SPANISH),
                                 BS_OWNERDRAW, IDC_BTN_LANG_ES, 0, 0, 0, 0);
    g_controls.btnThemeSystem = make_ctl(parent, 0, L"BUTTON", tr(STR_MENU_THEME_SYSTEM),
                                      BS_OWNERDRAW, IDC_BTN_THEME_SYSTEM, 0, 0, 0, 0);
    g_controls.btnThemeLight = make_ctl(parent, 0, L"BUTTON", tr(STR_MENU_THEME_LIGHT),
                                     BS_OWNERDRAW, IDC_BTN_THEME_LIGHT, 0, 0, 0, 0);
    g_controls.btnThemeDark = make_ctl(parent, 0, L"BUTTON", tr(STR_MENU_THEME_DARK),
                                    BS_OWNERDRAW, IDC_BTN_THEME_DARK, 0, 0, 0, 0);
    g_controls.lstPalette = make_ctl(parent, 0, L"LISTBOX", L"",
                                  LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
                                  IDC_LST_PALETTE, 0, 0, 0, 0);
    /* Settings palette list: catalogue names (Q-112, Q-116). Wii Blanco is
     * the initial choice; the rest follow A-Z. Applied in task (b). */
    {
        static const wchar_t *const names[] = {
            L"Wii Blanco",
            L"Animal Crossing",
            L"GameCube Indigo",
            L"GameCube Jet Black",
            L"GameCube Spice",
            L"Mario Kart Double Dash",
            L"Mario Kart Wii",
            L"Metroid Prime",
            L"Metroid Prime 2",
            L"Metroid Prime 3",
            L"New Super Mario Bros. Wii",
            L"Resident Evil 4",
            L"Smash Brawl",
            L"Super Mario Galaxy",
            L"Super Mario Galaxy 2",
            L"Super Mario Sunshine",
            L"Super Smash Bros. Melee",
            L"Wii Azul",
            L"Wii Fit",
            L"Wii Sports",
            L"Wii Sports Resort",
            L"Zelda Skyward Sword",
            L"Zelda Twilight Princess",
            L"Zelda Wind Waker"
        };
        size_t k;
        for (k = 0; k < sizeof names / sizeof names[0]; k++)
            SendMessageW(g_controls.lstPalette, LB_ADDSTRING, 0,
                         (LPARAM)names[k]);
        /* Wii Blanco is the initial selection (Q-112). */
        SendMessageW(g_controls.lstPalette, LB_SETCURSEL, 0, 0);
    }


    g_controls.progress = make_ctl(parent, 0, PROGRESS_CLASS_NAME, L"",
                              0, IDC_PROGRESS, 0, 0, 0, 0);
    g_controls.valState = make_ctl(parent, 0, L"STATIC", L"Estado: Detenido",
                              SS_RIGHT, IDC_VAL_STATE, 0, 0, 0, 0);
    /* Short system message to the right of state. */
    g_controls.valSystem = make_ctl(parent, 0, L"STATIC", L"",
                               SS_LEFT, IDC_VAL_SYSTEM, 0, 0, 0, 0);
    ShowWindow(g_controls.valSystem, SW_HIDE);   /* no message at startup */
    g_controls.valTime = make_ctl(parent, 0, L"STATIC", L"0:00.000 / 0:00.000",
                             0, IDC_VAL_TIME, 0, 0, 0, 0);
    g_controls.checkLoop = make_ctl(parent, 0, L"BUTTON", tr(STR_CHECK_LOOP),
                               BS_AUTOCHECKBOX | BS_LEFT, IDC_CHECK_LOOP,
                               0, 0, 0, 0);

    g_controls.groupSpeed = make_ctl(parent, 0, L"BUTTON", tr(STR_SPEED_GROUP),
                                BS_GROUPBOX, IDC_GROUP_SPEED, 0, 0, 0, 0);
    g_controls.valSpeed = make_ctl(parent, 0, L"STATIC", tr(STR_SPEED_NORMAL), SS_RIGHT,
                              IDC_VAL_SPEED, 0, 0, 0, 0);
    g_controls.valDevRate = make_ctl(parent, 0, L"STATIC", tr(STR_DEVICE_RATE_EMPTY),
                                SS_RIGHT, IDC_VAL_DEVRATE, 0, 0, 0, 0);
    g_controls.btnSpeed = make_ctl(parent, 0, L"BUTTON", tr(STR_SPEED_NORMAL),
                              BS_OWNERDRAW, IDC_BTN_SPEED, 0, 0, 0, 0);
    g_controls.capVer = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_VERSION), 0,
                            IDC_CAP_VER, 0, 0, 0, 0);
    g_controls.btnPair = make_ctl(parent, 0, L"BUTTON", L"\u2014", BS_OWNERDRAW,
                             IDC_BTN_PAIR, 0, 0, 0, 0);
    SetWindowTheme(g_controls.btnPair, L"", L"");

    g_controls.status = CreateWindowExW(0, STATUSCLASSNAMEW, tr(STR_STATUS_READY),
                                   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, parent,
                                   (HMENU)(INT_PTR)2, GetModuleHandleW(NULL),
                                   NULL);
    SendMessageW(g_controls.status, WM_SETFONT, (WPARAM)g_font, TRUE);
    /* The status bar uses the app's normal font, not the large one. */

    g_active_mode = -1;
    set_active_mode(0);   /* A2: opens Library and sets up the initial layout */
    EnableWindow(g_controls.btnPlay, FALSE);
    EnableWindow(g_controls.btnPrevious, FALSE);
    EnableWindow(g_controls.btnNext, FALSE);
    EnableWindow(g_controls.btnRemove, FALSE);
    EnableWindow(g_controls.btnClear, FALSE);
    EnableWindow(g_controls.checkLoop, FALSE);
    EnableWindow(g_controls.btnPair, FALSE);
}

/* ----------------------------------------------------------------- UI --- */

/* Command router: WM_COMMAND (button/menu/accelerator ids) to actions.
 * Each case does its own status message; update_ui() at the end re-syncs
 * everything from the engine rather than assuming the action worked, so the
 * display can never contradict the player state. */
static void on_command(int id)
{
    switch (id) {
    case IDC_BTN_OPEN:
        browse_files();
        break;
    /* Language and theme now live in the Settings panel (Q-121, Q-122),
     * raised as button commands instead of bottom-bar buttons / menus. */
    case IDC_BTN_LANG_EN:
        set_language(STR_LANG_EN);
        break;
    case IDC_BTN_LANG_ES:
        set_language(STR_LANG_ES);
        break;
    case IDC_BTN_THEME_SYSTEM:
        set_theme(THEME_SYSTEM);
        break;
    case IDC_BTN_THEME_LIGHT:
        set_theme(THEME_LIGHT);
        break;
    case IDC_BTN_THEME_DARK:
        set_theme(THEME_DARK);
        break;
    case IDC_BTN_MODE_LIBRARY:
        start_mode_slide(0);
        break;
    case IDC_BTN_MODE_SETTINGS:
        start_mode_slide(2);
        break;
    case IDC_BTN_REMOVE:
        if (g_library.selected_index >= 0)
            remove_track(g_library.selected_index);
        break;
    case IDC_BTN_CLEAR:
        remove_all();
        break;
    case IDC_BTN_PLAY: {
        /* One button for play and pause; the live state decides (g_player_snapshot may
         * be up to one tick behind). FINISHED restarts from the beginning,
         * because "play" on a finished song means hearing it again. */
        PlayerState st = player_get_state(g_player).state;
        TransportPlayAction action = transport_play_action(st);
        if (action == TRANSPORT_PLAY_RESUME) {
            player_play(g_player);
            set_status(tr(STR_RESUMING));
        } else if (action == TRANSPORT_PLAY_RESTART) {
            player_seek(g_player, 0);
            if (player_play(g_player) == 0)
                set_status(tr(STR_PLAYING_FROM_START));
            else
                set_status(tr(STR_PLAY_NO_AUDIO));
        } else if (action == TRANSPORT_PLAY_PAUSE) {
            player_pause(g_player);
            set_status(tr(STR_PAUSED));
        } else {
            if (player_play(g_player) == 0)
                set_status(tr(STR_PLAYING));
            else
                set_status(tr(STR_PLAY_NO_FILE));
        }
        break;
    }
    case IDC_BTN_PREVIOUS: {
        /* Media-player rule: more than 3 s into the song means "start it
         * over"; at the beginning it means "previous track" (wrapping to
         * the last row). Keeps playing across a track change. */
        PlayerSnapshot s = player_get_state(g_player);
        int prev = transport_previous_index(g_library.selected_index, g_library.count);
        if (transport_previous_action(s.position, s.sample_rate) == TRANSPORT_PREVIOUS_TRACK
            && prev >= 0 && prev != g_library.selected_index) {
            int wasPlaying = s.state == PLAYER_PLAYING;
            change_track(prev, wasPlaying);
            set_status(tr(STR_PREVIOUS_TRACK));
        } else {
            player_seek(g_player, 0);
            set_status(tr(STR_BACK_TO_START));
        }
        break;
    }
    case IDC_BTN_NEXT: {
        int next;
        int wasPlaying;
        next = transport_next_index(g_library.selected_index, g_library.count);
        if (next < 0)
            break;
        /* Keep playing across the change if the user was listening. */
        wasPlaying = player_get_state(g_player).state == PLAYER_PLAYING;
        change_track(next, wasPlaying);
        set_status(tr(STR_NEXT_TRACK));
        break;
    }
    case IDC_CHECK_LOOP:
        player_set_loop(g_player,
                        SendMessageW(g_controls.checkLoop, BM_GETCHECK, 0, 0)
                            == BST_CHECKED);
        break;
    case IDC_BTN_SPEED:
        /* Single toggle between the two allowed modes. */
        apply_speed(player_get_state(g_player).speed_ppm
                        == PLAYER_FLSM_SPEED_PPM
                    ? PLAYER_SPEED_SCALE
                    : PLAYER_FLSM_SPEED_PPM);
        break;
    case IDC_BTN_PAIR: {
        PlayerSnapshot s = player_get_state(g_player);
        wchar_t msg[80];
        int next;
        /* The UI never offers the engine's internal mix-all value (-1). */
        next = transport_next_pair(s.pair_sel, s.pair_count);
        if (next < 0)
            break;
        if (player_set_channel_pair(g_player, next) != 0)
            break;
        swprintf(msg, 80, tr(STR_VERSION_SELECTED),
                 next + 1, next * 2 + 1, next * 2 + 2);
        set_status(msg);
        break;
    }
    default:
        break;
    }
    update_ui();
}

/* --------------------------------------------------------- window proc -- */

/* Reads the saved language before any control is created, so the window
 * opens already in the right language. An unreadable or unsupported file is
 * reported in the status bar once the controls exist (see apply_language). */
static SettingsStatus g_settings_status = SETTINGS_OK;

static void load_saved_language(void)
{
    StrLanguage lang = STR_LANG_EN;
    g_settings_status = settings_load_language(&lang);
    strings_set_language(lang);
}

/* Writes the static caption of every control that create_controls() labels
 * with tr(). Controls whose text update_ui() owns (state, speed, version,
 * device, time) are not touched here: update_ui() rewrites them on the next
 * tick, and the cache it keeps is cleared below so that rewrite happens. */
static void relabel_controls(void)
{
    SetWindowTextW(g_controls.groupLib, tr(STR_LIBRARY_GROUP));
    SetWindowTextW(g_controls.btnModeLibrary, tr(STR_MODE_LIBRARY));
    SetWindowTextW(g_controls.btnModeSettings, tr(STR_MODE_SETTINGS));
    SetWindowTextW(g_controls.btnOpen, tr(STR_BUTTON_OPEN));
    SetWindowTextW(g_controls.btnRemove, tr(STR_BUTTON_REMOVE));
    SetWindowTextW(g_controls.btnClear, tr(STR_BUTTON_CLEAR));
    SetWindowTextW(g_controls.groupInfo, tr(STR_FILE_INFO_GROUP));
    SetWindowTextW(g_controls.capName, tr(STR_CAP_NAME));
    SetWindowTextW(g_controls.capCh, tr(STR_CAP_CHANNELS));
    SetWindowTextW(g_controls.capRate, tr(STR_CAP_RATE));
    SetWindowTextW(g_controls.capDur, tr(STR_CAP_DURATION));
    SetWindowTextW(g_controls.capSamples, tr(STR_CAP_SAMPLES));
    SetWindowTextW(g_controls.capLoop, tr(STR_CAP_LOOP));
    SetWindowTextW(g_controls.capLoopSt, tr(STR_CAP_LOOP_START));
    SetWindowTextW(g_controls.capLoopEnd, tr(STR_CAP_LOOP_END));
    SetWindowTextW(g_controls.capPath, tr(STR_CAP_PATH));
    list_refresh_loop_column();
    SetWindowTextW(g_controls.valLoopSt, tr(STR_LOOP_UNAVAILABLE));
    SetWindowTextW(g_controls.valLoopEnd, tr(STR_LOOP_UNAVAILABLE));
    SetWindowTextW(g_controls.groupPlay, tr(STR_PLAY_GROUP));
    SetWindowTextW(g_controls.btnPrevious, tr(STR_BUTTON_PREVIOUS));
    SetWindowTextW(g_controls.btnNext, tr(STR_BUTTON_NEXT));
    SetWindowTextW(g_controls.checkLoop, tr(STR_CHECK_LOOP));
    SetWindowTextW(g_controls.groupSpeed, tr(STR_SPEED_GROUP));
    {
        LVCOLUMNW col;
        int i;
        const wchar_t *names[5] = { tr(STR_COL_NAME), tr(STR_COL_RATE), tr(STR_COL_CHANNELS),
                                    tr(STR_COL_DURATION), tr(STR_COL_LOOP) };
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT;
        for (i = 0; i < 5; i++) {
            /* LVCOLUMNW.pszText is not const, but ListView_SetColumn only reads
             * the text; the cast satisfies the Win32 signature. */
            col.pszText = (wchar_t *)names[i];
            ListView_SetColumn(g_controls.list, i, &col);
        }
    }
    /* Force a rewrite of the texts update_ui() owns, instead of waiting for a
     * change that may never come (the cache there compares against the last
     * value written, which is now in the old language). */
    SetWindowTextW(g_controls.valState, L"");
    SetWindowTextW(g_controls.btnPlay, L"");
    SetWindowTextW(g_controls.btnSpeed, L"");
    SetWindowTextW(g_controls.btnPair, L"");
    SetWindowTextW(g_controls.valDevRate, L"");
    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* Loads the bundled font from assets\fonts next to the executable. A missing
 * file is reported in the status bar and the app falls back to the default
 * GUI font, so the UI still works without the asset. */
/* Full path of the bundled font: the folder of the executable plus the asset
 * path. Returns 0 when the path does not fit. */
static int ui_font_path(wchar_t *out, size_t out_len)
{
    wchar_t exe[MAX_PATH];
    wchar_t *slash;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return 0;
    slash = wcsrchr(exe, L'\\');
    if (!slash)
        return 0;
    *(slash + 1) = L'\0';
    return swprintf(out, out_len, L"%s%s", exe, UI_FONT_FILE) >= 0;
}

static void load_ui_font(void)
{
    wchar_t path[MAX_PATH];
    if (!ui_font_path(path, MAX_PATH)) {
        g_font_loaded = 0;
        return;
    }
    g_font_loaded = AddFontResourceExW(path, FR_PRIVATE, NULL) > 0;
}

static void unload_ui_font(void)
{
    wchar_t path[MAX_PATH];
    if (!g_font_loaded)
        return;
    if (ui_font_path(path, MAX_PATH))
        RemoveFontResourceExW(path, FR_PRIVATE, NULL);
    g_font_loaded = 0;
}

/* Rebuilds every brush and colour that depends on the theme, then repaints.
 * Called at startup and whenever the theme choice or the system mode changes. */
static void apply_theme(void)
{
    HBRUSH oldBg = g_brushBg, oldBorde = g_brushBorde, oldFace = g_brushFace;
    HBRUSH oldFill = g_brushProgFill, oldLoop = g_brushProgLoop, oldMarker = g_brushProgMarker;

    g_ui_theme_dark = theme_is_dark_for(g_theme_choice);
    g_brushBg = CreateSolidBrush(UI_C_FONDO);
    g_brushBorde = CreateSolidBrush(UI_C_BORDE);
    g_brushFace = CreateSolidBrush(UI_C_SUPERFICIE);
    g_brushProgFill = CreateSolidBrush(UI_C_ACENTO);
    g_brushProgLoop = CreateSolidBrush(UI_C_SECUNDARIO);
    g_brushProgMarker = CreateSolidBrush(UI_C_ACENTO);

    /* The window class keeps its own background brush, so point it at the new one. */
    if (g_hwnd)
        SetClassLongPtrW(g_hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)g_brushBg);

    if (g_controls.list) {
        ListView_SetBkColor(g_controls.list, UI_C_SUPERFICIE);
        ListView_SetTextBkColor(g_controls.list, UI_C_SUPERFICIE);
        ListView_SetTextColor(g_controls.list, UI_C_PRIMARIO);
    }

    if (oldBg) DeleteObject(oldBg);
    if (oldBorde) DeleteObject(oldBorde);
    if (oldFace) DeleteObject(oldFace);
    if (oldFill) DeleteObject(oldFill);
    if (oldLoop) DeleteObject(oldLoop);
    if (oldMarker) DeleteObject(oldMarker);

    if (g_hwnd)
        RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* Switches the colour theme at runtime, saves the choice and repaints. */
static void set_theme(ThemeChoice choice)
{
    g_theme_choice = choice;
    if (!g_selftest_mode && settings_save_theme(choice) != 0)
        set_status(tr(STR_SETTINGS_SAVE_FAILED));
    apply_theme();
}

/* Settings palette list: loads the picked catalogue entry and rebuilds the
 * theme brushes (apply_theme repaints too). Keeps the old palette if the
 * name cannot be read or loaded. */
static void on_palette_changed(void)
{
    wchar_t wname[128];
    char name[128];
    LRESULT sel, len;

    sel = SendMessageW(g_controls.lstPalette, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return;
    len = SendMessageW(g_controls.lstPalette, LB_GETTEXTLEN, sel, 0);
    if (len == LB_ERR || len >= (LRESULT)(sizeof wname / sizeof wname[0]))
        return;
    SendMessageW(g_controls.lstPalette, LB_GETTEXT, sel, (LPARAM)wname);
    /* Catalogue names are ASCII, so the ANSI conversion is lossless. */
    if (!narrow_path(wname, name, sizeof name))
        return;
    if (palette_load(name, "Claro", &g_palette)) {
        if (!g_selftest_mode && settings_save_palette(wname) != 0)
            set_status(tr(STR_SETTINGS_SAVE_FAILED));
        apply_theme();
    }
}

/* ------------------------------------------------------------------ slide state -- */

/* Slide animation state for Q-170 (mode change panel slide). */
static Anim g_slide_anim;
static int g_slide_mode_from;   /* mode that was visible before slide started */
static int g_slide_mode_to;     /* mode that we're sliding to */
static int g_slide_active;      /* 1 = slide in progress */
static int g_slide_panel_width; /* width of the sliding panel (groupLib) */
static int g_slide_direction;   /* -1 = old leaves left (Library->Settings),
                                   +1 = old leaves right (Settings->Library) */

/* Panel slide (Q-170), window side. Every control of the outgoing panel
 * (ui_panels.c, set of g_slide_mode_from) moves with panel_slide_old_offset();
 * every control of the incoming panel (set of g_slide_mode_to) moves with
 * panel_slide_new_offset(). Positions are the layout() ones: the incoming
 * panel's rects are the final layout, and the outgoing panel keeps the rects
 * it had before the change. No unit test: this depends on the window. */

#define SLIDE_MAX_CTLS 32

/* Rect of every control of a mode, as layout() places it, plus the handle.
 * Filled once per slide: `from` before the change, `to` after layout(). */
typedef struct {
    HWND hwnd;
    RECT rc;      /* normal position, in client coordinates */
} SlideCtl;

static SlideCtl g_slide_from[SLIDE_MAX_CTLS];
static int g_slide_from_n;
static SlideCtl g_slide_to[SLIDE_MAX_CTLS];
static int g_slide_to_n;

/* Handle of a control id, looked up through the Controls struct. Only ids that
 * ui_panels.c lists are needed; any other id maps to NULL. */
static HWND ctl_by_id(int id)
{
    switch (id) {
    case IDC_BTN_OPEN: return g_controls.btnOpen;
    case IDC_BTN_REMOVE: return g_controls.btnRemove;
    case IDC_BTN_CLEAR: return g_controls.btnClear;
    case IDC_GROUP_LIB: return g_controls.groupLib;
    case IDC_LIST: return g_controls.list;
    case IDC_GROUP_INFO: return g_controls.groupInfo;
    case IDC_VAL_NAME: return g_controls.valName;
    case IDC_VAL_PATH: return g_controls.valPath;
    case IDC_CAP_RATE: return g_controls.capRate;
    case IDC_VAL_RATE: return g_controls.valRate;
    case IDC_CAP_CH: return g_controls.capCh;
    case IDC_VAL_CH: return g_controls.valCh;
    case IDC_CAP_DUR: return g_controls.capDur;
    case IDC_VAL_DUR: return g_controls.valDur;
    case IDC_CAP_LOOP: return g_controls.capLoop;
    case IDC_VAL_LOOP: return g_controls.valLoop;
    case IDC_GROUP_SPEED: return g_controls.groupSpeed;
    case IDC_VAL_SPEED: return g_controls.valSpeed;
    case IDC_VAL_DEVRATE: return g_controls.valDevRate;
    case IDC_BTN_SPEED: return g_controls.btnSpeed;
    case IDC_BTN_PAIR: return g_controls.btnPair;
    case IDC_CAP_VER: return g_controls.capVer;
    case IDC_CAP_LANG: return g_controls.capLang;
    case IDC_CAP_THEME: return g_controls.capTheme;
    case IDC_CAP_PALETTE: return g_controls.capPalette;
    case IDC_BTN_LANG_EN: return g_controls.btnLangEn;
    case IDC_BTN_LANG_ES: return g_controls.btnLangEs;
    case IDC_BTN_THEME_SYSTEM: return g_controls.btnThemeSystem;
    case IDC_BTN_THEME_LIGHT: return g_controls.btnThemeLight;
    case IDC_BTN_THEME_DARK: return g_controls.btnThemeDark;
    case IDC_LST_PALETTE: return g_controls.lstPalette;
    default: return NULL;
    }
}

/* Snapshots the rect of every control of `panel` into `out`. */
static int slide_snapshot(PanelId panel, SlideCtl *out)
{
    int i, n = 0, count = panel_ids_count(panel);
    for (i = 0; i < count && n < SLIDE_MAX_CTLS; i++) {
        HWND h = ctl_by_id(panel_ids_get(panel, i));
        if (!h)
            continue;
        out[n].hwnd = h;
        GetWindowRect(h, &out[n].rc);
        MapWindowPoints(NULL, g_hwnd, (POINT *)&out[n].rc, 2);
        n++;
    }
    return n;
}

/* Moves the outgoing controls and the incoming ones according to
 * g_slide_direction: -1 = old leaves left / new enters from right
 * (Library -> Settings), +1 = old leaves right / new enters from left
 * (Settings -> Library). */
static void apply_slide_offsets(double progress)
{
    int i;
    int off_old = panel_slide_old_offset(progress, g_slide_panel_width,
                                         g_slide_direction);
    int off_new = panel_slide_new_offset(progress, g_slide_panel_width,
                                         g_slide_direction);

    for (i = 0; i < g_slide_from_n; i++) {
        RECT *r = &g_slide_from[i].rc;
        MoveWindow(g_slide_from[i].hwnd, r->left + off_old, r->top,
                   r->right - r->left, r->bottom - r->top, TRUE);
    }
    for (i = 0; i < g_slide_to_n; i++) {
        RECT *r = &g_slide_to[i].rc;
        MoveWindow(g_slide_to[i].hwnd, r->left + off_new, r->top,
                   r->right - r->left, r->bottom - r->top, TRUE);
    }
}

/* Union of the outgoing and incoming rects, repainted once the slide ends
 * (or while it moves). Only the panel area is invalidated, never the window. */
static void slide_invalidate_union(void)
{
    RECT u = { 0, 0, 0, 0 };
    int i, first = 1;
    for (i = 0; i < g_slide_from_n; i++) {
        if (first) { u = g_slide_from[i].rc; first = 0; }
        else UnionRect(&u, &u, &g_slide_from[i].rc);
    }
    for (i = 0; i < g_slide_to_n; i++) {
        if (first) { u = g_slide_to[i].rc; first = 0; }
        else UnionRect(&u, &u, &g_slide_to[i].rc);
    }
    if (!first) {
        /* The moving rects can cover up to a panel width beyond their normal
         * place; widen the invalid area by that much so no residue stays. */
        int pad = g_slide_panel_width;
        u.left -= pad;
        u.right += pad;
        InvalidateRect(g_hwnd, &u, FALSE);
    }
}

/* The mode buttons sit above the panels, outside the slide's union, so they
 * are repainted here: their accent colour follows g_active_mode. */
static void slide_invalidate_mode_buttons(void)
{
    HWND btns[2] = { g_controls.btnModeLibrary, g_controls.btnModeSettings };
    int i;
    for (i = 0; i < 2; i++) {
        RECT rc;
        if (btns[i] && GetWindowRect(btns[i], &rc)) {
            MapWindowPoints(NULL, g_hwnd, (POINT *)&rc, 2);
            InvalidateRect(g_hwnd, &rc, FALSE);
        }
    }
}

int mode_slide_running(void)
{
    return g_slide_active;
}

/* Starts the mode change (Q-170). The state changes at once; the animation
 * only moves the controls for 200 ms, from right to left. */
void start_mode_slide(int mode_to)
{
    RECT rc;
    if (g_active_mode == mode_to)
        return;

    g_slide_mode_from = g_active_mode;
    g_slide_mode_to = mode_to;
    /* Direction: -1 = old leaves left / new enters from right (Library ->
     * Settings). +1 = old leaves right / new enters from left
     * (Settings -> Library). */
    g_slide_direction = (g_slide_mode_from == 2) ? +1 : -1;

    /* Outgoing panel: where its controls are now. */
    g_slide_from_n = slide_snapshot(g_active_mode == 0 ? PANEL_LIBRARY : PANEL_SETTINGS,
                                    g_slide_from);
    g_slide_panel_width = 400;
    if (GetClientRect(g_controls.groupLib, &rc) && rc.right - rc.left > 0)
        g_slide_panel_width = rc.right - rc.left;

    /* State changes at once. Showing the incoming panel and laying it out
     * puts its controls at their normal positions. */
    g_active_mode = mode_to;
    set_active_mode_visibility(mode_to);
    layout();
    g_slide_to_n = slide_snapshot(mode_to == 0 ? PANEL_LIBRARY : PANEL_SETTINGS,
                                  g_slide_to);

    /* Incoming controls start off to the right, and go on top while moving. */
    {
        int i;
        for (i = 0; i < g_slide_to_n; i++)
            SetWindowPos(g_slide_to[i].hwnd, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    g_slide_active = 1;
    anim_start(&g_slide_anim, GetTickCount(), SLIDE_DURATION_MS);
    SetTimer(g_hwnd, TIMER_SLIDE, 16, NULL);
    apply_slide_offsets(0.0);
    slide_invalidate_union();
    slide_invalidate_mode_buttons();
}

/* Ends the mode change: the outgoing controls are hidden, the incoming ones
 * are back at their layout position, and the area is repainted. */
static void finish_mode_slide(void)
{
    int i;
    KillTimer(g_hwnd, TIMER_SLIDE);
    g_slide_active = 0;
    /* Invalida toda el area antes de reposicionar: borra cualquier resto
     * de pixeles del desplazamiento (botones clonados en posiciones
     * intermedias). Sin FillRect ni PrintWindow. */
    InvalidateRect(g_hwnd, NULL, TRUE);
    for (i = 0; i < g_slide_from_n; i++)
        ShowWindow(g_slide_from[i].hwnd, SW_HIDE);
    layout();
    slide_invalidate_union();
    slide_invalidate_mode_buttons();
    /* Forza el repintado inmediato de toda el area y todos los hijos:
     * elimina restos de pixeles del desplazamiento. Sin FillRect ni
     * PrintWindow. */
    RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

/* Shows the controls of `mode` and hides the rest (no layout). Used by the
 * instant change and by the slide, so both share one visibility rule. */
static void set_active_mode_visibility(int mode)
{
    int bib = (mode == 0);   /* Library: list + card + version */

    ctl_show(g_controls.btnOpen, bib);
    ctl_show(g_controls.btnRemove, bib);
    ctl_show(g_controls.btnClear, bib);
    ctl_show(g_controls.groupLib, bib);
    ctl_show(g_controls.list, bib);
    ctl_show(g_controls.groupInfo, bib);
    ctl_show(g_controls.valName, bib);
    ctl_show(g_controls.valPath, bib);
    ctl_show(g_controls.capRate, bib);
    ctl_show(g_controls.valRate, bib);
    ctl_show(g_controls.capCh, bib);
    ctl_show(g_controls.valCh, bib);
    ctl_show(g_controls.capDur, bib);
    ctl_show(g_controls.valDur, bib);
    ctl_show(g_controls.capLoop, bib);
    ctl_show(g_controls.valLoop, bib);
    ctl_show(g_controls.groupSpeed, bib);
    ctl_show(g_controls.valSpeed, bib);
    ctl_show(g_controls.valDevRate, bib);
    ctl_show(g_controls.btnSpeed, bib);
    ctl_show(g_controls.btnPair, bib);
    ctl_show(g_controls.capVer, bib);

    /* Fields the banner card (B1) does not show. */
    ctl_show(g_controls.capSamples, 0);
    ctl_show(g_controls.valSamples, 0);
    ctl_show(g_controls.capLoopSt, 0);
    ctl_show(g_controls.valLoopSt, 0);
    ctl_show(g_controls.capLoopEnd, 0);
    ctl_show(g_controls.valLoopEnd, 0);

    /* Settings panel controls: shown only in Settings mode (mode 2). */
    int settings = (mode == 2);
    ctl_show(g_controls.capLang, settings);
    ctl_show(g_controls.capTheme, settings);
    ctl_show(g_controls.capPalette, settings);
    ctl_show(g_controls.btnLangEn, settings);
    ctl_show(g_controls.btnLangEs, settings);
    ctl_show(g_controls.btnThemeSystem, settings);
    ctl_show(g_controls.btnThemeLight, settings);
    ctl_show(g_controls.btnThemeDark, settings);
    ctl_show(g_controls.lstPalette, settings);
}

/* A2: activates a mode. Instant apply for initial setup (create_controls),
 * without slide animation. */
void set_active_mode(int mode)
{
    if (g_active_mode == mode)
        return;
    g_active_mode = mode;

    set_active_mode_visibility(mode);
    layout();
    RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

/* B2: writes the short system message (errors and confirmations) to the
 * right of the big state text. Empty = no message. */
void set_system_message(const wchar_t *text)
{
    if (!g_controls.valSystem)
        return;
    SetWindowTextW(g_controls.valSystem, text);
    /* Without text, the gap does not look like a stray block. */
    ShowWindow(g_controls.valSystem, (text && text[0]) ? SW_SHOW : SW_HIDE);
}

/* Switches the language at runtime: saves the choice, relabels every control,
 * and refreshes the dynamic texts through update_ui(). */
static void set_language(StrLanguage lang)
{
    if (strings_language() == lang && g_settings_status == SETTINGS_OK)
        return;
    strings_set_language(lang);
    /* A scripted self-test changes the language only for its own session;
     * it must never overwrite the user's saved preference. */
    if (!g_selftest_mode && settings_save_language(lang) != 0)
        set_status(tr(STR_SETTINGS_SAVE_FAILED));
    relabel_controls();
    update_ui();
}

/* Devuelve un rect centrado dentro de `rc` escalado al 96 %. Se usa para
 * pintar los botones de transporte y velocidad "encogidos" mientras se
 * mantienen pulsados. El rect original (zona clicable) no cambia. */
static RECT shrink_rect(const RECT *rc, int pct)
{
    RECT s = *rc;
    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;
    int nw = w * pct / 100;
    int nh = h * pct / 100;
    s.left = rc->left + (w - nw) / 2;
    s.right = s.left + nw;
    s.top = rc->top + (h - nh) / 2;
    s.bottom = s.top + nh;
    return s;
}

/* Returns black or white text, whichever has the higher WCAG contrast
 * ratio against `bg`. Luminance uses linearized sRGB channels. */
static COLORREF pick_text_on(COLORREF bg)
{
    double r = (double)(GetRValue(bg)) / 255.0;
    double g = (double)(GetGValue(bg)) / 255.0;
    double b = (double)(GetBValue(bg)) / 255.0;
    double lr = r <= 0.03928 ? r / 12.92 : pow((r + 0.055) / 1.055, 2.4);
    double lg = g <= 0.03928 ? g / 12.92 : pow((g + 0.055) / 1.055, 2.4);
    double lb = b <= 0.03928 ? b / 12.92 : pow((b + 0.055) / 1.055, 2.4);
    double lum = 0.2126 * lr + 0.7152 * lg + 0.0722 * lb;
    double vs_white = 1.05 / (lum + 0.05);
    double vs_black = (lum + 0.05) / 0.05;
    return vs_white >= vs_black ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
}

/* Main window procedure. WM_CREATE builds controls + timer; WM_NOTIFY turns
 * list selection into select_track(); WM_COMMAND routes buttons; the color
 * messages tint the state label by playback state; WM_DESTROY tears down
 * the engine, GDI objects and the message loop (PostQuitMessage). */
static UINT dpi_for_window(HWND hwnd);

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        layout_init(dpi_for_window(hwnd));
        load_saved_language();
        create_controls(hwnd);
        apply_theme();
        SetTimer(hwnd, TIMER_UI, UI_TICK_MS, NULL);
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_SETTINGCHANGE:
        /* Windows app colour mode changed: re-read it when the saved choice is
         * SYSTEM, then repaint with the new palette. */
        if (lp && g_theme_choice == THEME_SYSTEM
            && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0)
            apply_theme();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        UINT dpi = dpi_for_window(hwnd);
        mm->ptMinTrackSize.x = MulDiv(760, (int)dpi, 96);
        mm->ptMinTrackSize.y = MulDiv(570, (int)dpi, 96);
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == 0)
            on_command((int)LOWORD(wp));
        else if (HIWORD(wp) == LBN_SELCHANGE && LOWORD(wp) == IDC_LST_PALETTE)
            on_palette_changed();
        return 0;

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->idFrom == IDC_LIST && nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;
            if ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED
                && (lv->uNewState & LVIS_SELECTED) && lv->iItem >= 0)
                select_track(lv->iItem);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_UI)
            update_ui();
        else if (wp == TIMER_SLIDE && g_slide_active) {
            double progress = anim_progress(&g_slide_anim, GetTickCount());
            if (progress >= 1.0)
                finish_mode_slide();
            else
                apply_slide_offsets(progress);
        }
        return 0;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        IconKind kind;
        COLORREF fg;
        COLORREF bg;
        HBRUSH face;
        if (dis->CtlType != ODT_BUTTON)
            return FALSE;
        /* Text buttons: language, theme and version pickers (Settings panel,
         * Q-121, Q-122). */
        if (dis->CtlID == IDC_BTN_LANG_EN || dis->CtlID == IDC_BTN_LANG_ES
            || dis->CtlID == IDC_BTN_THEME_SYSTEM || dis->CtlID == IDC_BTN_THEME_LIGHT
            || dis->CtlID == IDC_BTN_THEME_DARK) {
            wchar_t text[64];
            COLORREF text_fg = (dis->itemState & ODS_DISABLED)
                ? GetSysColor(COLOR_GRAYTEXT) : pick_text_on(UI_C_SUPERFICIE);
            HBRUSH text_bg = CreateSolidBrush(UI_C_SUPERFICIE);
            GetWindowTextW(dis->hwndItem, text, 64);
            FillRect(dis->hDC, &dis->rcItem, text_bg);
            DeleteObject(text_bg);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, text_fg);
            SelectObject(dis->hDC, g_font);
            DrawTextW(dis->hDC, text, -1, &dis->rcItem,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (dis->itemState & ODS_FOCUS) {
                RECT focus = dis->rcItem;
                InflateRect(&focus, -3, -3);
                DrawFocusRect(dis->hDC, &focus);
            }
            return TRUE;
        }
        /* Action buttons: open, remove, clear, speed and version use the palette surface. */
        if (dis->CtlID == IDC_BTN_OPEN || dis->CtlID == IDC_BTN_REMOVE
            || dis->CtlID == IDC_BTN_CLEAR || dis->CtlID == IDC_BTN_SPEED
            || dis->CtlID == IDC_BTN_PAIR) {
            wchar_t text[64];
            COLORREF text_fg = (dis->itemState & ODS_DISABLED)
                ? GetSysColor(COLOR_GRAYTEXT) : pick_text_on(UI_C_SUPERFICIE);
            RECT text_box = dis->rcItem;
            int pressed = dis->itemState & ODS_SELECTED;
            /* Velocidad se encoge entero (94 %: fondo, borde y texto) mientras
             * está pulsada. Su rect y zona clicable no cambian. */
            if (dis->CtlID == IDC_BTN_SPEED && pressed) {
                RECT face_rect = shrink_rect(&dis->rcItem, 94);
                FillRect(dis->hDC, &dis->rcItem, g_brushBg);
                FillRect(dis->hDC, &face_rect, g_brushFace);
                HPEN pen = CreatePen(PS_SOLID, 1, UI_C_BORDE);
                HPEN old_pen = (HPEN)SelectObject(dis->hDC, pen);
                HBRUSH old_brush = (HBRUSH)SelectObject(dis->hDC,
                    GetStockObject(NULL_BRUSH));
                Rectangle(dis->hDC, face_rect.left, face_rect.top,
                          face_rect.right, face_rect.bottom);
                SelectObject(dis->hDC, old_brush);
                SelectObject(dis->hDC, old_pen);
                DeleteObject(pen);
                text_box = face_rect;
            } else {
                HBRUSH text_bg = CreateSolidBrush(UI_C_SUPERFICIE);
                FillRect(dis->hDC, &dis->rcItem, text_bg);
                DeleteObject(text_bg);
            }
            GetWindowTextW(dis->hwndItem, text, 64);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, text_fg);
            SelectObject(dis->hDC, g_font);
            DrawTextW(dis->hDC, text, -1, &text_box,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (dis->itemState & ODS_FOCUS) {
                RECT focus = dis->rcItem;
                InflateRect(&focus, -3, -3);
                DrawFocusRect(dis->hDC, &focus);
            }
            return TRUE;
        }
        /* Mode buttons (A2): the active one is painted with accent color and
         * border (Q-078, WCAG 1.4.1); inactive ones use the surface. */
        if (dis->CtlID == IDC_BTN_MODE_LIBRARY
            || dis->CtlID == IDC_BTN_MODE_SETTINGS) {
            int active = (dis->CtlID == IDC_BTN_MODE_LIBRARY && g_active_mode == 0)
                      || (dis->CtlID == IDC_BTN_MODE_SETTINGS && g_active_mode == 2);
            HBRUSH modeFace = CreateSolidBrush(active ? UI_C_ACENTO : UI_C_SUPERFICIE);
            FillRect(dis->hDC, &dis->rcItem, modeFace);
            DeleteObject(modeFace);
            if (active) {
                HPEN pen = CreatePen(PS_SOLID, 2, UI_C_BORDE);
                HPEN old_pen = (HPEN)SelectObject(dis->hDC, pen);
                HBRUSH old_brush = (HBRUSH)SelectObject(dis->hDC,
                    GetStockObject(NULL_BRUSH));
                Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
                          dis->rcItem.right, dis->rcItem.bottom);
                SelectObject(dis->hDC, old_brush);
                SelectObject(dis->hDC, old_pen);
                DeleteObject(pen);
            }
            wchar_t text[64];
            GetWindowTextW(dis->hwndItem, text, 64);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, active ? pick_text_on(UI_C_ACENTO) : UI_C_PRIMARIO);
            SelectObject(dis->hDC, g_font);
            DrawTextW(dis->hDC, text, -1, &dis->rcItem,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (dis->itemState & ODS_FOCUS) {
                RECT focus = dis->rcItem;
                InflateRect(&focus, -3, -3);
                DrawFocusRect(dis->hDC, &focus);
            }
            return TRUE;
        }
        switch (dis->CtlID) {
        case IDC_BTN_PLAY: {
            TransportLogicalState ls = transport_logical_state(g_player_snapshot.state,
                g_player_snapshot.speed_ppm == PLAYER_FLSM_SPEED_PPM,
                g_player_snapshot.pair_sel, g_player_snapshot.pair_count);
            kind = ls.playing ? ICON_PAUSE : ICON_PLAY;
            break;
        }
        case IDC_BTN_PREVIOUS: kind = ICON_PREVIOUS; break;
        case IDC_BTN_NEXT:     kind = ICON_NEXT; break;
        default:
            return FALSE;
        }
        /* Disabled buttons are grey; the rest use the theme accent. */
        if (dis->itemState & ODS_DISABLED)
            fg = GetSysColor(COLOR_GRAYTEXT);
        else
            fg = UI_C_ACENTO;
        bg = UI_C_SUPERFICIE;
        face = CreateSolidBrush(bg);
        int pressed = dis->itemState & ODS_SELECTED;
        RECT icon_box = dis->rcItem;
        /* Anterior, Play y Siguiente se encogen enteros (94 %: fondo, borde
         * e icono) mientras están pulsados; su rect y zona clicable no cambian. */
        if (pressed) {
            RECT face_rect = shrink_rect(&dis->rcItem, 94);
            FillRect(dis->hDC, &dis->rcItem, g_brushBg);
            FillRect(dis->hDC, &face_rect, face);
            HPEN pen = CreatePen(PS_SOLID, 1, UI_C_BORDE);
            HPEN old_pen = (HPEN)SelectObject(dis->hDC, pen);
            HBRUSH old_brush = (HBRUSH)SelectObject(dis->hDC,
                GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, face_rect.left, face_rect.top,
                      face_rect.right, face_rect.bottom);
            SelectObject(dis->hDC, old_brush);
            SelectObject(dis->hDC, old_pen);
            DeleteObject(pen);
            icon_box = face_rect;
        } else {
            FillRect(dis->hDC, &dis->rcItem, face);
        }
        DeleteObject(face);
        icons_draw(dis->hDC, icon_box, kind, fg);
        if (dis->itemState & ODS_FOCUS) {
            RECT focus = dis->rcItem;
            InflateRect(&focus, -3, -3);
            DrawFocusRect(dis->hDC, &focus);
        }
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkMode(hdc, TRANSPARENT);
        /* Default: window background (UI_C_FONDO), no box. So labels,
         * read-only values, group titles and the loop checkbox do not look
         * like highlighted boxes. */
        HBRUSH brush = g_brushBg;
        SetBkColor(hdc, UI_C_FONDO);
        if (ctl == g_controls.valState) {
            PlayerState st = g_player_snapshot.state;
            COLORREF c = g_ui_theme_dark ? UI_STATE_DARK_LOADING : UI_STATE_LIGHT_LOADING;
            if (st == PLAYER_PLAYING)
                c = g_ui_theme_dark ? UI_STATE_DARK_PLAYING : UI_STATE_LIGHT_PLAYING;
            else if (st == PLAYER_PAUSED)
                c = g_ui_theme_dark ? UI_STATE_DARK_PAUSED : UI_STATE_LIGHT_PAUSED;
            else if (st == PLAYER_FINISHED)
                c = g_ui_theme_dark ? UI_STATE_DARK_FINISHED : UI_STATE_LIGHT_FINISHED;
            SetTextColor(hdc, c);
        } else if (ctl == g_controls.valSystem) {
            SetTextColor(hdc, UI_C_SECUNDARIO);
        } else {
            SetTextColor(hdc, UI_C_PRIMARIO);
        }
        /* Action and mode buttons keep their surface and border. */
        if (ctl == g_controls.btnOpen || ctl == g_controls.btnRemove
            || ctl == g_controls.btnClear || ctl == g_controls.btnPrevious
            || ctl == g_controls.btnPlay || ctl == g_controls.btnNext
            || ctl == g_controls.btnLangEn || ctl == g_controls.btnLangEs
            || ctl == g_controls.btnThemeSystem || ctl == g_controls.btnThemeLight
            || ctl == g_controls.btnThemeDark || ctl == g_controls.btnSpeed
            || ctl == g_controls.btnPair
            || ctl == g_controls.btnModeLibrary
            || ctl == g_controls.btnModeSettings) {
            brush = g_brushFace;
            SetBkColor(hdc, UI_C_SUPERFICIE);
        }
        return (LRESULT)brush;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_UI);
        player_destroy(g_player);
        g_player = NULL;
        if (g_font) {
            DeleteObject(g_font);
            g_font = NULL;
        }
        unload_ui_font();
        if (g_brushBg) {
            DeleteObject(g_brushBg);
            g_brushBg = NULL;
        }
        if (g_brushBorde) {
            DeleteObject(g_brushBorde);
            g_brushBorde = NULL;
        }
        if (g_brushFace) {
            DeleteObject(g_brushFace);
            g_brushFace = NULL;
        }
        /* The three progress brushes are process-lifetime GDI objects:
         * deleting them here keeps the handle count flat across runs. */
        if (g_brushProgFill) {
            DeleteObject(g_brushProgFill);
            g_brushProgFill = NULL;
        }
        if (g_brushProgLoop) {
            DeleteObject(g_brushProgLoop);
            g_brushProgLoop = NULL;
        }
        if (g_brushProgMarker) {
            DeleteObject(g_brushProgMarker);
            g_brushProgMarker = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ---------------------------------------------------------------- main -- */

static int cli_add_track(void *context, const wchar_t *path,
                         wchar_t *error, size_t error_length)
{
    (void)context;
    return add_track(path, error, error_length);
}

static void cli_report_error(void *context, const wchar_t *message)
{
    (void)context;
    if (wcsncmp(message, L"Path for --invalid", 18) == 0)
        set_status(message);
    else
        MessageBoxW(NULL, message, tr(STR_INVALID_FILE_TITLE),
                    MB_OK | MB_ICONWARNING);
}

/* Parse command-line options after the window exists so positional BRSTM
 * paths can be inserted into the ListView. The report remains unopened until
 * parsing finishes and only opens when --selftest was explicitly requested. */
static void load_args(int argc, wchar_t **argv)
{
    CliOptions options;
    CliParseCallbacks callbacks = {
        cli_add_track,
        cli_report_error,
        NULL
    };

    cli_options_parse(argc, argv, &options, &callbacks);
    g_selftest_mode = options.selftest;
    g_has_invalid_path = options.has_invalid_path;
    if (g_has_invalid_path)
        wcscpy_s(g_selftest_invalid_path, sizeof g_selftest_invalid_path / sizeof *g_selftest_invalid_path,
                 options.invalid_path);

    /* Narrow fprintf() on a ccs=UTF-8 stream aborts on this CRT, so the
     * report is plain text and is opened only for a selftest run. */
    if (options.selftest && options.has_report_path)
        g_selftest_report = _wfopen(options.report_path, L"w");
}

/* DPI helpers. The newer APIs are looked up at run time so the app still
 * starts on Windows versions that lack them. */
typedef BOOL (WINAPI *SetDpiContextFn)(DPI_AWARENESS_CONTEXT);
typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);

static void dpi_set_aware(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetDpiContextFn set_ctx = NULL;
    if (user32)
        set_ctx = (SetDpiContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (set_ctx)
        set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

/* DPI of the primary screen; used before any window exists. */
static UINT dpi_screen(void)
{
    HDC dc = GetDC(NULL);
    int dpi = 96;
    if (dc) {
        dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(NULL, dc);
    }
    return dpi > 0 ? (UINT)dpi : 96;
}

static UINT dpi_for_window(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    GetDpiForWindowFn get_dpi = NULL;
    if (user32)
        get_dpi = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
    if (get_dpi && hwnd) {
        UINT dpi = get_dpi(hwnd);
        if (dpi)
            return dpi;
    }
    return dpi_screen();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    wchar_t **argv;
    int argc = 0, rc = 0;
    MSG msg;

    (void)prev;
    (void)cmd;

    dpi_set_aware();
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    /* The saved theme decides the palette; a missing or invalid file means SYSTEM. */
    settings_load_theme(&g_theme_choice);
    g_ui_theme_dark = theme_is_dark_for(g_theme_choice);

    /* Saved palette, set before any brush or control reads UI_C_*. Wii Blanco
     * is the fallback, the same entry the Settings list selects first (Q-112). */
    {
        char saved_palette[128];
        settings_load_palette(saved_palette, sizeof saved_palette);
        if (!palette_load(saved_palette, "Claro", &g_palette))
            palette_load("Wii Blanco", "Claro", &g_palette);
    }

    /* Selftest output must reach the caller's console when there is one. */
    if (argc > 1 && wcscmp(argv[1], L"--selftest") == 0
        && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *f = NULL;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!wc.hIcon)
        wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    apply_theme();
    wc.hbrBackground = g_brushBg;
    wc.lpszClassName = L"BRLoopPlayer";
    if (!RegisterClassExW(&wc))
        return 1;

    load_ui_font();
    /* The window does not exist yet, so the font uses the screen DPI. */
    g_font = CreateFontW(-MulDiv(16, (int)dpi_screen(), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, UI_FONT_FACE);

    {
        WNDCLASSW wcProg;
        memset(&wcProg, 0, sizeof wcProg);
        wcProg.lpfnWndProc = progress_proc;
        wcProg.hInstance = inst;
        wcProg.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wcProg.lpszClassName = PROGRESS_CLASS_NAME;
        if (!RegisterClassW(&wcProg))
            return 1;
    }
    g_player = player_create(1);
    if (!g_player) {
        MessageBoxW(NULL, tr(STR_ENGINE_START_FAILED),
                    tr(STR_FATAL_ERROR_TITLE), MB_OK | MB_ICONERROR);
        LocalFree(argv);
        return 1;
    }

    /* Tracks are added after the window exists: add_track inserts into the
     * ListView, which is created during CreateWindowExW (WM_CREATE). */
    {
        UINT dpi = dpi_screen();
        g_hwnd = CreateWindowExW(0, L"BRLoopPlayer",
                                 tr(STR_WINDOW_TITLE),
                                 WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 MulDiv(940, (int)dpi, 96), MulDiv(520, (int)dpi, 96),
                                 NULL, NULL, inst, NULL);
    }
    if (!g_hwnd) {
        player_destroy(g_player);
        LocalFree(argv);
        return 1;
    }

    load_args(argc, argv);

    if (g_library.count > 0)
        ListView_SetItemState(g_controls.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    /* Written last: opening the first track on the command line overwrites the
     * status bar, so the notice about a bad settings file goes after it. */
    if (g_settings_status == SETTINGS_INVALID)
        set_status(tr(STR_SETTINGS_INVALID));
    if (!g_font_loaded)
        set_status(tr(STR_FONT_MISSING));

    if (g_selftest_mode) {
        UiSelfTestContext test = {0};
        test.window = g_hwnd;
        test.controls = &g_controls;
        test.player = g_player;
        test.snapshot = &g_player_snapshot;
        test.row_count = &g_library.count;
        test.selected = &g_library.selected_index;
        test.invalid_path = g_selftest_invalid_path;
        test.has_invalid_path = g_has_invalid_path;
        test.report = g_selftest_report;
        test.add_track = cli_add_track;
        test.add_track_context = NULL;
        test.library = &g_library;
        test.font_loaded = g_font_loaded;
        test.ui_font = g_font;
        rc = ui_selftest_run(&test);
        g_selftest_report = NULL;
        LocalFree(argv);
        return rc;
    }

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LocalFree(argv);
    return 0;
}
