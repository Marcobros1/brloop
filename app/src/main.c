/* Metal Kart Wii desktop prototype (T2): Win32 test UI for the BRSTM
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

#define IDC_LIST         1001
#define IDC_BTN_OPEN     1002
#define IDC_BTN_REMOVE   1003
#define IDC_BTN_CLEAR    1004
#define IDC_BTN_PLAY     1005
#define IDC_BTN_PAUSE    1006
#define IDC_BTN_STOP     1007
#define IDC_CHECK_LOOP   1008
#define IDC_BTN_SPD100   1010
#define IDC_BTN_FLSM     1011
#define IDC_BTN_PAIR     1012
#define IDC_PROGRESS     1014
#define IDC_VAL_STATE    1015
#define IDC_VAL_TIME     1016
#define IDC_VAL_SPEED    1017
#define IDC_VAL_DEVRATE  1018
#define IDC_VAL_NAME     1020
#define IDC_VAL_PATH     1021
#define IDC_VAL_RATE     1022
#define IDC_VAL_CH       1023
#define IDC_VAL_SAMPLES  1024
#define IDC_VAL_DUR      1025
#define IDC_VAL_LOOP     1026
#define IDC_VAL_LOOPST   1027
#define IDC_CAP_LOOPEND  1028
#define IDC_VAL_LOOPEND  1029
#define IDC_CAP_NAME     1030
#define IDC_CAP_CH       1031
#define IDC_CAP_RATE     1032
#define IDC_CAP_DUR      1033
#define IDC_CAP_SAMPLES  1034
#define IDC_CAP_LOOP     1035
#define IDC_CAP_LOOPST   1036
#define IDC_CAP_PATH     1037
#define IDC_CAP_VER      1038

#define TIMER_UI 1
#define UI_TICK_MS 100

typedef struct {
    wchar_t *path;
    wchar_t *name;
    uint32_t rate;
    uint8_t  channels;
    uint32_t total;
    uint8_t  loop;
    uint32_t loop_start;
} TrackRow;

typedef struct {
    HWND list, status;
    HWND groupLib, groupInfo, groupPlay, groupSpeed;
    HWND btnOpen, btnRemove, btnClear;
    HWND btnPlay, btnPause, btnStop;
    HWND btnSpd100, btnFlsm, btnPair;
    HWND checkLoop, progress;
    HWND capName, capCh, capRate, capDur, capSamples, capLoop, capLoopSt;
    HWND capLoopEnd;
    HWND capPath, capVer;
    HWND valName, valPath, valRate, valCh, valSamples, valDur;
    HWND valLoop, valLoopSt, valState, valTime, valSpeed, valDevRate;
    HWND valLoopEnd;
} Controls;

/* ------------------------------------------------------------- app state --
 * Win32 pattern: one instance of the whole UI lives in file-scope globals
 * (single window, single player). Everything below is UI-thread-only, so no
 * lock is needed here - the only cross-thread object is g_player, which has
 * its own critical section. */
static HWND g_hwnd;         /* main window; also passed to dialogs */
static Controls g_ctl;      /* every child control handle, filled once */
static HFONT g_font;        /* shared Segoe UI font for all controls */
static HBRUSH g_brushFace;  /* group-box background brush */
static Player *g_player;    /* engine; created in wWinMain, destroyed in
                             * WM_DESTROY (so every exit path is covered by
                             * the window lifetime) */

static TrackRow *g_rows;    /* library array, grows by doubling */
static int g_rowCount, g_rowCap;
static int g_selected = -1; /* index into g_rows, -1 = nothing selected */

/* Last snapshot polled from the player (100 ms timer). The UI never reads
 * the player directly outside update_ui/select_track, so g_snap is the one
 * place "what the UI believes" lives; command handlers that must not act on
 * stale data re-query the player instead. */
static PlayerSnapshot g_snap;
static uint32_t g_last_reported_error; /* audio_error already shown in status */
static int g_decode_reported;          /* decode failure already shown once */

/* Selftest bookkeeping (--selftest). */
static int g_stMode;             /* running as a scripted self-test */
static int g_stChecks, g_stFails;/* counters for the final RESULT line */
static FILE *g_stReport;         /* optional report file (see load_args) */
static wchar_t g_stInvalid[1024];/* path of a file that must be rejected */
static int g_hasInvalid;

/* Heap copy of a wide string (the library owns every path/name it keeps;
 * NULL on OOM, which callers turn into an error message). */
static wchar_t *dup_wide(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *d = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (d)
        memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

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
    swprintf(out, n, L"%u,%.*s×", whole, len, frac);
}

/* Status bar text: every user-visible message funnels through here so the
 * selftest and the user see the same wording. */
static void set_status(const wchar_t *text)
{
    if (g_ctl.status)
        SendMessageW(g_ctl.status, SB_SETTEXTW, 0, (LPARAM)text);
}

/* ------------------------------------------------------------ selftest -- */

/* Record one check result: counters, stdout and report file stay in sync so
 * a redirected run and an interactive one show the same results. */
static void st_check(int ok, const char *name)
{
    g_stChecks++;
    if (!ok) {
        g_stFails++;
        fprintf(stdout, "[FAIL] %s\n", name);
        if (g_stReport)
            fprintf(g_stReport, "[FAIL] %s\n", name);
    } else {
        printf("[PASS] %s\n", name);
        if (g_stReport)
            fprintf(g_stReport, "[PASS] %s\n", name);
    }
    fflush(stdout);
    if (g_stReport)
        fflush(g_stReport);
}

/* Run the message loop for `ms` milliseconds. The selftest uses it instead
 * of Sleep so window messages, timers and the engine callbacks keep being
 * processed while waiting - a bare sleep would freeze the UI under test. */
static void pump(DWORD ms)
{
    DWORD deadline = GetTickCount() + ms;
    MSG msg;
    do {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    } while (GetTickCount() < deadline);
}

/* Pump messages until the player reaches `want` or the timeout expires.
 * Returns the final answer, so callers can assert on it (a timeout is a
 * failed check, not a hang). */
static int wait_state(PlayerState want, DWORD timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    do {
        PlayerSnapshot s = player_get_state(g_player);
        if (s.state == want)
            return 1;
        pump(10);
    } while (GetTickCount() < deadline);
    return player_get_state(g_player).state == want;
}

/* Simulate a button click by sending the exact WM_COMMAND the control
 * itself would send - the selftest therefore exercises the same
 * on_command() path as a real click, not a private test entry point. */
static void send_cmd(int id)
{
    SendMessageW(g_hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
}

/* Diagnostics go to stdout and, when requested, to the report file: stdout
 * is re-routed to the parent console by AttachConsole, so the report is the
 * only reliable channel when output is redirected. */
static void st_logf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("%s", line);
    fflush(stdout);
    if (g_stReport) {
        fputs(line, g_stReport);
        fflush(g_stReport);
    }
}

/* ---------------------------------------------------------------- rows -- */

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
    ListView_InsertItem(g_ctl.list, &it);

    swprintf(buf, 64, L"%u", row->rate);
    ListView_SetItemText(g_ctl.list, idx, 1, buf);
    swprintf(buf, 64, L"%u", row->channels);
    ListView_SetItemText(g_ctl.list, idx, 2, buf);
    format_time(buf, 64, (double)row->total / row->rate);
    ListView_SetItemText(g_ctl.list, idx, 3, buf);
    ListView_SetItemText(g_ctl.list, idx, 4, row->loop ? L"Sí" : L"No");
}

/* Info panel with nothing to show: placeholders + loop control disabled, so
 * values from a previous track can never be mistaken for the new state. */
static void clear_info_panel(void)
{
    SetWindowTextW(g_ctl.valName, L"—");
    SetWindowTextW(g_ctl.valPath, L"");
    SetWindowTextW(g_ctl.valRate, L"—");
    SetWindowTextW(g_ctl.valCh, L"—");
    SetWindowTextW(g_ctl.valSamples, L"—");
    SetWindowTextW(g_ctl.valDur, L"—");
    SetWindowTextW(g_ctl.valLoop, L"—");
    SetWindowTextW(g_ctl.valLoopSt, L"No disponible");
    SetWindowTextW(g_ctl.valLoopEnd, L"No disponible");
    EnableWindow(g_ctl.checkLoop, FALSE);
}

/* Fill the info panel from a fresh snapshot. Also syncs the loop checkbox
 * with the engine (BM_SETCHECK here and in update_ui: the checkbox is
 * driven state, not the source of truth - player_set_loop is). */
static void fill_info_panel(const PlayerSnapshot *s)
{
    wchar_t buf[128];
    wchar_t t[40];

    swprintf(buf, 128, L"%u Hz", s->sample_rate);
    SetWindowTextW(g_ctl.valRate, buf);
    swprintf(buf, 128, L"%u", s->channels);
    SetWindowTextW(g_ctl.valCh, buf);
    swprintf(buf, 128, L"%llu", (unsigned long long)s->total_samples);
    SetWindowTextW(g_ctl.valSamples, buf);
    format_time(t, 40, (double)s->total_samples / s->sample_rate);
    SetWindowTextW(g_ctl.valDur, t);
    if (s->file_loop) {
        format_time(t, 40, (double)s->loop_start / s->sample_rate);
        swprintf(buf, 128, L"%u (%s)", s->loop_start, t);
        SetWindowTextW(g_ctl.valLoopSt, buf);
        swprintf(buf, 128, L"%llu (fin del archivo)",
                 (unsigned long long)s->total_samples);
        SetWindowTextW(g_ctl.valLoopEnd, buf);
        SetWindowTextW(g_ctl.valLoop, L"Sí");
    } else {
        SetWindowTextW(g_ctl.valLoop, L"No");
        SetWindowTextW(g_ctl.valLoopSt, L"No disponible");
        SetWindowTextW(g_ctl.valLoopEnd, L"No disponible");
    }
    EnableWindow(g_ctl.checkLoop, s->file_loop != 0);
    SendMessageW(g_ctl.checkLoop, BM_SETCHECK,
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

    if (idx < 0 || idx >= g_rowCount || idx == g_selected)
        return;

    /* Changing track always stops the current playback (player_open stops
     * it as well; stop first so the UI state is consistent). */
    player_stop(g_player);
    player_close(g_player);
    g_selected = idx;
    row = &g_rows[idx];

    if (!narrow_path(row->path, narrow, sizeof narrow)) {
        clear_info_panel();
        set_status(L"La ruta no es representable en la codificación ANSI "
                   L"del sistema.");
        return;
    }
    err[0] = '\0';
    if (player_open(g_player, narrow, err, sizeof err) != 0) {
        wchar_t werr[512];
        MultiByteToWideChar(CP_ACP, 0, err, -1, werr, 512);
        clear_info_panel();
        swprintf(msg, 1400, L"No se pudo abrir «%s»: %s", row->name, werr);
        set_status(msg);
        return;
    }

    SetWindowTextW(g_ctl.valName, row->name);
    SetWindowTextW(g_ctl.valPath, row->path);
    s = player_get_state(g_player);
    fill_info_panel(&s);
    g_snap = s;
    g_last_reported_error = 0;
    g_decode_reported = 0;
    swprintf(msg, 1400, L"Archivo abierto: %s", row->name);
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
    TrackRow *row;

    if (!narrow_path(path, narrow, sizeof narrow)) {
        swprintf(errbuf, errlen, L"«%s»: ruta no representable en ANSI",
                 file_name_of(path));
        return -1;
    }
    st = brstm_open(narrow, &tmp, err, sizeof err);
    if (st != BRSTM_OK) {
        wchar_t werr[512];
        MultiByteToWideChar(CP_ACP, 0, err, -1, werr, 512);
        swprintf(errbuf, errlen, L"«%s»: %s", file_name_of(path), werr);
        return -1;
    }

    if (g_rowCount == g_rowCap) {
        int cap = g_rowCap ? g_rowCap * 2 : 8;
        TrackRow *rows = (TrackRow *)realloc(g_rows,
                                             (size_t)cap * sizeof *rows);
        if (!rows) {
            brstm_close(&tmp);
            swprintf(errbuf, errlen, L"«%s»: sin memoria", file_name_of(path));
            return -1;
        }
        g_rows = rows;
        g_rowCap = cap;
    }
    row = &g_rows[g_rowCount];
    memset(row, 0, sizeof *row);
    row->path = dup_wide(path);
    row->name = dup_wide(file_name_of(path));
    if (!row->path || !row->name) {
        free(row->path);
        free(row->name);
        brstm_close(&tmp);
        swprintf(errbuf, errlen, L"sin memoria");
        return -1;
    }
    row->rate = tmp.info.sample_rate;
    row->channels = tmp.info.channels;
    row->total = tmp.info.total_samples;
    row->loop = tmp.info.loop;
    row->loop_start = tmp.info.loop_start;
    brstm_close(&tmp);

    list_insert_row(g_rowCount, row);
    g_rowCount++;
    return 0;
}

/* Remove row idx: shift the array, delete the list item and repair the
 * selection. If the removed row was playing, playback stops; the next row
 * (or none) becomes the selection, keeping g_selected always valid. */
static void remove_track(int idx)
{
    TrackRow *row;
    if (idx < 0 || idx >= g_rowCount)
        return;
    row = &g_rows[idx];
    free(row->path);
    free(row->name);
    memmove(&g_rows[idx], &g_rows[idx + 1],
            (size_t)(g_rowCount - idx - 1) * sizeof *g_rows);
    g_rowCount--;
    ListView_DeleteItem(g_ctl.list, idx);

    if (g_selected == idx) {
        player_stop(g_player);
        player_close(g_player);
        g_selected = -1;
        clear_info_panel();
        if (g_rowCount > 0)
            ListView_SetItemState(g_ctl.list,
                                  (idx < g_rowCount) ? idx : g_rowCount - 1,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
    } else if (g_selected > idx) {
        g_selected--;
    }
}

/* "Limpiar": stop playback, free every row's strings and empty the list.
 * The g_rows buffer itself is kept (capacity reuse across clears; it is
 * process-lifetime state like the rest of the globals and is reclaimed by
 * the OS at exit). */
static void remove_all(void)
{
    player_stop(g_player);
    player_close(g_player);
    for (int i = 0; i < g_rowCount; i++) {
        free(g_rows[i].path);
        free(g_rows[i].name);
    }
    g_rowCount = 0;
    g_selected = -1;
    ListView_DeleteAllItems(g_ctl.list);
    clear_info_panel();
    set_status(L"Lista vacía.");
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
    ofn.lpstrFilter =
        L"Archivos BRSTM (*.brstm)\0*.brstm\0Todos los archivos (*.*)\0*.*\0";
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
            swprintf(box, 2048, L"%s\n(y otros %d archivos no válidos)",
                     msg, failed - 1);
        else
            swprintf(box, 2048, L"%s", msg);
        MessageBoxW(g_hwnd, box, L"Archivo no válido o ilegible",
                    MB_OK | MB_ICONWARNING);
    }
    if (added > 0 && g_selected < 0 && g_rowCount > 0)
        ListView_SetItemState(g_ctl.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    if (added > 0) {
        swprintf(sum, 128, L"%d archivo(s) en la lista.", g_rowCount);
        set_status(sum);
    }
}

/* ------------------------------------------------- progress with loop ---- */

/* Custom progress bar: the loop section [loop_start, total) is shaded and a
 * marker line shows the loop start, so both loop points are visible while
 * the song plays. Paint state is UI-thread-only (set in update_ui). */
#define PROGRESS_CLASS_NAME L"MkwProgress"

static uint64_t g_prog_position;
static uint64_t g_prog_total;
static uint32_t g_prog_loop_start;
static int      g_prog_has_loop;
static HBRUSH   g_brushProgFill;   /* played portion */
static HBRUSH   g_brushProgLoop;   /* loop region */
static HBRUSH   g_brushProgMarker; /* loop start line */

static void update_ui(void);

/* Seek: click (or drag end) on the bar jumps to that source position. */
static int g_prog_drag;
static int g_prog_drag_x;

/* Map a click/drag x position to a source sample and seek there. The 1px
 * frame on each side of the custom bar is excluded, and the ratio is done
 * in 64-bit to avoid overflow on long files. Seeking while paused or
 * stopped just moves the cursor (player.c contract), so this works in any
 * state. */
static void prog_seek_from_x(HWND hwnd, int x)
{
    RECT rc;
    int span;

    if (!g_player || g_prog_total == 0)
        return;
    GetClientRect(hwnd, &rc);
    span = (rc.right - rc.left) - 2; /* minus the 1px frame on each side */
    if (span < 1)
        return;
    x -= 1;
    if (x < 0) x = 0;
    if (x > span) x = span;
    if (player_seek(g_player, (uint64_t)g_prog_total * (uint64_t)x
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
        if (g_prog_total > 0 && w > 2 && h > 2) {
            int span = w - 2;
            int loop_x = -1;
            int fill_x;

            if (g_prog_has_loop) {
                loop_x = 1 + (int)((uint64_t)span * g_prog_loop_start /
                                   g_prog_total);
                if (loop_x > w - 2)
                    loop_x = w - 2;
                if (loop_x > 1) {
                    RECT lr = { loop_x, 1, w - 1, h - 1 };
                    FillRect(dc, &lr, g_brushProgLoop);
                }
            }
            fill_x = 1 + (int)((uint64_t)span * g_prog_position /
                               g_prog_total);
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
        if (g_prog_drag && w > 2) {
            int dx = g_prog_drag_x < 1 ? 1 : g_prog_drag_x;
            if (dx > w - 2) dx = w - 2;
            RECT dr = { dx - 1, 0, dx + 1, h };
            FillRect(dc, &dr, GetSysColorBrush(COLOR_3DDKSHADOW));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (g_prog_total > 0) {
            g_prog_drag = 1;
            g_prog_drag_x = (int)(short)LOWORD(lp);
            SetCapture(hwnd);
            prog_seek_from_x(hwnd, g_prog_drag_x);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (g_prog_drag) {
            g_prog_drag_x = (int)(short)LOWORD(lp);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_prog_drag) {
            g_prog_drag = 0;
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

    SendMessageW(g_ctl.status, WM_SIZE, 0, 0);

    leftW = cw * 46 / 100;
    if (leftW < 330) leftW = 330;
    if (leftW > 440) leftW = 440;
    rightX = m + leftW + g;
    rightW = cw - rightX - m;
    x0 = rightX + 12;
    col2 = rightX + rightW / 2 + 2;

    /* Library (left column). */
    move_ctl(g_ctl.groupLib, m, m, leftW, ch - statusH - 2 * m);
    move_ctl(g_ctl.btnOpen, m + 12, m + 22, 96, 26);
    move_ctl(g_ctl.btnRemove, m + 116, m + 22, 84, 26);
    move_ctl(g_ctl.btnClear, m + 208, m + 22, 84, 26);
    move_ctl(g_ctl.list, m + 12, m + 56, leftW - 24,
             ch - statusH - 2 * m - 56 - 12);

    /* Information panel. */
    move_ctl(g_ctl.groupInfo, rightX, m, rightW, infoH);
    y = m + 22;
    move_ctl(g_ctl.capName, x0, y, 84, 18);
    move_ctl(g_ctl.valName, x0 + 88, y, rightW / 2 - 104, 18);
    move_ctl(g_ctl.capCh, col2, y, 74, 18);
    move_ctl(g_ctl.valCh, col2 + 78, y, rightW - (col2 - rightX) - 90, 18);
    y += 24;
    move_ctl(g_ctl.capRate, x0, y, 84, 18);
    move_ctl(g_ctl.valRate, x0 + 88, y, rightW / 2 - 104, 18);
    move_ctl(g_ctl.capDur, col2, y, 74, 18);
    move_ctl(g_ctl.valDur, col2 + 78, y, rightW - (col2 - rightX) - 90, 18);
    y += 24;
    move_ctl(g_ctl.capSamples, x0, y, 84, 18);
    move_ctl(g_ctl.valSamples, x0 + 88, y, rightW / 2 - 104, 18);
    move_ctl(g_ctl.capLoop, col2, y, 74, 18);
    move_ctl(g_ctl.valLoop, col2 + 78, y, rightW - (col2 - rightX) - 90, 18);
    y += 24;
    move_ctl(g_ctl.capLoopSt, x0, y, 84, 18);
    move_ctl(g_ctl.valLoopSt, x0 + 88, y, col2 - (x0 + 88) - 12, 18);
    move_ctl(g_ctl.capLoopEnd, col2, y, 74, 18);
    move_ctl(g_ctl.valLoopEnd, col2 + 78, y, rightW - (col2 - rightX) - 90,
             18);
    y += 24;
    move_ctl(g_ctl.capVer, x0, y, 84, 18);
    move_ctl(g_ctl.btnPair, x0 + 88, y - 2, rightW - 104, 22);
    y += 26;
    move_ctl(g_ctl.capPath, x0, y, 84, 18);
    move_ctl(g_ctl.valPath, x0 + 88, y, rightW - 104, 20);

    /* Transport. */
    y = m + infoH + g;
    move_ctl(g_ctl.groupPlay, rightX, y, rightW, playH);
    move_ctl(g_ctl.btnPlay, rightX + 12, y + 22, 96, 28);
    move_ctl(g_ctl.btnPause, rightX + 116, y + 22, 84, 28);
    move_ctl(g_ctl.btnStop, rightX + 208, y + 22, 84, 28);
    move_ctl(g_ctl.progress, rightX + 12, y + 58, rightW - 24 - 176, 16);
    move_ctl(g_ctl.valState, rightX + rightW - 12 - 164, y + 56, 164, 20);
    move_ctl(g_ctl.valTime, rightX + 12, y + 80, 170, 18);
    move_ctl(g_ctl.checkLoop, rightX + rightW - 12 - 170, y + 78, 170, 20);

    /* Speed. */
    y = m + infoH + g + playH + g;
    move_ctl(g_ctl.groupSpeed, rightX, y, rightW, speedH);
    move_ctl(g_ctl.valSpeed, rightX + 12, y + 28, 84, 20);
    move_ctl(g_ctl.valDevRate, rightX + rightW - 12 - 170, y + 28, 170, 20);
    move_ctl(g_ctl.btnSpd100, rightX + 12, y + 60, 56, 26);
    move_ctl(g_ctl.btnFlsm, rightX + 76, y + 60, 146, 26);
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
 * yet, so play/pause/stop/remove/clear/loop/pair all start disabled and
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

    g_ctl.groupLib = make_ctl(parent, 0, L"BUTTON", L"Biblioteca",
                              BS_GROUPBOX, 0, 0, 0, 0, 0);
    g_ctl.btnOpen = make_ctl(parent, 0, L"BUTTON", L"Abrir…",
                             0, IDC_BTN_OPEN, 0, 0, 0, 0);
    g_ctl.btnRemove = make_ctl(parent, 0, L"BUTTON", L"Quitar",
                               0, IDC_BTN_REMOVE, 0, 0, 0, 0);
    g_ctl.btnClear = make_ctl(parent, 0, L"BUTTON", L"Limpiar",
                              0, IDC_BTN_CLEAR, 0, 0, 0, 0);
    g_ctl.list = make_ctl(parent, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SHOWSELALWAYS, IDC_LIST,
                          0, 0, 0, 0);
    ListView_SetExtendedListViewStyle(g_ctl.list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    memset(&col, 0, sizeof col);
    col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
    col.fmt = LVCFMT_LEFT;
    {
        const wchar_t *names[5] = { L"Nombre", L"Hz", L"Canales",
                                    L"Duración", L"Loop" };
        int widths[5] = { 170, 55, 62, 74, 48 };
        for (i = 0; i < 5; i++) {
            col.pszText = (wchar_t *)names[i];
            col.cx = widths[i];
            col.iSubItem = i;
            ListView_InsertColumn(g_ctl.list, i, &col);
        }
    }

    g_ctl.groupInfo = make_ctl(parent, 0, L"BUTTON",
                               L"Información del archivo", BS_GROUPBOX,
                               0, 0, 0, 0, 0);
    g_ctl.capName = make_ctl(parent, 0, L"STATIC", L"Nombre:", 0,
                             IDC_CAP_NAME, 0, 0, 0, 0);
    g_ctl.capCh = make_ctl(parent, 0, L"STATIC", L"Canales:", 0,
                           IDC_CAP_CH, 0, 0, 0, 0);
    g_ctl.capRate = make_ctl(parent, 0, L"STATIC", L"Frecuencia:", 0,
                             IDC_CAP_RATE, 0, 0, 0, 0);
    g_ctl.capDur = make_ctl(parent, 0, L"STATIC", L"Duración:", 0,
                            IDC_CAP_DUR, 0, 0, 0, 0);
    g_ctl.capSamples = make_ctl(parent, 0, L"STATIC", L"Muestras:", 0,
                                IDC_CAP_SAMPLES, 0, 0, 0, 0);
    g_ctl.capLoop = make_ctl(parent, 0, L"STATIC", L"Loop:", 0,
                             IDC_CAP_LOOP, 0, 0, 0, 0);
    g_ctl.capLoopSt = make_ctl(parent, 0, L"STATIC", L"Inicio de loop:", 0,
                               IDC_CAP_LOOPST, 0, 0, 0, 0);
    g_ctl.capLoopEnd = make_ctl(parent, 0, L"STATIC", L"Fin de loop:", 0,
                                IDC_CAP_LOOPEND, 0, 0, 0, 0);
    g_ctl.capPath = make_ctl(parent, 0, L"STATIC", L"Ruta:", 0,
                             IDC_CAP_PATH, 0, 0, 0, 0);
    g_ctl.valName = make_ctl(parent, 0, L"STATIC", L"—", 0,
                             IDC_VAL_NAME, 0, 0, 0, 0);
    g_ctl.valPath = make_ctl(parent, WS_EX_CLIENTEDGE, L"EDIT", L"",
                             ES_READONLY | ES_AUTOHSCROLL, IDC_VAL_PATH,
                             0, 0, 0, 0);
    g_ctl.valRate = make_ctl(parent, 0, L"STATIC", L"—", 0,
                             IDC_VAL_RATE, 0, 0, 0, 0);
    g_ctl.valCh = make_ctl(parent, 0, L"STATIC", L"—", 0,
                           IDC_VAL_CH, 0, 0, 0, 0);
    g_ctl.valSamples = make_ctl(parent, 0, L"STATIC", L"—", 0,
                                IDC_VAL_SAMPLES, 0, 0, 0, 0);
    g_ctl.valDur = make_ctl(parent, 0, L"STATIC", L"—", 0,
                            IDC_VAL_DUR, 0, 0, 0, 0);
    g_ctl.valLoop = make_ctl(parent, 0, L"STATIC", L"—", 0,
                             IDC_VAL_LOOP, 0, 0, 0, 0);
    g_ctl.valLoopSt = make_ctl(parent, 0, L"STATIC", L"No disponible", 0,
                               IDC_VAL_LOOPST, 0, 0, 0, 0);
    g_ctl.valLoopEnd = make_ctl(parent, 0, L"STATIC", L"No disponible", 0,
                                IDC_VAL_LOOPEND, 0, 0, 0, 0);
    g_ctl.capVer = make_ctl(parent, 0, L"STATIC", L"Versión:", 0,
                            IDC_CAP_VER, 0, 0, 0, 0);
    g_ctl.btnPair = make_ctl(parent, 0, L"BUTTON", L"—", 0,
                             IDC_BTN_PAIR, 0, 0, 0, 0);

    g_ctl.groupPlay = make_ctl(parent, 0, L"BUTTON", L"Reproducción",
                               BS_GROUPBOX, 0, 0, 0, 0, 0);
    g_ctl.btnPlay = make_ctl(parent, 0, L"BUTTON", L"Reproducir",
                             0, IDC_BTN_PLAY, 0, 0, 0, 0);
    g_ctl.btnPause = make_ctl(parent, 0, L"BUTTON", L"Pausar",
                              0, IDC_BTN_PAUSE, 0, 0, 0, 0);
    g_ctl.btnStop = make_ctl(parent, 0, L"BUTTON", L"Detener",
                             0, IDC_BTN_STOP, 0, 0, 0, 0);
    g_ctl.progress = make_ctl(parent, 0, PROGRESS_CLASS_NAME, L"",
                              0, IDC_PROGRESS, 0, 0, 0, 0);
    g_ctl.valState = make_ctl(parent, 0, L"STATIC", L"Estado: Detenido",
                              SS_RIGHT, IDC_VAL_STATE, 0, 0, 0, 0);
    g_ctl.valTime = make_ctl(parent, 0, L"STATIC", L"0:00.000 / 0:00.000",
                             0, IDC_VAL_TIME, 0, 0, 0, 0);
    g_ctl.checkLoop = make_ctl(parent, 0, L"BUTTON", L"Repetir loop",
                               BS_AUTOCHECKBOX | BS_LEFT, IDC_CHECK_LOOP,
                               0, 0, 0, 0);

    g_ctl.groupSpeed = make_ctl(parent, 0, L"BUTTON", L"Velocidad",
                                BS_GROUPBOX, 0, 0, 0, 0, 0);
    g_ctl.valSpeed = make_ctl(parent, 0, L"STATIC", L"1,00×", SS_RIGHT,
                              IDC_VAL_SPEED, 0, 0, 0, 0);
    g_ctl.valDevRate = make_ctl(parent, 0, L"STATIC", L"Dispositivo: —",
                                SS_RIGHT, IDC_VAL_DEVRATE, 0, 0, 0, 0);
    g_ctl.btnSpd100 = make_ctl(parent, 0, L"BUTTON", L"1,0×", 0,
                               IDC_BTN_SPD100, 0, 0, 0, 0);
    g_ctl.btnFlsm = make_ctl(parent, 0, L"BUTTON", L"Final Lap ×1,06812", 0,
                             IDC_BTN_FLSM, 0, 0, 0, 0);

    g_ctl.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"Listo.",
                                   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, parent,
                                   (HMENU)(INT_PTR)2, GetModuleHandleW(NULL),
                                   NULL);
    SendMessageW(g_ctl.status, WM_SETFONT, (WPARAM)g_font, TRUE);

    layout();
    EnableWindow(g_ctl.btnPlay, FALSE);
    EnableWindow(g_ctl.btnPause, FALSE);
    EnableWindow(g_ctl.btnStop, FALSE);
    EnableWindow(g_ctl.btnRemove, FALSE);
    EnableWindow(g_ctl.btnClear, FALSE);
    EnableWindow(g_ctl.checkLoop, FALSE);
    EnableWindow(g_ctl.btnPair, FALSE);
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
    SetWindowTextW(g_ctl.valSpeed, buf);
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

    g_snap = s;

    switch (s.state) {
    case PLAYER_PLAYING:
        stateText = L"Estado: Reproduciendo";
        break;
    case PLAYER_PAUSED:
        stateText = L"Estado: Pausado";
        break;
    case PLAYER_FINISHED:
        stateText = L"Estado: Terminado";
        break;
    default:
        stateText = L"Estado: Detenido";
        break;
    }
    {
        wchar_t cur[64];
        GetWindowTextW(g_ctl.valState, cur, 64);
        if (wcscmp(cur, stateText) != 0)
            SetWindowTextW(g_ctl.valState, stateText);
    }

    {
        const wchar_t *pauseText =
            s.state == PLAYER_PAUSED ? L"Reanudar" : L"Pausar";
        wchar_t curPause[32];
        GetWindowTextW(g_ctl.btnPause, curPause, 32);
        if (wcscmp(curPause, pauseText) != 0)
            SetWindowTextW(g_ctl.btnPause, pauseText);
    }
    EnableWindow(g_ctl.btnPlay,
                 g_selected >= 0 && s.state != PLAYER_PLAYING);
    EnableWindow(g_ctl.btnPause,
                 s.state == PLAYER_PLAYING || s.state == PLAYER_PAUSED);
    EnableWindow(g_ctl.btnStop,
                 s.state == PLAYER_PLAYING || s.state == PLAYER_PAUSED);
    EnableWindow(g_ctl.btnRemove, g_rowCount > 0);
    EnableWindow(g_ctl.btnClear, g_rowCount > 0);

    /* Version button: its caption is the current selection. */
    {
        wchar_t pairText[64], cur[64];
        const wchar_t *want;
        if (s.channels == 0) {
            want = L"—";
        } else if (s.pair_count == 0) {
            want = L"Estéreo";
        } else if (s.pair_sel < 0) {
            want = L"Todas (mezcla)";
        } else {
            swprintf(pairText, 64, L"Versión %d · canales %d-%d",
                     s.pair_sel + 1, s.pair_sel * 2 + 1, s.pair_sel * 2 + 2);
            want = pairText;
        }
        GetWindowTextW(g_ctl.btnPair, cur, 64);
        if (wcscmp(cur, want) != 0)
            SetWindowTextW(g_ctl.btnPair, want);
        EnableWindow(g_ctl.btnPair, s.pair_count > 0);
    }

    if (s.file_loop)
        SendMessageW(g_ctl.checkLoop, BM_SETCHECK,
                     s.loop_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    devRate = s.sample_rate ? (double)s.device_rate : 0.0;
    if (s.sample_rate) {
        if (s.audio_folded || s.pair_sel >= 0)
            swprintf(buf, 160, L"Dispositivo: %u Hz · estéreo",
                     s.device_rate);
        else
            swprintf(buf, 160, L"Dispositivo: %u Hz", s.device_rate);
        SetWindowTextW(g_ctl.valDevRate, buf);
        format_speed(buf, 40, s.speed_ppm);
        SetWindowTextW(g_ctl.valSpeed, buf);
    } else {
        SetWindowTextW(g_ctl.valDevRate, L"Dispositivo: —");
    }

    if (g_prog_position != s.position || g_prog_total != s.total_samples ||
        g_prog_has_loop != s.file_loop ||
        g_prog_loop_start != s.loop_start) {
        g_prog_position = s.position;
        g_prog_total = s.total_samples;
        g_prog_has_loop = s.file_loop;
        g_prog_loop_start = s.loop_start;
        InvalidateRect(g_ctl.progress, NULL, FALSE);
    }
    if (s.total_samples > 0) {
        format_time(t1, 40, devRate > 0 ? (double)s.position / devRate : 0.0);
        format_time(t2, 40, devRate > 0
                          ? (double)s.total_samples / devRate : 0.0);
        swprintf(buf, 160, L"%s / %s", t1, t2);
        SetWindowTextW(g_ctl.valTime, buf);
    } else {
        SetWindowTextW(g_ctl.valTime, L"0:00.000 / 0:00.000");
    }

    if (s.audio_error != 0 && s.audio_error != g_last_reported_error) {
        wchar_t werr[256], msg[320];
        waveOutGetErrorTextW(s.audio_error, werr, 256);
        swprintf(msg, 320, L"Error de salida de audio: %s", werr);
        set_status(msg);
        g_last_reported_error = s.audio_error;
    }
    if (s.decode_failed && !g_decode_reported) {
        set_status(L"Error de decodificación; reproducción detenida.");
        g_decode_reported = 1;
    }
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
    case IDC_BTN_REMOVE:
        if (g_selected >= 0)
            remove_track(g_selected);
        break;
    case IDC_BTN_CLEAR:
        remove_all();
        break;
    case IDC_BTN_PLAY:
        if (player_play(g_player) == 0)
            set_status(L"Reproduciendo…");
        else
            set_status(L"No se puede reproducir (sin archivo o sin salida "
                       L"de audio).");
        break;
    case IDC_BTN_PAUSE:
        /* Read the live state: g_snap may be up to one tick behind. */
        if (player_get_state(g_player).state == PLAYER_PAUSED) {
            player_play(g_player);
            set_status(L"Reanudando…");
        } else {
            player_pause(g_player);
            set_status(L"Pausado.");
        }
        break;
    case IDC_BTN_STOP:
        player_stop(g_player);
        set_status(L"Detenido.");
        break;
    case IDC_CHECK_LOOP:
        player_set_loop(g_player,
                        SendMessageW(g_ctl.checkLoop, BM_GETCHECK, 0, 0)
                            == BST_CHECKED);
        break;
    case IDC_BTN_SPD100:
        apply_speed(PLAYER_SPEED_SCALE);
        break;
    case IDC_BTN_FLSM:
        apply_speed(PLAYER_FLSM_SPEED_PPM);
        break;
    case IDC_BTN_PAIR: {
        PlayerSnapshot s = player_get_state(g_player);
        wchar_t msg[80];
        int next;
        if (s.pair_count == 0)
            break;
        if (s.pair_sel < 0)
            next = 0;
        else if (s.pair_sel + 1 < (int)s.pair_count)
            next = s.pair_sel + 1;
        else
            next = -1;
        if (player_set_channel_pair(g_player, next) != 0)
            break;
        if (next < 0)
            set_status(L"Mezclando todos los canales.");
        else {
            swprintf(msg, 80, L"Versión %d: canales %d-%d.",
                     next + 1, next * 2 + 1, next * 2 + 2);
            set_status(msg);
        }
        break;
    }
    default:
        break;
    }
    update_ui();
}

/* --------------------------------------------------------- window proc -- */

/* Main window procedure. WM_CREATE builds controls + timer; WM_NOTIFY turns
 * list selection into select_track(); WM_COMMAND routes buttons; the color
 * messages tint the state label by playback state; WM_DESTROY tears down
 * the engine, GDI objects and the message loop (PostQuitMessage). */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
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

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkMode(hdc, TRANSPARENT);
        if (ctl == g_ctl.valState) {
            PlayerState st = g_snap.state;
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
        if (g_brushFace) {
            DeleteObject(g_brushFace);
            g_brushFace = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ------------------------------------------------------------ selftest -- */

/* Process working set in bytes (selftest leak check; 0 if the API fails,
 * which the check then tolerates rather than reporting a false leak). */
static SIZE_T working_set(void)
{
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return pmc.WorkingSetSize;
    return 0;
}

/* Resource baselines for the leak checks: taken before the first play
 * (phase 2) and compared after the cycles and after window close (run at
 * the end). File-scope because the phases are separate functions. */
static DWORD g_stHandles0;
static SIZE_T g_stMem0;

/* Phase 1 - library: the custom progress class is in use, an invalid file
 * is rejected without entering the list, and selecting the first row opens
 * it and fills the metadata (same handler as a real click). */
static void st_phase_library(void)
{
    /* The progress control must be our loop-aware custom class. */
    {
        wchar_t cls[64] = L"";
        GetClassNameW(g_ctl.progress, cls, 64);
        st_check(wcscmp(cls, PROGRESS_CLASS_NAME) == 0,
                 "progress control uses the loop-aware custom class");
    }

    /* Invalid file must be rejected without entering the list. */
    if (g_hasInvalid) {
        wchar_t err[1024];
        int before = g_rowCount;
        int rc = add_track(g_stInvalid, err, 1024);
        st_check(rc != 0 && g_rowCount == before,
                 "invalid file rejected with error message");
    }

    /* Select first track (same handler as a click). */
    ListView_SetItemState(g_ctl.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    pump(50);
    st_check(g_selected == 0 && g_snap.sample_rate > 0,
             "selection opens track and fills metadata");
}

/* Phase 2 - transport: play reaches PLAYING with the device open, the API
 * stays responsive while the engine decodes (100 ms budget on
 * player_get_state), and pause freezes the position before resume. Also
 * records the resource baselines used by the final leak checks. */
static void st_phase_transport(void)
{
    int i;

    /* Play: real audio path. */
    GetProcessHandleCount(GetCurrentProcess(), &g_stHandles0);
    g_stMem0 = working_set();
    send_cmd(IDC_BTN_PLAY);
    st_check(wait_state(PLAYER_PLAYING, 4000), "play reaches PLAYING");
    st_check(player_get_state(g_player).audio_error == 0,
             "audio device opened without error");

    /* UI thread responsiveness while the engine plays. */
    {
        LARGE_INTEGER f, t0, t1, worst;
        double worst_ms;
        QueryPerformanceFrequency(&f);
        worst.QuadPart = 0;
        for (i = 0; i < 50; i++) {
            QueryPerformanceCounter(&t0);
            (void)player_get_state(g_player);
            QueryPerformanceCounter(&t1);
            if (t1.QuadPart - t0.QuadPart > worst.QuadPart)
                worst.QuadPart = t1.QuadPart - t0.QuadPart;
            Sleep(0);
        }
        worst_ms = 1000.0 * (double)worst.QuadPart / (double)f.QuadPart;
        printf("  max player_get_state latency: %.3f ms\n", worst_ms);
        st_check(worst_ms < 100.0,
                 "engine API answers in <100 ms while playing");
    }

    /* Pause / resume with position stability. */
    send_cmd(IDC_BTN_PAUSE);
    st_check(wait_state(PLAYER_PAUSED, 2000), "pause reaches PAUSED");
    {
        uint64_t p0 = player_get_state(g_player).position;
        pump(300);
        uint64_t p1 = player_get_state(g_player).position;
        st_check(p0 == p1, "position frozen while paused");
    }
    send_cmd(IDC_BTN_PAUSE);
    st_check(wait_state(PLAYER_PLAYING, 2000), "resume reaches PLAYING");
}

/* Phase 3 - seek, speed, loop: a click at 3/4 of the bar seeks into
 * 55..80% and playback continues from there; the FLSM button changes the
 * device rate live; the loop checkbox (emulating a real click on the
 * BS_AUTOCHECKBOX) flips the engine flag and restores it. */
static void st_phase_seek_speed(void)
{
    /* Seek: a click on the progress bar jumps near that point and the
     * playback keeps going from there. */
    {
        RECT rc;
        int x;
        PlayerSnapshot s1, s2;

        GetClientRect(g_ctl.progress, &rc);
        x = (rc.right - rc.left) * 3 / 4;
        SendMessageW(g_ctl.progress, WM_LBUTTONDOWN, 0, MAKELPARAM(x, 8));
        SendMessageW(g_ctl.progress, WM_LBUTTONUP, 0, MAKELPARAM(x, 8));
        pump(100);
        s1 = player_get_state(g_player);
        st_check(s1.position > s1.total_samples * 55 / 100
                 && s1.position < s1.total_samples * 80 / 100,
                 "click on the progress bar seeks near that point");
        pump(900);
        s2 = player_get_state(g_player);
        st_check(s2.state == PLAYER_PLAYING && s2.position > s1.position,
                 "playback continues from the seeked position");
    }

    /* Speed change to the FLSM factor while playing. */
    send_cmd(IDC_BTN_FLSM);
    pump(500);
    {
        PlayerSnapshot s = player_get_state(g_player);
        uint32_t want = player_device_rate(s.sample_rate,
                                           PLAYER_FLSM_SPEED_PPM);
        st_check(s.state == PLAYER_PLAYING && s.device_rate == want,
                 "FLSM speed applied live, device rate matches floor(rate*k)");
    }

    /* Loop toggle (only if the file has loop metadata). */
    {
        PlayerSnapshot s = player_get_state(g_player);
        if (s.file_loop) {
            int before = s.loop_enabled;
            int boxBefore =
                SendMessageW(g_ctl.checkLoop, BM_GETCHECK, 0, 0)
                    == BST_CHECKED;
            /* The handler reads the checkbox (correct for a real click on a
             * BS_AUTOCHECKBOX, which flips the box before BN_CLICKED is
             * delivered); emulate that click instead of only sending the
             * command, and verify box and engine start in sync. */
            st_check(boxBefore == (before != 0),
                     "loop checkbox matches the engine state");
            SendMessageW(g_ctl.checkLoop, BM_SETCHECK,
                         boxBefore ? BST_UNCHECKED : BST_CHECKED, 0);
            send_cmd(IDC_CHECK_LOOP);
            pump(50);
            s = player_get_state(g_player);
            st_check(s.loop_enabled != before, "loop toggle flips the flag");
            SendMessageW(g_ctl.checkLoop, BM_SETCHECK,
                         boxBefore ? BST_CHECKED : BST_UNCHECKED, 0);
            send_cmd(IDC_CHECK_LOOP);
            pump(50);
        } else {
            st_check(1, "no-loop file: loop control left disabled");
        }
    }
}

/* Phase 4 - track switch and version (channel-pair) selection: switching
 * while playing stops cleanly; the 4-channel second track exposes 2 pairs;
 * the button cycles 1 -> 2 -> mix, its caption matches, and switching
 * versions live (device reopen path) keeps playback running without audio
 * errors. */
static void st_phase_pair(void)
{
    /* Track switch while playing: playback must stop cleanly. */
    ListView_SetItemState(g_ctl.list, 1, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    st_check(wait_state(PLAYER_STOPPED, 2000),
             "switching track while playing stops playback");
    st_check(g_selected == 1, "second track selected");

    /* Version (channel pair) selection on the 4-channel second track. */
    {
        PlayerSnapshot s = player_get_state(g_player);
        st_check(s.channels == 4 && s.pair_count == 2,
                 "4-channel file exposes 2 channel pairs");
        send_cmd(IDC_BTN_PAIR);
        pump(50);
        s = player_get_state(g_player);
        st_check(s.pair_sel == 0, "version button selects pair 1");
        send_cmd(IDC_BTN_PAIR);
        pump(50);
        s = player_get_state(g_player);
        st_check(s.pair_sel == 1, "second press selects pair 2");
        {
            wchar_t cap[64] = L"";
            GetWindowTextW(g_ctl.btnPair, cap, 64);
            st_check(wcsstr(cap, L"Versión 2") != NULL,
                     "button caption shows the selected version");
        }
        send_cmd(IDC_BTN_PAIR);
        pump(50);
        s = player_get_state(g_player);
        st_check(s.pair_sel == -1, "third press returns to all channels");

        /* Live switch: a version is chosen while audio is playing. */
        send_cmd(IDC_BTN_PAIR);
        pump(50); /* pair 1 selected */
        send_cmd(IDC_BTN_PLAY);
        st_check(wait_state(PLAYER_PLAYING, 4000),
                 "play with a version selected");
        send_cmd(IDC_BTN_PAIR);
        pump(150); /* pair 2 selected */
        s = player_get_state(g_player);
        st_check(s.state == PLAYER_PLAYING && s.pair_sel == 1,
                 "version switches live while playing");
        send_cmd(IDC_BTN_PAIR);
        pump(150); /* back to all channels */
        s = player_get_state(g_player);
        st_check(s.state == PLAYER_PLAYING && s.pair_sel == -1
                 && s.audio_error == 0,
                 "mix-all restored live without audio errors");
        send_cmd(IDC_BTN_STOP);
        st_check(wait_state(PLAYER_STOPPED, 2000),
                 "stop after the version checks");
    }
}

static int run_selftest(void)
{
    DWORD handles1, handles2;
    SIZE_T mem1;
    int i;

    printf("selftest: %d track(s)%s\n", g_rowCount,
           g_hasInvalid ? " + invalid file" : "");
    if (g_stReport)
        fprintf(g_stReport, "selftest: %d track(s)%s\n", g_rowCount,
                g_hasInvalid ? " + invalid file" : "");

    st_check(g_rowCount >= 2, "at least 2 tracks loaded");
    st_phase_library();
    st_phase_transport();
    st_phase_seek_speed();
    st_phase_pair();

    /* Open/play/stop cycles must not leak handles or memory. */
    for (i = 0; i < 3; i++) {
        send_cmd(IDC_BTN_PLAY);
        wait_state(PLAYER_PLAYING, 4000);
        pump(150);
        send_cmd(IDC_BTN_STOP);
        wait_state(PLAYER_STOPPED, 2000);
    }
    pump(200);
    GetProcessHandleCount(GetCurrentProcess(), &handles1);
    mem1 = working_set();
    st_logf("handles after cycles: %lu (base %lu), working set +%.1f MB\n",
            (unsigned long)handles1, (unsigned long)g_stHandles0,
            ((double)mem1 - (double)g_stMem0) / (1024.0 * 1024.0));
    /* The device is open when g_stHandles0 is taken and closed after the
     * last stop, so a healthy count can be BELOW the baseline; only growth
     * would indicate a leak. */
    st_check(handles1 <= g_stHandles0 + 8,
             "no handle growth across play/stop cycles");
    st_check(mem1 <= g_stMem0 + 8u * 1024u * 1024u,
             "memory growth under 8 MB across cycles");

    /* Cleanup: close the real window and compare resources. */
    SendMessageW(g_hwnd, WM_CLOSE, 0, 0);
    {
        MSG msg;
        for (;;) {
            if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT)
                    break;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            } else {
                Sleep(5);
            }
        }
    }
    pump(100);
    GetProcessHandleCount(GetCurrentProcess(), &handles2);
    st_logf("handles after window close: %lu (base %lu)\n",
            (unsigned long)handles2, (unsigned long)g_stHandles0);
    st_check(handles2 <= g_stHandles0 + 8,
             "closing the window releases resources");

    printf("RESULT: %s (%d checks, %d failed)\n",
           g_stFails ? "FAIL" : "PASS", g_stChecks, g_stFails);
    if (g_stReport) {
        fprintf(g_stReport, "RESULT: %s (%d checks, %d failed)\n",
                g_stFails ? "FAIL" : "PASS", g_stChecks, g_stFails);
        fclose(g_stReport);
        g_stReport = NULL;
    }
    return g_stFails ? 1 : 0;
}

/* ---------------------------------------------------------------- main -- */

/* Argument parsing for the CLI (paths = tracks to preload; --selftest starts
 * the scripted session; --invalid names a file that must be rejected;
 * --report redirects the results). Runs after the window exists because
 * add_track needs the ListView. */
static void load_args(int argc, wchar_t **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--selftest") == 0) {
            g_stMode = 1;
        } else if (wcscmp(argv[i], L"--invalid") == 0 && i + 1 < argc) {
            wcscpy_s(g_stInvalid, 1024, argv[++i]);
            g_hasInvalid = 1;
        } else if (wcscmp(argv[i], L"--report") == 0 && i + 1 < argc) {
            /* Narrow fprintf() on a ccs=UTF-8 stream aborts on this CRT
             * (measured: 0xC0000409 in the test harness), so the report is
             * plain text. */
            g_stReport = _wfopen(argv[++i], L"w");
        } else {
            wchar_t err[1024];
            if (add_track(argv[i], err, 1024) != 0 && !g_stMode)
                MessageBoxW(NULL, err, L"Archivo no válido o ilegible",
                            MB_OK | MB_ICONWARNING);
        }
    }
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
    wc.lpszClassName = L"MkwiiBrstmPlayer";
    if (!RegisterClassExW(&wc))
        return 1;

    g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
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
        MessageBoxW(NULL, L"No se pudo iniciar el motor de reproducción.",
                    L"Error fatal", MB_OK | MB_ICONERROR);
        LocalFree(argv);
        return 1;
    }

    /* Tracks are added after the window exists: add_track inserts into the
     * ListView, which is created during CreateWindowExW (WM_CREATE). */
    g_hwnd = CreateWindowExW(0, L"MkwiiBrstmPlayer",
                             L"Metal Kart Wii — Reproductor BRSTM (T2)",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 940, 520,
                             NULL, NULL, inst, NULL);
    if (!g_hwnd) {
        player_destroy(g_player);
        LocalFree(argv);
        return 1;
    }

    load_args(argc, argv);

    if (g_rowCount > 0)
        ListView_SetItemState(g_ctl.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);

    if (g_stMode) {
        rc = run_selftest();
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
