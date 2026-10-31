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
#include <commdlg.h>
#include <psapi.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "player.h"
#include "ui_types.h"
#include "ui_selftest.h"
#include "cli_options.h"
#include "library.h"
#include "transport.h"
#include "strings.h"
#include "settings.h"
#include "icons.h"

/* ------------------------------------------------------------- app state --
 * Win32 pattern: one instance of the whole UI lives in file-scope globals
 * (single window, single player). Everything below is UI-thread-only, so no
 * lock is needed here - the only cross-thread object is g_player, which has
 * its own critical section. */
static HWND g_hwnd;         /* main window; also passed to dialogs */
static Controls g_controls;      /* every child control handle, filled once */
/* The UI font ships with the app (SIL OFL 1.1, see app/assets/fonts/OFL.txt).
 * It is loaded privately, so it works without being installed on the system. */
#define UI_FONT_FACE L"Pixelify Sans"
#define UI_FONT_FILE L"assets\\fonts\\PixelifySans.ttf"
static HFONT g_font;        /* shared UI font for all controls */
static int g_font_loaded;
static HBRUSH g_brushFace;  /* group-box background brush */
static Player *g_player;    /* engine; created in wWinMain, destroyed in
                             * WM_DESTROY (so every exit path is covered by
                             * the window lifetime) */

static LibraryState g_library = {
    NULL, 0, 0, -1
};

/* Last snapshot polled from the player (100 ms timer). The UI never reads
 * the player directly outside update_ui/select_track, so g_player_snapshot is the one
 * place "what the UI believes" lives; command handlers that must not act on
 * stale data re-query the player instead. */
static PlayerSnapshot g_player_snapshot;
static uint32_t g_last_reported_error; /* audio_error already shown in status */
static int g_decode_reported;          /* decode failure already shown once */

/* Selftest bookkeeping (--selftest). */
static int g_selftest_mode;             /* running as a scripted self-test */
static FILE *g_selftest_report;         /* optional report file (see load_args) */
static wchar_t g_selftest_invalid_path[1024];/* path of a file that must be rejected */
static int g_has_invalid_path;

/* UTF-16 path -> ANSI path for the core (fopen). Fails clearly when the
 * path cannot be represented in the system code page. */
static int narrow_path(const wchar_t *w, char *out, size_t outlen)
{
    BOOL used = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1,
                                out, (int)outlen, NULL, &used);
    return n > 0 && !used;
}

/* File name portion of a path, for display and error messages (handles both
 * separator styles; no allocation, points into `path`). */
static const wchar_t *file_name_of(const wchar_t *path)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    const wchar_t *slash2 = wcsrchr(path, L'/');
    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    return slash ? slash + 1 : path;
}

/* "0:00.000" or "1:23:04.567" (hours only when needed). Milliseconds are
 * rounded, then split largest-first so the parts always add up. */
static void format_time(wchar_t *out, size_t n, double sec)
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

/* "1,00" .. "1,06812": >=2 decimals, trailing zeros trimmed, comma. */
static void format_speed(wchar_t *out, size_t n, uint32_t ppm)
{
    wchar_t frac[16];
    uint32_t whole = ppm / PLAYER_SPEED_SCALE;
    uint32_t rem = ppm % PLAYER_SPEED_SCALE;
    int len = 5;
    swprintf(frac, 16, L"%06u", rem);
    while (len > 2 && frac[len - 1] == L'0')
        len--;
    swprintf(out, n, L"%u,%.*sÃ—", whole, len, frac);
}

/* Status bar text: every user-visible message funnels through here so the
 * selftest and the user see the same wording. */
static void set_status(const wchar_t *text)
{
    if (g_controls.status)
        SendMessageW(g_controls.status, SB_SETTEXTW, 0, (LPARAM)text);
}

/* ---------------------------------------------------------------- rows -- */

static void list_set_loop_cell(int idx, const TrackRow *row);

/* Insert the list columns' values for one library row. Called once per row
 * when added; column order must match create_controls(). */
static void list_insert_row(int idx, const TrackRow *row)
{
    LVITEMW it;
    wchar_t buf[64];
    memset(&it, 0, sizeof it);
    it.iItem = idx;
    it.pszText = (wchar_t *)row->name;
    it.mask = LVIF_TEXT;
    ListView_InsertItem(g_controls.list, &it);

    swprintf(buf, 64, L"%u", row->rate);
    ListView_SetItemText(g_controls.list, idx, 1, buf);
    swprintf(buf, 64, L"%u", row->channels);
    ListView_SetItemText(g_controls.list, idx, 2, buf);
    format_time(buf, 64, (double)row->total / row->rate);
    ListView_SetItemText(g_controls.list, idx, 3, buf);
    list_set_loop_cell(idx, row);
}

/* Writes the loop column in the current language. Called on insert and again
 * on every language change, so the column never keeps the old language. */
static void list_set_loop_cell(int idx, const TrackRow *row)
{
    /* The list view copies the text and never writes to it, so the const
     * string from the table is safe to pass through its non-const parameter. */
    ListView_SetItemText(g_controls.list, idx, 4,
                         (wchar_t *)(row->loop ? tr(STR_LOOP_YES) : tr(STR_LOOP_NO)));
}

static void list_refresh_loop_column(void)
{
    for (int i = 0; i < g_library.count; i++)
        list_set_loop_cell(i, &g_library.rows[i]);
}

/* Info panel with nothing to show: placeholders + loop control disabled, so
 * values from a previous track can never be mistaken for the new state. */
static void clear_info_panel(void)
{
    SetWindowTextW(g_controls.valName, L"â€”");
    SetWindowTextW(g_controls.valPath, L"");
    SetWindowTextW(g_controls.valRate, L"â€”");
    SetWindowTextW(g_controls.valCh, L"â€”");
    SetWindowTextW(g_controls.valSamples, L"â€”");
    SetWindowTextW(g_controls.valDur, L"â€”");
    SetWindowTextW(g_controls.valLoop, L"â€”");
    SetWindowTextW(g_controls.valLoopSt, tr(STR_LOOP_UNAVAILABLE));
    SetWindowTextW(g_controls.valLoopEnd, tr(STR_LOOP_UNAVAILABLE));
    EnableWindow(g_controls.checkLoop, FALSE);
}

/* Fill the info panel from a fresh snapshot. Also syncs the loop checkbox
 * with the engine (BM_SETCHECK here and in update_ui: the checkbox is
 * driven state, not the source of truth - player_set_loop is). */
static void fill_info_panel(const PlayerSnapshot *s)
{
    wchar_t buf[128];
    wchar_t t[40];

    swprintf(buf, 128, L"%u Hz", s->sample_rate);
    SetWindowTextW(g_controls.valRate, buf);
    swprintf(buf, 128, L"%u", s->channels);
    SetWindowTextW(g_controls.valCh, buf);
    swprintf(buf, 128, L"%llu", (unsigned long long)s->total_samples);
    SetWindowTextW(g_controls.valSamples, buf);
    format_time(t, 40, (double)s->total_samples / s->sample_rate);
    SetWindowTextW(g_controls.valDur, t);
    if (s->file_loop) {
        format_time(t, 40, (double)s->loop_start / s->sample_rate);
        swprintf(buf, 128, L"%u (%s)", s->loop_start, t);
        SetWindowTextW(g_controls.valLoopSt, buf);
        swprintf(buf, 128, tr(STR_LOOP_END_POSITION),
                 (unsigned long long)s->total_samples);
        SetWindowTextW(g_controls.valLoopEnd, buf);
        SetWindowTextW(g_controls.valLoop, tr(STR_LOOP_YES));
    } else {
        SetWindowTextW(g_controls.valLoop, tr(STR_LOOP_NO));
        SetWindowTextW(g_controls.valLoopSt, tr(STR_LOOP_UNAVAILABLE));
        SetWindowTextW(g_controls.valLoopEnd, tr(STR_LOOP_UNAVAILABLE));
    }
    EnableWindow(g_controls.checkLoop, s->file_loop != 0);
    SendMessageW(g_controls.checkLoop, BM_SETCHECK,
                 s->loop_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* Make `idx` the current track: stop/close the previous one, open the new
 * path through the player and refresh the info panel. Any failure (ANSI
 * path unrepresentable, invalid BRSTM) leaves the player closed and shows
 * the reason in the status bar - the selection still moves so the user sees
 * which row the message is about. */
static void select_track(int idx)
{
    TrackRow *row;
    char narrow[1024];
    char err[512];
    wchar_t msg[1400];
    PlayerSnapshot s;

    if (idx < 0 || idx >= g_library.count || idx == g_library.selected_index)
        return;

    /* Close the current track before selecting the new one. This keeps the
     * UI and player state aligned when opening the replacement fails. */
    player_stop(g_player);
    player_close(g_player);
    g_library.selected_index = idx;
    row = &g_library.rows[idx];

    if (!narrow_path(row->path, narrow, sizeof narrow)) {
        clear_info_panel();
        set_status(tr(STR_PATH_NOT_ANSI));
        return;
    }
    err[0] = '\0';
    if (player_open(g_player, narrow, err, sizeof err) != 0) {
        wchar_t werr[512];
        MultiByteToWideChar(CP_ACP, 0, err, -1, werr, 512);
        clear_info_panel();
        swprintf(msg, 1400, tr(STR_TRACK_OPEN_FAILED), row->name, werr);
        set_status(msg);
        return;
    }

    SetWindowTextW(g_controls.valName, row->name);
    SetWindowTextW(g_controls.valPath, row->path);
    s = player_get_state(g_player);
    /* The UI exposes individual versions only. The engine still supports
     * pair -1 for internal tests and callers that explicitly need a mix. */
    if (s.pair_count > 0 && s.pair_sel < 0) {
        if (player_set_channel_pair(g_player, 0) == 0)
            s = player_get_state(g_player);
    }
    fill_info_panel(&s);
    g_player_snapshot = s;
    g_last_reported_error = 0;
    g_decode_reported = 0;
    swprintf(msg, 1400, tr(STR_TRACK_OPENED), row->name);
    set_status(msg);
}

/* Validate a file with a full brstm_open and append it to the library.
 * Validation happens here (not on selection) so bad files are rejected the
 * moment they enter the list, with a specific message per file. The opened
 * Brstm is only read for the metadata columns and closed immediately; the
 * player opens it again on selection. Returns 0 or -1 with errbuf set. */
static int add_track(const wchar_t *path, wchar_t *errbuf, size_t errlen)
{
    char narrow[1024];
    char err[512];
    Brstm tmp;
    BrstmStatus st;
    LibraryTrackInfo info;

    if (!narrow_path(path, narrow, sizeof narrow)) {
        swprintf(errbuf, errlen, tr(STR_PATH_NOT_ANSI_NAMED),
                 file_name_of(path));
        return -1;
    }
    st = brstm_open(narrow, &tmp, err, sizeof err);
    if (st != BRSTM_OK) {
        wchar_t werr[512];
        MultiByteToWideChar(CP_ACP, 0, err, -1, werr, 512);
        swprintf(errbuf, errlen, tr(STR_TRACK_INVALID_NAMED), file_name_of(path), werr);
        return -1;
    }

    info.rate = tmp.info.sample_rate;
    info.channels = tmp.info.channels;
    info.total = tmp.info.total_samples;
    info.loop = tmp.info.loop;
    info.loop_start = tmp.info.loop_start;
    brstm_close(&tmp);

    if (library_contains_path(&g_library, path)) {
        swprintf(errbuf, errlen, tr(STR_TRACK_ALREADY_ADDED), file_name_of(path));
        return -1;
    }
    if (library_add(&g_library, path, file_name_of(path), &info) != 0) {
        swprintf(errbuf, errlen, tr(STR_TRACK_NO_MEMORY), file_name_of(path));
        return -1;
    }
    list_insert_row(g_library.count - 1, &g_library.rows[g_library.count - 1]);
    return 0;
}

/* Remove row idx: shift the array, delete the list item and repair the
 * selection. If the removed row was playing, playback stops; the next row
 * (or none) becomes the selection, keeping g_library.selected_index always valid. */
static void remove_track(int idx)
{
    int was_selected;
    int replacement;

    if (idx < 0 || idx >= g_library.count)
        return;
    was_selected = idx == g_library.selected_index;
    replacement = library_remove(&g_library, idx);
    ListView_DeleteItem(g_controls.list, idx);

    if (was_selected) {
        player_stop(g_player);
        player_close(g_player);
        clear_info_panel();
        if (replacement >= 0) {
            /* Select and open synchronously: waiting for LVN_ITEMCHANGED would
             * leave the player closed until the message loop runs again. The
             * list selection is then kept in step with the library state. */
            ListView_SetItemState(g_controls.list, replacement,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            g_library.selected_index = -1;
            select_track(replacement);
        }
    }
}

/* "Limpiar": stop playback, free every row's strings and empty the list.
 * The g_library.rows buffer itself is kept (capacity reuse across clears; it is
 * process-lifetime state like the rest of the globals and is reclaimed by
 * the OS at exit). */
static void remove_all(void)
{
    player_stop(g_player);
    player_close(g_player);
    library_clear(&g_library);
    ListView_DeleteAllItems(g_controls.list);
    clear_info_panel();
    set_status(tr(STR_LIST_EMPTY));
}

/* ----------------------------------------------------------- file open -- */

/* File dialog (multi-select allowed). The OPENFILENAME buffer uses the
 * documented two-part layout: dir\0 then file\0 file\0 ... \0 (single file
 * = just the path). Every added file is validated by add_track; failures
 * are reported once, with a count, instead of one box per file. */
static void browse_files(void)
{
    static wchar_t buf[65536];
    static wchar_t dir[33000];
    static wchar_t full[33000];
    OPENFILENAMEW ofn;
    wchar_t err[1024], msg[1400], sum[128];
    int added = 0, failed = 0;

    memset(&ofn, 0, sizeof ofn);
    memset(buf, 0, sizeof buf);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    /* File-dialog filter: each name is followed by its pattern, and the list
     * ends with a double NUL, so the pieces are joined by hand. */
    {
        wchar_t filter[256];
        size_t fi = 0;
        const wchar_t *parts[4] = { tr(STR_FILTER_BRSTM), L"*.brstm", tr(STR_FILTER_ALL), L"*.*" };
        int pi;
        for (pi = 0; pi < 4; pi++) {
            size_t len = wcslen(parts[pi]);
            memcpy(filter + fi, parts[pi], (len + 1) * sizeof(wchar_t));
            fi += len + 1;
        }
        filter[fi] = L'\0';
        ofn.lpstrFilter = filter;
    }
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 65536;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER
              | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn))
        return;

    msg[0] = L'\0';
    wchar_t *p = buf;
    if (p[0] && p[wcslen(p) + 1] != L'\0') {
        wcscpy_s(dir, 33000, p);
        p += wcslen(p) + 1;
        while (*p) {
            swprintf(full, 33000, L"%s\\%s", dir, p);
            if (add_track(full, err, 1024) == 0)
                added++;
            else {
                if (failed == 0)
                    wcscpy_s(msg, 1400, err);
                failed++;
            }
            p += wcslen(p) + 1;
        }
    } else {
        if (add_track(buf, err, 1024) == 0)
            added++;
        else {
            wcscpy_s(msg, 1400, err);
            failed++;
        }
    }

    if (failed > 0) {
        wchar_t box[2048];
        if (failed > 1)
            swprintf(box, 2048, tr(STR_INVALID_FILES_MORE), msg, failed - 1);
        else
            swprintf(box, 2048, L"%s", msg);
        MessageBoxW(g_hwnd, box, tr(STR_INVALID_FILE_TITLE),
                    MB_OK | MB_ICONWARNING);
    }
    if (added > 0 && g_library.selected_index < 0 && g_library.count > 0)
        ListView_SetItemState(g_controls.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    if (added > 0) {
        swprintf(sum, 128, tr(STR_LIST_COUNT), g_library.count);
        set_status(sum);
    }
}

/* ------------------------------------------------- progress with loop ---- */

/* Custom progress bar: the loop section [loop_start, total) is shaded and a
 * marker line shows the loop start, so both loop points are visible while
 * the song plays. Paint state is UI-thread-only (set in update_ui). */

static uint64_t g_progress_position;
static uint64_t g_progress_total;
static uint32_t g_progress_loop_start;
static int      g_progress_has_loop;
static HBRUSH   g_brushProgFill;   /* played portion */
static HBRUSH   g_brushProgLoop;   /* loop region */
static HBRUSH   g_brushProgMarker; /* loop start line */

static void update_ui(void);
static void set_language(StrLanguage lang);
static void show_language_menu(HWND owner);

/* Seek: click (or drag end) on the bar jumps to that source position. */
static int g_progress_dragging;
static int g_progress_drag_x;

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
        FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
        FrameRect(dc, &rc, GetSysColorBrush(COLOR_3DSHADOW));
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
            FillRect(dc, &dr, GetSysColorBrush(COLOR_3DDKSHADOW));
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

/* -------------------------------------------------------------- layout -- */

/* Absolute position/size of one control; no-op for a not-yet-created handle
 * so layout() is safe to call before every control exists. */
static void move_ctl(HWND h, int x, int y, int w, int hh)
{
    if (h)
        SetWindowPos(h, NULL, x, y, w, hh,
                     SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Recompute every control position from the client size. Called on
 * WM_SIZE and once after creation. Fixed pixel geometry (no auto-scale):
 * the window enforces a 760x504 minimum in WM_GETMINMAXINFO, and the
 * layout is simple enough that proportional scaling would add complexity
 * without visible benefit at this stage. Left column = library (width
 * clamped 330..440), right column = info / transport / speed panels. */
static void layout(void)
{
    RECT rc;
    int cw, ch;
    int m = 12, g = 10, statusH = 24;
    int leftW, rightX, rightW;
    int infoH = 178, playH = 110, speedH = 100;
    int x0, col2, y;

    if (!g_hwnd)
        return;
    GetClientRect(g_hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw < 100 || ch < 100)
        return;

    SendMessageW(g_controls.status, WM_SIZE, 0, 0);

    leftW = cw * 46 / 100;
    if (leftW < 330) leftW = 330;
    if (leftW > 440) leftW = 440;
    rightX = m + leftW + g;
    rightW = cw - rightX - m;
    x0 = rightX + 12;
    col2 = rightX + rightW / 2 + 2;

    /* Library (left column). */
    move_ctl(g_controls.groupLib, m, m, leftW, ch - statusH - 2 * m);
    move_ctl(g_controls.btnOpen, m + 12, m + 22, 96, 26);
    move_ctl(g_controls.btnRemove, m + 116, m + 22, 84, 26);
    move_ctl(g_controls.btnClear, m + 208, m + 22, 84, 26);
    move_ctl(g_controls.list, m + 12, m + 56, leftW - 24,
             ch - statusH - 2 * m - 56 - 12);

    /* Information panel. */
    move_ctl(g_controls.groupInfo, rightX, m, rightW, infoH);
    y = m + 22;
    move_ctl(g_controls.capName, x0, y, 84, 18);
    move_ctl(g_controls.valName, x0 + 88, y, rightW / 2 - 104, 18);
    move_ctl(g_controls.capCh, col2, y, 74, 18);
    move_ctl(g_controls.valCh, col2 + 78, y, rightW - (col2 - rightX) - 90, 18);
    y += 24;
    move_ctl(g_controls.capRate, x0, y, 84, 18);
    move_ctl(g_controls.valRate, x0 + 88, y, rightW / 2 - 104, 18);
    move_ctl(g_controls.capDur, col2, y, 74, 18);
    move_ctl(g_controls.valDur, col2 + 78, y, rightW - (col2 - rightX) - 90, 18);
    y += 24;
    move_ctl(g_controls.capSamples, x0, y, 84, 18);
    move_ctl(g_controls.valSamples, x0 + 88, y, rightW / 2 - 104, 18);
    move_ctl(g_controls.capLoop, col2, y, 74, 18);
    move_ctl(g_controls.valLoop, col2 + 78, y, rightW - (col2 - rightX) - 90, 18);
    y += 24;
    move_ctl(g_controls.capLoopSt, x0, y, 84, 18);
    move_ctl(g_controls.valLoopSt, x0 + 88, y, col2 - (x0 + 88) - 12, 18);
    move_ctl(g_controls.capLoopEnd, col2, y, 74, 18);
    move_ctl(g_controls.valLoopEnd, col2 + 78, y, rightW - (col2 - rightX) - 90,
             18);
    y += 24;
    move_ctl(g_controls.capVer, x0, y, 84, 18);
    move_ctl(g_controls.btnPair, x0 + 88, y - 2, rightW - 104, 22);
    y += 26;
    move_ctl(g_controls.capPath, x0, y, 84, 18);
    move_ctl(g_controls.valPath, x0 + 88, y, rightW - 104, 20);

    /* Transport: one play/pause button (96+ px so "Reproducir" fits),
     * previous-track and next-track beside it. */
    y = m + infoH + g;
    move_ctl(g_controls.groupPlay, rightX, y, rightW, playH);
    move_ctl(g_controls.btnPrevious, rightX + 12, y + 22, 84, 28);
    move_ctl(g_controls.btnPlay, rightX + 108, y + 22, 104, 28);
    move_ctl(g_controls.btnNext, rightX + 224, y + 22, 96, 28);
    move_ctl(g_controls.btnLang, rightX + rightW - 12 - 40, y + 22, 40, 28);
    move_ctl(g_controls.progress, rightX + 12, y + 58, rightW - 24 - 176, 16);
    move_ctl(g_controls.valState, rightX + rightW - 12 - 164, y + 56, 164, 20);
    move_ctl(g_controls.valTime, rightX + 12, y + 80, 170, 18);
    move_ctl(g_controls.checkLoop, rightX + rightW - 12 - 170, y + 78, 170, 20);

    /* Speed: a single toggle button showing the mode in force. */
    y = m + infoH + g + playH + g;
    move_ctl(g_controls.groupSpeed, rightX, y, rightW, speedH);
    move_ctl(g_controls.valSpeed, rightX + 12, y + 28, 84, 20);
    move_ctl(g_controls.valDevRate, rightX + rightW - 12 - 170, y + 28, 170, 20);
    move_ctl(g_controls.btnSpeed, rightX + 12, y + 60, 200, 26);
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
                              BS_GROUPBOX, 0, 0, 0, 0, 0);
    g_controls.btnOpen = make_ctl(parent, 0, L"BUTTON", tr(STR_BUTTON_OPEN),
                             0, IDC_BTN_OPEN, 0, 0, 0, 0);
    g_controls.btnRemove = make_ctl(parent, 0, L"BUTTON", tr(STR_BUTTON_REMOVE),
                               0, IDC_BTN_REMOVE, 0, 0, 0, 0);
    g_controls.btnClear = make_ctl(parent, 0, L"BUTTON", tr(STR_BUTTON_CLEAR),
                              0, IDC_BTN_CLEAR, 0, 0, 0, 0);
    g_controls.list = make_ctl(parent, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SHOWSELALWAYS, IDC_LIST,
                          0, 0, 0, 0);
    ListView_SetExtendedListViewStyle(g_controls.list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

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
                               0, 0, 0, 0, 0);
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
    g_controls.valName = make_ctl(parent, 0, L"STATIC", L"â€”", 0,
                             IDC_VAL_NAME, 0, 0, 0, 0);
    g_controls.valPath = make_ctl(parent, WS_EX_CLIENTEDGE, L"EDIT", L"",
                             ES_READONLY | ES_AUTOHSCROLL, IDC_VAL_PATH,
                             0, 0, 0, 0);
    g_controls.valRate = make_ctl(parent, 0, L"STATIC", L"â€”", 0,
                             IDC_VAL_RATE, 0, 0, 0, 0);
    g_controls.valCh = make_ctl(parent, 0, L"STATIC", L"â€”", 0,
                           IDC_VAL_CH, 0, 0, 0, 0);
    g_controls.valSamples = make_ctl(parent, 0, L"STATIC", L"â€”", 0,
                                IDC_VAL_SAMPLES, 0, 0, 0, 0);
    g_controls.valDur = make_ctl(parent, 0, L"STATIC", L"â€”", 0,
                            IDC_VAL_DUR, 0, 0, 0, 0);
    g_controls.valLoop = make_ctl(parent, 0, L"STATIC", L"â€”", 0,
                             IDC_VAL_LOOP, 0, 0, 0, 0);
    g_controls.valLoopSt = make_ctl(parent, 0, L"STATIC", tr(STR_LOOP_UNAVAILABLE), 0,
                               IDC_VAL_LOOPST, 0, 0, 0, 0);
    g_controls.valLoopEnd = make_ctl(parent, 0, L"STATIC", tr(STR_LOOP_UNAVAILABLE), 0,
                                IDC_VAL_LOOPEND, 0, 0, 0, 0);
    g_controls.capVer = make_ctl(parent, 0, L"STATIC", tr(STR_CAP_VERSION), 0,
                            IDC_CAP_VER, 0, 0, 0, 0);
    g_controls.btnPair = make_ctl(parent, 0, L"BUTTON", L"â€”", 0,
                             IDC_BTN_PAIR, 0, 0, 0, 0);

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
    g_controls.btnLang = make_ctl(parent, 0, L"BUTTON", tr(STR_MENU_LANGUAGE),
                             BS_OWNERDRAW, IDC_BTN_LANG, 0, 0, 0, 0);
    g_controls.progress = make_ctl(parent, 0, PROGRESS_CLASS_NAME, L"",
                              0, IDC_PROGRESS, 0, 0, 0, 0);
    g_controls.valState = make_ctl(parent, 0, L"STATIC", L"Estado: Detenido",
                              SS_RIGHT, IDC_VAL_STATE, 0, 0, 0, 0);
    g_controls.valTime = make_ctl(parent, 0, L"STATIC", L"0:00.000 / 0:00.000",
                             0, IDC_VAL_TIME, 0, 0, 0, 0);
    g_controls.checkLoop = make_ctl(parent, 0, L"BUTTON", tr(STR_CHECK_LOOP),
                               BS_AUTOCHECKBOX | BS_LEFT, IDC_CHECK_LOOP,
                               0, 0, 0, 0);

    g_controls.groupSpeed = make_ctl(parent, 0, L"BUTTON", tr(STR_SPEED_GROUP),
                                BS_GROUPBOX, 0, 0, 0, 0, 0);
    g_controls.valSpeed = make_ctl(parent, 0, L"STATIC", L"1,00Ã—", SS_RIGHT,
                              IDC_VAL_SPEED, 0, 0, 0, 0);
    g_controls.valDevRate = make_ctl(parent, 0, L"STATIC", L"Dispositivo: â€”",
                                SS_RIGHT, IDC_VAL_DEVRATE, 0, 0, 0, 0);
    g_controls.btnSpeed = make_ctl(parent, 0, L"BUTTON", L"1,0Ã—", 0,
                              IDC_BTN_SPEED, 0, 0, 0, 0);

    g_controls.status = CreateWindowExW(0, STATUSCLASSNAMEW, tr(STR_STATUS_READY),
                                   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, parent,
                                   (HMENU)(INT_PTR)2, GetModuleHandleW(NULL),
                                   NULL);
    SendMessageW(g_controls.status, WM_SETFONT, (WPARAM)g_font, TRUE);

    layout();
    EnableWindow(g_controls.btnPlay, FALSE);
    EnableWindow(g_controls.btnPrevious, FALSE);
    EnableWindow(g_controls.btnNext, FALSE);
    EnableWindow(g_controls.btnRemove, FALSE);
    EnableWindow(g_controls.btnClear, FALSE);
    EnableWindow(g_controls.checkLoop, FALSE);
    EnableWindow(g_controls.btnPair, FALSE);
}

/* ----------------------------------------------------------------- UI --- */

/* Apply a speed button: send it to the engine and update the label at once
 * (the 100 ms timer would catch up anyway, but immediate feedback is what a
 * click should feel like). Only two fixed speeds exist by design: 1,0x and
 * the observed FLSM factor (AGENTS/requirements: no arbitrary slider). */
static void apply_speed(uint32_t ppm)
{
    wchar_t buf[40];
    player_set_speed(g_player, ppm);
    format_speed(buf, 40, ppm);
    SetWindowTextW(g_controls.valSpeed, buf);
}

/* One full UI refresh from a fresh snapshot; runs every UI_TICK_MS and
 * after every command. Deliberately idempotent: SetWindowText is only called
 * when the text actually changed, so the 10 Hz polling does not churn
 * redraws or fight the user. Sections: state text, button enables, version
 * (pair) caption, loop checkbox, device/speed labels, progress repaint, time
 * display, one-shot error messages. */
static void update_ui(void)
{
    PlayerSnapshot s = player_get_state(g_player);
    const wchar_t *stateText;
    wchar_t buf[160], t1[40], t2[40];
    double devRate;

    g_player_snapshot = s;

    switch (s.state) {
    case PLAYER_PLAYING:
        stateText = tr(STR_STATE_PLAYING);
        break;
    case PLAYER_PAUSED:
        stateText = tr(STR_STATE_PAUSED);
        break;
    case PLAYER_FINISHED:
        stateText = tr(STR_STATE_FINISHED);
        break;
    default:
        stateText = tr(STR_STATE_STOPPED);
        break;
    }
    {
        wchar_t cur[64];
        GetWindowTextW(g_controls.valState, cur, 64);
        if (wcscmp(cur, stateText) != 0)
            SetWindowTextW(g_controls.valState, stateText);
    }

    /* The single transport button announces what the NEXT click does. */
    {
        const wchar_t *playText;
        wchar_t curPlay[32];
        TransportLogicalState ls = transport_logical_state(s.state, s.speed_ppm == PLAYER_FLSM_SPEED_PPM,
                                                           s.pair_sel, s.pair_count);
        if (ls.playing)
            playText = tr(STR_BUTTON_PAUSE);
        else if (ls.paused)
            playText = tr(STR_BUTTON_RESUME);
        else if (ls.finished)
            playText = tr(STR_BUTTON_REPLAY);
        else
            playText = tr(STR_BUTTON_PLAY);
        GetWindowTextW(g_controls.btnPlay, curPlay, 32);
        if (wcscmp(curPlay, playText) != 0)
            SetWindowTextW(g_controls.btnPlay, playText);
    }
    EnableWindow(g_controls.btnPlay, g_library.selected_index >= 0);
    EnableWindow(g_controls.btnPrevious, g_library.selected_index >= 0 && s.total_samples > 0);
    EnableWindow(g_controls.btnNext, g_library.selected_index >= 0 && g_library.count > 1);
    EnableWindow(g_controls.btnRemove, g_library.count > 0);
    EnableWindow(g_controls.btnClear, g_library.count > 0);

    /* Speed button: caption is the mode in force (like the version button). */
    {
        wchar_t speedText[64], cur[64];
        TransportLogicalState speedState = transport_logical_state(s.state,
            s.speed_ppm == PLAYER_FLSM_SPEED_PPM, s.pair_sel, s.pair_count);
        if (speedState.speed_flsm)
            wcscpy(speedText, tr(STR_SPEED_FLSM));
        else if (s.speed_ppm == PLAYER_SPEED_SCALE)
            wcscpy(speedText, tr(STR_SPEED_NORMAL));
        else {
            format_speed(buf, 40, s.speed_ppm);
            swprintf(speedText, 64, L"%s", buf);
        }
        GetWindowTextW(g_controls.btnSpeed, cur, 64);
        if (wcscmp(cur, speedText) != 0)
            SetWindowTextW(g_controls.btnSpeed, speedText);
    }

    /* Version button: its caption is the current selection. */
    {
        wchar_t pairText[64], cur[64];
        const wchar_t *want;
        if (s.channels == 0) {
            want = tr(STR_NO_TRACK_VALUE);
        } else if (s.pair_count == 0) {
            want = tr(STR_VERSION_STEREO);
        } else {
            int pair = s.pair_sel < 0 ? 0 : s.pair_sel;
            swprintf(pairText, 64, tr(STR_VERSION_CAPTION),
                     pair + 1, pair * 2 + 1, pair * 2 + 2);
            want = pairText;
        }
        GetWindowTextW(g_controls.btnPair, cur, 64);
        if (wcscmp(cur, want) != 0)
            SetWindowTextW(g_controls.btnPair, want);
        EnableWindow(g_controls.btnPair, s.pair_count > 0);
    }

    if (s.file_loop)
        SendMessageW(g_controls.checkLoop, BM_SETCHECK,
                     s.loop_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    devRate = s.sample_rate ? (double)s.device_rate : 0.0;
    if (s.sample_rate) {
        if (s.audio_folded || s.pair_sel >= 0)
            swprintf(buf, 160, tr(STR_DEVICE_RATE_STEREO), s.device_rate);
        else
            swprintf(buf, 160, tr(STR_DEVICE_RATE), s.device_rate);
        SetWindowTextW(g_controls.valDevRate, buf);
        format_speed(buf, 40, s.speed_ppm);
        SetWindowTextW(g_controls.valSpeed, buf);
    } else {
        SetWindowTextW(g_controls.valDevRate, tr(STR_DEVICE_RATE_EMPTY));
    }

    if (g_progress_position != s.position || g_progress_total != s.total_samples ||
        g_progress_has_loop != s.file_loop ||
        g_progress_loop_start != s.loop_start) {
        g_progress_position = s.position;
        g_progress_total = s.total_samples;
        g_progress_has_loop = s.file_loop;
        g_progress_loop_start = s.loop_start;
        InvalidateRect(g_controls.progress, NULL, FALSE);
    }
    if (s.total_samples > 0) {
        format_time(t1, 40, devRate > 0 ? (double)s.position / devRate : 0.0);
        format_time(t2, 40, devRate > 0
                          ? (double)s.total_samples / devRate : 0.0);
        swprintf(buf, 160, L"%s / %s", t1, t2);
        SetWindowTextW(g_controls.valTime, buf);
    } else {
        SetWindowTextW(g_controls.valTime, L"0:00.000 / 0:00.000");
    }

    if (s.audio_error != 0 && s.audio_error != g_last_reported_error) {
        wchar_t werr[256], msg[320];
        waveOutGetErrorTextW(s.audio_error, werr, 256);
        swprintf(msg, 320, tr(STR_AUDIO_OUTPUT_ERROR), werr);
        set_status(msg);
        g_last_reported_error = s.audio_error;
    }
    if (s.decode_failed && !g_decode_reported) {
        set_status(tr(STR_STATE_DECODE_ERROR));
        g_decode_reported = 1;
    }
}

/* Move the selection to `idx` through the same handler a real click uses
 * and resume playback if the user was listening - the shared body of the
 * previous/next transport buttons. */
static void change_track(int idx, int wasPlaying)
{
    ListView_SetItemState(g_controls.list, idx, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    if (g_library.selected_index != idx)
        select_track(idx); /* no LVN_ITEMCHANGED when the row does not change */
    if (wasPlaying && player_get_state(g_player).state != PLAYER_PLAYING)
        player_play(g_player);
}

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
    case IDC_BTN_LANG:
        show_language_menu(g_hwnd);
        break;
    case IDM_LANG_EN:
        set_language(STR_LANG_EN);
        break;
    case IDM_LANG_ES:
        set_language(STR_LANG_ES);
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

/* Language picker opened from the globe button. Built on each click so its
 * labels and the marked language always match the language in force. */
static void show_language_menu(HWND owner)
{
    POINT pt;
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, IDM_LANG_EN, tr(STR_MENU_ENGLISH));
    AppendMenuW(menu, MF_STRING, IDM_LANG_ES, tr(STR_MENU_SPANISH));
    CheckMenuRadioItem(menu, IDM_LANG_EN, IDM_LANG_ES,
                       strings_language() == STR_LANG_ES ? IDM_LANG_ES : IDM_LANG_EN,
                       MF_BYCOMMAND);
    GetCursorPos(&pt);
    SetForegroundWindow(owner);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, NULL);
    DestroyMenu(menu);
}

/* Writes the static caption of every control that create_controls() labels
 * with tr(). Controls whose text update_ui() owns (state, speed, version,
 * device, time) are not touched here: update_ui() rewrites them on the next
 * tick, and the cache it keeps is cleared below so that rewrite happens. */
static void relabel_controls(void)
{
    SetWindowTextW(g_controls.groupLib, tr(STR_LIBRARY_GROUP));
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
    SetWindowTextW(g_controls.capVer, tr(STR_CAP_VERSION));
    list_refresh_loop_column();
    SetWindowTextW(g_controls.valLoopSt, tr(STR_LOOP_UNAVAILABLE));
    SetWindowTextW(g_controls.valLoopEnd, tr(STR_LOOP_UNAVAILABLE));
    SetWindowTextW(g_controls.groupPlay, tr(STR_PLAY_GROUP));
    SetWindowTextW(g_controls.btnPrevious, tr(STR_BUTTON_PREVIOUS));
    SetWindowTextW(g_controls.btnNext, tr(STR_BUTTON_NEXT));
    SetWindowTextW(g_controls.btnLang, tr(STR_MENU_LANGUAGE));
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
            col.pszText = (wchar_t *)names[i];
            ListView_SetColumn(g_controls.list, i, &col);
        }
    }
    /* Force a rewrite of the texts update_ui() owns, instead of waiting for a
     * change that may never come (the cache there compares against the last
     * value written, which is now in the old language). */
    SetWindowTextW(g_controls.valState, L"");
    SetWindowTextW(g_controls.btnPlay, L"");
    SetWindowTextW(g_controls.btnPair, L"");
    SetWindowTextW(g_controls.btnSpeed, L"");
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

/* Main window procedure. WM_CREATE builds controls + timer; WM_NOTIFY turns
 * list selection into select_track(); WM_COMMAND routes buttons; the color
 * messages tint the state label by playback state; WM_DESTROY tears down
 * the engine, GDI objects and the message loop (PostQuitMessage). */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        load_saved_language();
        create_controls(hwnd);
        SetTimer(hwnd, TIMER_UI, UI_TICK_MS, NULL);
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = 760;
        mm->ptMinTrackSize.y = 504;
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == 0)
            on_command((int)LOWORD(wp));
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
        return 0;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        IconKind kind;
        COLORREF fg;
        COLORREF bg;
        HBRUSH face;
        if (dis->CtlType != ODT_BUTTON)
            return FALSE;
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
        case IDC_BTN_LANG:     kind = ICON_LANGUAGE; break;
        default:
            return FALSE;
        }
        /* Disabled buttons are drawn grey; pressed and normal use the face
         * colour, so the glyph keeps its contrast in every state. */
        if (dis->itemState & ODS_DISABLED)
            fg = GetSysColor(COLOR_GRAYTEXT);
        else
            fg = GetSysColor(COLOR_BTNTEXT);
        bg = GetSysColor(COLOR_BTNFACE);
        face = CreateSolidBrush(bg);
        FillRect(dis->hDC, &dis->rcItem, face);
        DeleteObject(face);
        icons_draw(dis->hDC, dis->rcItem, kind, fg);
        if (dis->itemState & ODS_FOCUS) {
            RECT focus = dis->rcItem;
            InflateRect(&focus, -3, -3);
            DrawFocusRect(dis->hDC, &focus);
        }
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkMode(hdc, TRANSPARENT);
        if (ctl == g_controls.valState) {
            PlayerState st = g_player_snapshot.state;
            COLORREF c = RGB(96, 96, 96);
            if (st == PLAYER_PLAYING) c = RGB(27, 124, 52);
            else if (st == PLAYER_PAUSED) c = RGB(191, 108, 0);
            else if (st == PLAYER_FINISHED) c = RGB(21, 101, 192);
            SetTextColor(hdc, c);
        } else {
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }
        return (LRESULT)g_brushFace;
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
    if (wcsncmp(message, L"Ruta de --invalid", 18) == 0)
        set_status(message);
    else
        MessageBoxW(NULL, message, L"Archivo no vÃ¡lido o ilegible",
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

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    wchar_t **argv;
    int argc = 0, rc = 0;
    MSG msg;

    (void)prev;
    (void)cmd;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);

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
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"BRLoopPlayer";
    if (!RegisterClassExW(&wc))
        return 1;

    load_ui_font();
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, UI_FONT_FACE);
    g_brushFace = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));

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
    g_brushProgFill = CreateSolidBrush(RGB(70, 140, 220));
    g_brushProgLoop = CreateSolidBrush(RGB(255, 236, 200));
    g_brushProgMarker = CreateSolidBrush(RGB(214, 92, 24));

    g_player = player_create(1);
    if (!g_player) {
        MessageBoxW(NULL, tr(STR_ENGINE_START_FAILED),
                    tr(STR_FATAL_ERROR_TITLE), MB_OK | MB_ICONERROR);
        LocalFree(argv);
        return 1;
    }

    /* Tracks are added after the window exists: add_track inserts into the
     * ListView, which is created during CreateWindowExW (WM_CREATE). */
    g_hwnd = CreateWindowExW(0, L"BRLoopPlayer",
                             tr(STR_WINDOW_TITLE),
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 940, 520,
                             NULL, NULL, inst, NULL);
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
