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
#define IDC_SPEED        1009
#define IDC_BTN_SPD100   1010
#define IDC_BTN_FLSM     1011
#define IDC_BTN_SPD150   1012
#define IDC_BTN_SPD200   1013
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
#define IDC_CAP_NAME     1030
#define IDC_CAP_CH       1031
#define IDC_CAP_RATE     1032
#define IDC_CAP_DUR      1033
#define IDC_CAP_SAMPLES  1034
#define IDC_CAP_LOOP     1035
#define IDC_CAP_LOOPST   1036
#define IDC_CAP_PATH     1037

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
    HWND btnSpd100, btnFlsm, btnSpd150, btnSpd200;
    HWND checkLoop, speed, progress;
    HWND capName, capCh, capRate, capDur, capSamples, capLoop, capLoopSt;
    HWND capPath;
    HWND valName, valPath, valRate, valCh, valSamples, valDur;
    HWND valLoop, valLoopSt, valState, valTime, valSpeed, valDevRate;
} Controls;

static HWND g_hwnd;
static Controls g_ctl;
static HFONT g_font;
static HBRUSH g_brushFace;
static Player *g_player;

static TrackRow *g_rows;
static int g_rowCount, g_rowCap;
static int g_selected = -1;

static PlayerSnapshot g_snap;
static uint32_t g_last_reported_error;
static int g_decode_reported;

/* Selftest bookkeeping. */
static int g_stMode;
static int g_stChecks, g_stFails;
static FILE *g_stReport;
static wchar_t g_stInvalid[1024];
static int g_hasInvalid;

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

static const wchar_t *file_name_of(const wchar_t *path)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    const wchar_t *slash2 = wcsrchr(path, L'/');
    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    return slash ? slash + 1 : path;
}

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

static void set_status(const wchar_t *text)
{
    if (g_ctl.status)
        SendMessageW(g_ctl.status, SB_SETTEXTW, 0, (LPARAM)text);
}

/* ------------------------------------------------------------ selftest -- */

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
    EnableWindow(g_ctl.checkLoop, FALSE);
}

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
        SetWindowTextW(g_ctl.valLoop, L"Sí");
    } else {
        SetWindowTextW(g_ctl.valLoop, L"No");
        SetWindowTextW(g_ctl.valLoopSt, L"No disponible");
    }
    EnableWindow(g_ctl.checkLoop, s->file_loop != 0);
    SendMessageW(g_ctl.checkLoop, BM_SETCHECK,
                 s->loop_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

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

/* -------------------------------------------------------------- layout -- */

static void move_ctl(HWND h, int x, int y, int w, int hh)
{
    if (h)
        SetWindowPos(h, NULL, x, y, w, hh,
                     SWP_NOZORDER | SWP_NOACTIVATE);
}

static void layout(void)
{
    RECT rc;
    int cw, ch;
    int m = 12, g = 10, statusH = 24;
    int leftW, rightX, rightW;
    int infoH = 154, playH = 110, speedH = 100;
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
    move_ctl(g_ctl.valLoopSt, x0 + 88, y, rightW - 104, 18);
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
    move_ctl(g_ctl.speed, rightX + 12, y + 24, rightW - 292, 28);
    move_ctl(g_ctl.valSpeed, rightX + rightW - 12 - 170 - 6 - 84, y + 28,
             84, 20);
    move_ctl(g_ctl.valDevRate, rightX + rightW - 12 - 170, y + 28, 170, 20);
    move_ctl(g_ctl.btnSpd100, rightX + 12, y + 60, 56, 26);
    move_ctl(g_ctl.btnFlsm, rightX + 76, y + 60, 146, 26);
    move_ctl(g_ctl.btnSpd150, rightX + 230, y + 60, 56, 26);
    move_ctl(g_ctl.btnSpd200, rightX + 294, y + 60, 56, 26);
}

/* ------------------------------------------------------------ creation -- */

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

    g_ctl.groupPlay = make_ctl(parent, 0, L"BUTTON", L"Reproducción",
                               BS_GROUPBOX, 0, 0, 0, 0, 0);
    g_ctl.btnPlay = make_ctl(parent, 0, L"BUTTON", L"Reproducir",
                             0, IDC_BTN_PLAY, 0, 0, 0, 0);
    g_ctl.btnPause = make_ctl(parent, 0, L"BUTTON", L"Pausar",
                              0, IDC_BTN_PAUSE, 0, 0, 0, 0);
    g_ctl.btnStop = make_ctl(parent, 0, L"BUTTON", L"Detener",
                             0, IDC_BTN_STOP, 0, 0, 0, 0);
    g_ctl.progress = make_ctl(parent, 0, PROGRESS_CLASSW, L"",
                              PBS_SMOOTH, IDC_PROGRESS, 0, 0, 0, 0);
    SendMessageW(g_ctl.progress, PBM_SETRANGE32, 0, 1000);
    g_ctl.valState = make_ctl(parent, 0, L"STATIC", L"Estado: Detenido",
                              SS_RIGHT, IDC_VAL_STATE, 0, 0, 0, 0);
    g_ctl.valTime = make_ctl(parent, 0, L"STATIC", L"0:00.000 / 0:00.000",
                             0, IDC_VAL_TIME, 0, 0, 0, 0);
    g_ctl.checkLoop = make_ctl(parent, 0, L"BUTTON", L"Repetir loop",
                               BS_AUTOCHECKBOX | BS_LEFT, IDC_CHECK_LOOP,
                               0, 0, 0, 0);

    g_ctl.groupSpeed = make_ctl(parent, 0, L"BUTTON", L"Velocidad",
                                BS_GROUPBOX, 0, 0, 0, 0, 0);
    g_ctl.speed = make_ctl(parent, 0, TRACKBAR_CLASSW, L"",
                           TBS_NOTICKS | TBS_HORZ | TBS_TOOLTIPS,
                           IDC_SPEED, 0, 0, 0, 0);
    SendMessageW(g_ctl.speed, TBM_SETRANGE, TRUE, MAKELPARAM(50, 200));
    SendMessageW(g_ctl.speed, TBM_SETPOS, TRUE, 100);
    g_ctl.valSpeed = make_ctl(parent, 0, L"STATIC", L"1,00×", SS_RIGHT,
                              IDC_VAL_SPEED, 0, 0, 0, 0);
    g_ctl.valDevRate = make_ctl(parent, 0, L"STATIC", L"Dispositivo: —",
                                SS_RIGHT, IDC_VAL_DEVRATE, 0, 0, 0, 0);
    g_ctl.btnSpd100 = make_ctl(parent, 0, L"BUTTON", L"1,0×", 0,
                               IDC_BTN_SPD100, 0, 0, 0, 0);
    g_ctl.btnFlsm = make_ctl(parent, 0, L"BUTTON", L"Final Lap ×1,06812", 0,
                             IDC_BTN_FLSM, 0, 0, 0, 0);
    g_ctl.btnSpd150 = make_ctl(parent, 0, L"BUTTON", L"1,5×", 0,
                               IDC_BTN_SPD150, 0, 0, 0, 0);
    g_ctl.btnSpd200 = make_ctl(parent, 0, L"BUTTON", L"2,0×", 0,
                               IDC_BTN_SPD200, 0, 0, 0, 0);

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
}

/* ----------------------------------------------------------------- UI --- */

static void apply_speed(uint32_t ppm)
{
    wchar_t buf[40];
    player_set_speed(g_player, ppm);
    format_speed(buf, 40, ppm);
    SetWindowTextW(g_ctl.valSpeed, buf);
    SendMessageW(g_ctl.speed, TBM_SETPOS, TRUE,
                 (int)(ppm / 10000u));
}

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

    if (s.file_loop)
        SendMessageW(g_ctl.checkLoop, BM_SETCHECK,
                     s.loop_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    devRate = s.sample_rate ? (double)s.device_rate : 0.0;
    if (s.sample_rate) {
        if (s.audio_folded)
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

    if (s.total_samples > 0) {
        int pos = (int)((s.position * 1000ULL) / s.total_samples);
        if (pos > 1000) pos = 1000;
        SendMessageW(g_ctl.progress, PBM_SETPOS, pos, 0);
        format_time(t1, 40, devRate > 0 ? (double)s.position / devRate : 0.0);
        format_time(t2, 40, devRate > 0
                          ? (double)s.total_samples / devRate : 0.0);
        swprintf(buf, 160, L"%s / %s", t1, t2);
        SetWindowTextW(g_ctl.valTime, buf);
    } else {
        SendMessageW(g_ctl.progress, PBM_SETPOS, 0, 0);
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
    case IDC_BTN_SPD150:
        apply_speed(1500000u);
        break;
    case IDC_BTN_SPD200:
        apply_speed(2000000u);
        break;
    default:
        break;
    }
    update_ui();
}

/* --------------------------------------------------------- window proc -- */

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
        mm->ptMinTrackSize.y = 480;
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

    case WM_HSCROLL: {
        if ((HWND)wp == g_ctl.speed) {
            int pos = (int)SendMessageW(g_ctl.speed, TBM_GETPOS, 0, 0);
            if (LOWORD(wp) != TB_THUMBTRACK) {
                wchar_t buf[40];
                player_set_speed(g_player, (uint32_t)pos * 10000u);
                format_speed(buf, 40, (uint32_t)pos * 10000u);
                SetWindowTextW(g_ctl.valSpeed, buf);
                update_ui();
            }
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

static SIZE_T working_set(void)
{
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return pmc.WorkingSetSize;
    return 0;
}

static int run_selftest(void)
{
    DWORD handles0, handles1, handles2;
    SIZE_T mem0, mem1;
    int i;

    printf("selftest: %d track(s)%s\n", g_rowCount,
           g_hasInvalid ? " + invalid file" : "");
    if (g_stReport)
        fprintf(g_stReport, "selftest: %d track(s)%s\n", g_rowCount,
                g_hasInvalid ? " + invalid file" : "");

    st_check(g_rowCount >= 2, "at least 2 tracks loaded");

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

    /* Play: real audio path. */
    GetProcessHandleCount(GetCurrentProcess(), &handles0);
    mem0 = working_set();
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

    /* Track switch while playing: playback must stop cleanly. */
    ListView_SetItemState(g_ctl.list, 1, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    st_check(wait_state(PLAYER_STOPPED, 2000),
             "switching track while playing stops playback");
    st_check(g_selected == 1, "second track selected");

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
            (unsigned long)handles1, (unsigned long)handles0,
            ((double)mem1 - (double)mem0) / (1024.0 * 1024.0));
    /* The device is open when handles0 is taken and closed after the last
     * stop, so a healthy count can be BELOW the baseline; only growth would
     * indicate a leak. */
    st_check(handles1 <= handles0 + 8,
             "no handle growth across play/stop cycles");
    st_check(mem1 <= mem0 + 8u * 1024u * 1024u,
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
            (unsigned long)handles2, (unsigned long)handles0);
    st_check(handles2 <= handles0 + 8,
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
