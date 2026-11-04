#include "ui_library.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "ui_layout.h"
#include "strings.h"
#include "transport.h"

/* Insert the list columns' values for one library row. Called once per row
 * when added; column order must match create_controls(). */
static void list_set_loop_cell(int idx, const TrackRow *row);

void list_insert_row(int idx, const TrackRow *row)
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

void list_refresh_loop_column(void)
{
    for (int i = 0; i < g_library.count; i++)
        list_set_loop_cell(i, &g_library.rows[i]);
}

/* Info panel with nothing to show: placeholders + loop control disabled, so
 * values from a previous track can never be mistaken for the new state. */
void clear_info_panel(void)
{
    set_name_fitted(L"\u2014");
    SetWindowTextW(g_controls.valPath, L"");
    SetWindowTextW(g_controls.valRate, L"\u2014");
    SetWindowTextW(g_controls.valCh, L"\u2014");
    SetWindowTextW(g_controls.valSamples, L"\u2014");
    SetWindowTextW(g_controls.valDur, L"\u2014");
    SetWindowTextW(g_controls.valLoop, L"\u2014");
    SetWindowTextW(g_controls.valLoopSt, tr(STR_LOOP_UNAVAILABLE));
    SetWindowTextW(g_controls.valLoopEnd, tr(STR_LOOP_UNAVAILABLE));
    EnableWindow(g_controls.checkLoop, FALSE);
}

/* Fill the info panel from a fresh snapshot. Also syncs the loop checkbox
 * with the engine (BM_SETCHECK here and in update_ui: the checkbox is
 * driven state, not the source of truth - player_set_loop is). */
void fill_info_panel(const PlayerSnapshot *s)
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
void select_track(int idx)
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
        set_system_message(tr(STR_PATH_NOT_ANSI));
        return;
    }
    err[0] = '\0';
    if (player_open(g_player, narrow, err, sizeof err) != 0) {
        wchar_t werr[512];
        MultiByteToWideChar(CP_ACP, 0, err, -1, werr, 512);
        clear_info_panel();
        swprintf(msg, 1400, tr(STR_TRACK_OPEN_FAILED), row->name, werr);
        set_status(msg);
        set_system_message(msg);
        return;
    }

    set_name_fitted(row->name);
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
    swprintf(msg, 1400, tr(STR_SYS_OPENED), row->name);
    set_system_message(msg);
}

/* Validate a file with a full brstm_open and append it to the library.
 * Validation happens here (not on selection) so bad files are rejected the
 * moment they enter the list, with a specific message per file. The opened
 * Brstm is only read for the metadata columns and closed immediately; the
 * player opens it again on selection. Returns 0 or -1 with errbuf set. */
int add_track(const wchar_t *path, wchar_t *errbuf, size_t errlen)
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
void remove_track(int idx)
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
void remove_all(void)
{
    player_stop(g_player);
    player_close(g_player);
    library_clear(&g_library);
    ListView_DeleteAllItems(g_controls.list);
    clear_info_panel();
    set_status(tr(STR_LIST_EMPTY));
}

/* File dialog (multi-select allowed). The OPENFILENAME buffer uses the
 * documented two-part layout: dir\0 then file\0 file\0 ... \0 (single file
 * = just the path). Every added file is validated by add_track; failures
 * are reported once, with a count, instead of one box per file. */
void browse_files(void)
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

/* One full UI refresh from a fresh snapshot; runs every UI_TICK_MS and
 * after every command. Deliberately idempotent: SetWindowText is only called
 * when the text actually changed, so the 10 Hz polling does not churn
 * redraws or fight the user. Sections: state text, button enables, version
 * (pair) caption, loop checkbox, device/speed labels, progress repaint, time
 * display, one-shot error messages. */
void update_ui(void)
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
void change_track(int idx, int wasPlaying)
{
    ListView_SetItemState(g_controls.list, idx, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    if (g_library.selected_index != idx)
        select_track(idx); /* no LVN_ITEMCHANGED when the row does not change */
    if (wasPlaying && player_get_state(g_player).state != PLAYER_PLAYING)
        player_play(g_player);
}
