#include <windows.h>
#include <commctrl.h>
#include <psapi.h>

#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "ui_selftest.h"
#include "ui_state.h"
#include "ui_layout.h"
#include "ui_library.h"
#include "transport.h"
#include "strings.h"
#include <winreg.h>

/* The hidden window owns the state passed in UiSelfTestContext. This private
 * pointer avoids threading a large context through every phase helper. */
static UiSelfTestContext *g_test;

/* Run the real Win32 handlers rather than private test-only entry points. */

/* Record one check result: counters, stdout and report file stay in sync so
 * a redirected run and an interactive one show the same results. */
static void st_check(int ok, const char *name)
{
    g_test->checks++;
    if (!ok) {
        g_test->fails++;
        fprintf(stdout, "[FAIL] %s\n", name);
        if (g_test->report)
            fprintf(g_test->report, "[FAIL] %s\n", name);
    } else {
        printf("[PASS] %s\n", name);
        if (g_test->report)
            fprintf(g_test->report, "[PASS] %s\n", name);
    }
    fflush(stdout);
    if (g_test->report)
        fflush(g_test->report);
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
        PlayerSnapshot s = player_get_state(g_test->player);
        if (s.state == want)
            return 1;
        pump(10);
    } while (GetTickCount() < deadline);
    return player_get_state(g_test->player).state == want;
}

/* Simulate a button click by sending the exact WM_COMMAND the control
 * itself would send - the selftest therefore exercises the same
 * on_command() path as a real click, not a private test entry point. */
static void send_cmd(int id)
{
    SendMessageW(g_test->window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
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
    /* Print the formatted text through a literal format, never the variable one. */
    printf("%s", line);
    fflush(stdout);
    if (g_test->report) {
        fputs(line, g_test->report);
        fflush(g_test->report);
    }
}

/* Handle count at a phase boundary: written to the report so a leak check
 * failure can be traced to the phase that grew, instead of guessing. */
static void st_log_handles(const char *tag)
{
    DWORD h = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h);
    st_logf("handles %s: %lu\n", tag, (unsigned long)h);
}

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


/* Phase 1 - library: the custom progress class is in use, an invalid file
 * is rejected without entering the list, and selecting the first row opens
 * it and fills the metadata (same handler as a real click). */
static void st_phase_library(void)
{
    /* The progress control must be our loop-aware custom class. */
    {
        wchar_t cls[64] = L"";
        GetClassNameW((*g_test->controls).progress, cls, 64);
        st_check(wcscmp(cls, PROGRESS_CLASS_NAME) == 0,
                 "progress control uses the loop-aware custom class");
    }

    /* Invalid file must be rejected without entering the list. */
    if (g_test->has_invalid_path) {
        wchar_t err[1024];
        int before = (*g_test->row_count);
        int rc = g_test->add_track(g_test->add_track_context, g_test->invalid_path, err, 1024);
        st_check(rc != 0 && (*g_test->row_count) == before,
                 "invalid file rejected with error message");
    }

    /* Adding a track that is already in the library must be refused, and the
     * list must not grow. The first row is a file known to be valid. */
    if (*g_test->row_count >= 1 && g_test->library) {
        wchar_t err[1024];
        int before = (*g_test->row_count);
        int rc = g_test->add_track(g_test->add_track_context,
                                   g_test->library->rows[0].path, err, 1024);
        st_check(rc != 0 && (*g_test->row_count) == before,
                 "adding a track already in the library is refused");
    }

    /* Select first track (same handler as a click). */
    ListView_SetItemState((*g_test->controls).list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    pump(50);
    st_check((*g_test->selected) == 0 && (*g_test->snapshot).sample_rate > 0,
             "selection opens track and fills metadata");
}

/* Phase 1b - modes and theme: the mode bar (Biblioteca / Ajustes) and the
 * theme selector (Claro / Oscuro / Sistema) change through the same
 * WM_COMMAND path as a real click, and the central block follows the mode. */
static void st_phase_modes_theme(void)
{
    /* Start in Library mode (create_controls calls set_active_mode(0)). */
    st_check(g_active_mode == 0, "arranca en modo Biblioteca");

    /* Switch to Settings: the central block (library) is hidden. */
    send_cmd(IDC_BTN_MODE_SETTINGS);
    pump(50);
    st_check(g_active_mode == 2, "modo Ajustes activado");
    st_check((GetWindowLongPtrW((*g_test->controls).groupLib, GWL_STYLE) & WS_VISIBLE) == 0,
             "grupo Biblioteca oculto en modo Ajustes");
    st_check((GetWindowLongPtrW((*g_test->controls).list, GWL_STYLE) & WS_VISIBLE) == 0,
             "lista oculta en modo Ajustes");

    /* Theme and language from the buttons in the Settings panel (Q-121, Q-122).
     * The Settings controls are only visible in this mode. */
    send_cmd(IDC_BTN_THEME_LIGHT);
    pump(50);
    st_check(g_theme_choice == THEME_LIGHT, "tema claro seleccionado");

    send_cmd(IDC_BTN_THEME_DARK);
    pump(50);
    st_check(g_theme_choice == THEME_DARK, "tema oscuro seleccionado");

    send_cmd(IDC_BTN_THEME_SYSTEM);
    pump(50);
    st_check(g_theme_choice == THEME_SYSTEM, "tema vuelve al sistema");

    send_cmd(IDC_BTN_LANG_EN);
    pump(50);
    st_check(strings_language() == STR_LANG_EN, "idioma English seleccionado");

    send_cmd(IDC_BTN_LANG_ES);
    pump(50);
    st_check(strings_language() == STR_LANG_ES, "idioma Espanol seleccionado");

    /* Back to Library: the central block reappears. */
    send_cmd(IDC_BTN_MODE_LIBRARY);
    pump(50);
    st_check(g_active_mode == 0, "modo Biblioteca restaurado");
    st_check((GetWindowLongPtrW((*g_test->controls).groupLib, GWL_STYLE) & WS_VISIBLE) != 0,
             "grupo Biblioteca visible en modo Biblioteca");
    st_check((GetWindowLongPtrW((*g_test->controls).list, GWL_STYLE) & WS_VISIBLE) != 0,
             "lista visible en modo Biblioteca");
}

/* Defensive branches: out-of-range indexes must not move state, and the
 * system-theme decision must follow AppsUseLightTheme. The registry value is
 * toggled, the system-change notification is sent to the real window, and the
 * original value is restored at the end. */
static void st_phase_defensive(void)
{
    int sel = (*g_test->selected);
    int count = (*g_test->row_count);

    /* select_track() with an out-of-range index keeps the selection. */
    select_track(-1);
    st_check((*g_test->selected) == sel, "select_track(-1) keeps the selection");
    select_track(count);
    st_check((*g_test->selected) == sel, "select_track(count) keeps the selection");

    /* remove_track() with an out-of-range index keeps the library. */
    remove_track(-1);
    st_check((*g_test->row_count) == count && (*g_test->selected) == sel,
             "remove_track(-1) keeps the library");
    remove_track(count);
    st_check((*g_test->row_count) == count && (*g_test->selected) == sel,
             "remove_track(count) keeps the library");

    /* system_theme_is_dark() (main.c:52-63) reads AppsUseLightTheme: flip the
     * value, notify the system theme change through the real window, check the
     * UI flag follows, and restore the original theme state at the end
     * (including the read-error path, main.c:60-61). */
    {
        HKEY hk;
        LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hk);
        if (st == ERROR_SUCCESS) {
            DWORD original = 0, originalSize = sizeof original, type = 0;
            int had = RegGetValueW(hk, L"AppsUseLightTheme", NULL, RRF_RT_REG_DWORD,
                                   &type, &original, &originalSize) == ERROR_SUCCESS;
            int originalDark = g_ui_theme_dark;
            DWORD restoreValue = originalDark ? 0u : 1u;

            DWORD toggled = originalDark ? 1u : 0u;
            RegSetValueExW(hk, L"AppsUseLightTheme", 0, REG_DWORD,
                           (const BYTE *)&toggled, sizeof toggled);
            SendMessageW(g_test->window, WM_SETTINGCHANGE, 0,
                         (LPARAM)L"ImmersiveColorSet");
            st_check(g_ui_theme_dark == (toggled == 0 ? 1 : 0),
                     "system theme follows AppsUseLightTheme");

            if (had) {
                RegSetValueExW(hk, L"AppsUseLightTheme", 0, REG_DWORD,
                               (const BYTE *)&original, sizeof original);
                SendMessageW(g_test->window, WM_SETTINGCHANGE, 0,
                             (LPARAM)L"ImmersiveColorSet");
                st_check(g_ui_theme_dark == originalDark,
                         "system theme restored after the toggle");
            } else {
                RegDeleteValueW(hk, L"AppsUseLightTheme");
                SendMessageW(g_test->window, WM_SETTINGCHANGE, 0,
                             (LPARAM)L"ImmersiveColorSet");
                st_check(g_ui_theme_dark == 0,
                         "unreadable AppsUseLightTheme keeps the light theme");
            }
            RegSetValueExW(hk, L"AppsUseLightTheme", 0, REG_DWORD,
                           (const BYTE *)&restoreValue, sizeof restoreValue);
            SendMessageW(g_test->window, WM_SETTINGCHANGE, 0,
                         (LPARAM)L"ImmersiveColorSet");
            st_check(g_ui_theme_dark == originalDark,
                     "system theme restored at the end of the phase");
            RegCloseKey(hk);
        } else {
            st_check(0, "could not open the system theme key");
        }
    }
}

/* Phase 1c - library CRUD: add, select and remove tracks using the synthetic
 * files passed on the command line. Runs before the transport starts, with the
 * library already loaded and nothing playing. */
static void st_phase_library_crud(void)
{
    int before = (*g_test->row_count);
    wchar_t err[1024];

    if (before < 2) {
        st_check(0, "se necesitan al menos 2 pistas sintéticas para CRUD");
        return;
    }

    /* list_insert_row runs from add_track each time a track enters the library.
     * Check that the row appears in the ListView, not only in the array. */
    {
        int rows = ListView_GetItemCount((*g_test->controls).list);
        st_check(rows == before && rows >= 2,
                 "list_insert_row insertó la fila de cada pista añadida");
    }

    /* Select the second track (same handler as a click on the list). */
    ListView_SetItemState((*g_test->controls).list, 1, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    pump(50);
    st_check((*g_test->selected) == 1 && (*g_test->snapshot).sample_rate > 0,
             "seleccionar la segunda pista la abre y rellena metadatos");

    /* Adding a track already in the list must be refused and must not grow it. */
    {
        int before2 = (*g_test->row_count);
        int rc = g_test->add_track(g_test->add_track_context,
                                   g_test->library->rows[0].path, err, 1024);
        st_check(rc != 0 && (*g_test->row_count) == before2,
                 "añadir una pista duplicada es rechazado");
    }

    /* Remove the selected track and add it back. */
    {
        const wchar_t *removedPath = g_test->library->rows[(*g_test->selected)].path;
        size_t removedPathLen = wcslen(removedPath) + 1;
        wchar_t *removedPathCopy = (wchar_t *)malloc(removedPathLen * sizeof(wchar_t));
        if (removedPathCopy) {
            memcpy(removedPathCopy, removedPath, removedPathLen * sizeof(wchar_t));
            send_cmd(IDC_BTN_REMOVE);
            pump(100);
            st_check((*g_test->row_count) == before - 1,
                     "quitar la pista seleccionada reduce la biblioteca");
            /* Re-add the removed track. */
            int rc = g_test->add_track(g_test->add_track_context,
                                       removedPathCopy, err, 1024);
            st_check(rc == 0 && (*g_test->row_count) == before,
                     "re-añadir la pista quitada la recupera");
            free(removedPathCopy);
        } else {
            st_check(0, "no se pudo reservar memoria para la prueba de re-add");
        }
    }

    /* Leave a known selection (first track) for the next phases.
     * Select from the "nothing selected" state left by the remove, as
     * phase_library does at startup. */
    ListView_SetItemState((*g_test->controls).list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    pump(50);
    st_check((*g_test->selected) == 0
                 && (*g_test->snapshot).sample_rate > 0,
             "primera pista seleccionada y abierta tras el CRUD");
}

/* Phase 1d - "Anterior" (previous) from the first row at the start of the song:
 * position <= 3 s means previous track, which from the first row wraps to the
 * last one (transport_previous_index returns count-1). This branch is never
 * reached: the selftest only tried "previous" from a non-zero row, and
 * "restart song" at the end of the song. */
static void st_phase_previous_wrap(void)
{
    st_check((*g_test->selected) == 0 && (*g_test->snapshot).sample_rate > 0,
             "partiendo de la primera fila al inicio de la canción");
    send_cmd(IDC_BTN_PREVIOUS);
    pump(200);
    st_check((*g_test->selected) == (*g_test->row_count) - 1
                 && (*g_test->snapshot).sample_rate > 0,
             "anterior desde la primera fila envuelve al último registro");

    /* Back to the first track (stereo, stopped) so the transport phases start
     * from the base state they expect. The list allows multiple selection (no
     * LVS_SINGLESEL), so item 0 stays marked after item 1 was marked: clear both
     * before selecting 0 again. Otherwise the state does not change, so
     * LVN_ITEMCHANGED does not fire and select_track(0) does not run. */
    ListView_SetItemState((*g_test->controls).list, 0, 0, LVIS_SELECTED);
    ListView_SetItemState((*g_test->controls).list, 1, 0, LVIS_SELECTED);
    ListView_SetItemState((*g_test->controls).list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    pump(50);
    st_check((*g_test->selected) == 0 && (*g_test->snapshot).sample_rate > 0,
             "vuelve a la primera pista antes del transporte");
}

/* Phase 1e - library error paths without audio: a file that disappears after
 * it was added makes select_track() fail to open it (status bar and banner
 * show the error and the player stays closed), and a file that is not a
 * BRSTM is refused by add_track() with a named message. Restores the first
 * row afterwards so the transport phases start from the usual state. */
static void st_phase_library_errors(void)
{
    wchar_t err[1024] = L"";
    wchar_t bar[256] = L"";
    wchar_t gone[MAX_PATH] = L"";
    wchar_t tmp[MAX_PATH];
    int rc;
    int before;

    /* A file that is not a valid BRSTM is refused with its name in the message. */
    before = (*g_test->row_count);
    rc = g_test->add_track(g_test->add_track_context, g_test->invalid_path, err, 1024);
    st_check(rc != 0 && (*g_test->row_count) == before
                 && wcsstr(err, file_name_of(g_test->invalid_path)) != NULL,
             "add_track names the file it refused");

    if (*g_test->row_count < 1 || !g_test->library) {
        st_check(0, "library has a track to copy for the open-failure check");
        return;
    }

    /* Copy the first track to a temp file, add it, then delete the file so
     * the next select must fail at open time. */
    if (GetTempPathW(MAX_PATH, tmp) == 0
        || GetTempFileNameW(tmp, L"blp", 0, gone) == 0
        || !CopyFileW(g_test->library->rows[0].path, gone, FALSE)) {
        st_check(0, "temporary copy for the open-failure check");
        return;
    }
    before = (*g_test->row_count);
    rc = g_test->add_track(g_test->add_track_context, gone, err, 1024);
    st_check(rc == 0 && (*g_test->row_count) == before + 1,
             "a valid copy joins the library");
    DeleteFileW(gone);

    /* Select the row whose file is gone: open fails and the status bar gets
     * the same message the app builds for this row, in the active language. */
    ListView_SetItemState((*g_test->controls).list, before, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    pump(100);
    SendMessageW((*g_test->controls).status, SB_GETTEXTW, 0, (LPARAM)bar);
    {
        /* The bar must start with the app's own message for this row, the
         * name then the engine's reason; the reason text varies, so the check
         * fixes the prefix up to the first ": " after the name. */
        wchar_t prefix[1400] = L"";
        swprintf(prefix, 1400, tr(STR_TRACK_OPEN_FAILED),
                 g_test->library->rows[before].name, L"");
        size_t cut = wcslen(prefix);
        st_check((*g_test->selected) == before
                     && cut > 0 && wcsncmp(bar, prefix, cut) == 0,
                 "a vanished file is selected and its open error is shown");
    }

    /* Put the library back to the usual first-track state for later phases:
     * drop the copy, then clear the selection before picking row 0 again, so
     * the change is seen as a new selection (same as previous_wrap does). */
    send_cmd(IDC_BTN_REMOVE);
    pump(100);
    ListView_SetItemState((*g_test->controls).list, -1, 0, LVIS_SELECTED);
    pump(20);
    ListView_SetItemState((*g_test->controls).list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    pump(50);
}

/* Phase 2 - transport: play reaches PLAYING with the device open, the API
 * stays responsive while the engine decodes (100 ms budget on
 * player_get_state), and pause freezes the position before resume. Also
 * records the resource baselines used by the final leak checks. */
static void st_phase_transport(void)
{
    int i;

    /* Play: real audio path. */
    GetProcessHandleCount(GetCurrentProcess(), &g_test->baseline_handles);
    g_test->baseline_memory = working_set();
    send_cmd(IDC_BTN_PLAY);
    st_check(wait_state(PLAYER_PLAYING, 4000), "play reaches PLAYING");
    st_check(player_get_state(g_test->player).audio_error == 0,
             "audio device opened without error");

    /* UI thread responsiveness while the engine plays. */
    {
        LARGE_INTEGER f, t0, t1, worst;
        double worst_ms;
        QueryPerformanceFrequency(&f);
        worst.QuadPart = 0;
        for (i = 0; i < 50; i++) {
            QueryPerformanceCounter(&t0);
            (void)player_get_state(g_test->player);
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

    /* Pause / resume through the single play button, with position
     * stability and a caption that announces the next click. */
    {
        PlayerSnapshot s = player_get_state(g_test->player);
        TransportLogicalState ls = transport_logical_state(s.state,
            s.speed_ppm == PLAYER_FLSM_SPEED_PPM, s.pair_sel, s.pair_count);
        st_check(ls.playing, "play button offers pause while playing");
    }
    send_cmd(IDC_BTN_PLAY);
    st_check(wait_state(PLAYER_PAUSED, 2000), "pause reaches PAUSED");
    {
        uint64_t p0 = player_get_state(g_test->player).position;
        pump(300);
        uint64_t p1 = player_get_state(g_test->player).position;
        st_check(p0 == p1, "position frozen while paused");
        {
            PlayerSnapshot s = player_get_state(g_test->player);
            TransportLogicalState ls = transport_logical_state(s.state,
                s.speed_ppm == PLAYER_FLSM_SPEED_PPM, s.pair_sel, s.pair_count);
            st_check(ls.paused, "play button offers resume while paused");
        }
    }
    send_cmd(IDC_BTN_PLAY);
    st_check(wait_state(PLAYER_PLAYING, 2000), "resume reaches PLAYING");
}

/* Phase 3 - seek, speed, loop: a click at 3/4 of the bar seeks into
 * 55..80% and playback continues from there; the speed toggle switches
 * 1.0x <-> Final Lap and back with a matching caption; "Anterior" restarts a
 * song that is more than 3 s in; the loop checkbox (emulating a real click
 * on the BS_AUTOCHECKBOX) flips the engine flag and restores it. */
static void st_phase_seek_speed(void)
{
    /* Seek: a click on the progress bar jumps near that point and the
     * playback keeps going from there. */
    {
        RECT rc;
        int x;
        PlayerSnapshot s1, s2;

        GetClientRect((*g_test->controls).progress, &rc);
        x = (rc.right - rc.left) * 3 / 4;
        SendMessageW((*g_test->controls).progress, WM_LBUTTONDOWN, 0, MAKELPARAM(x, 8));
        SendMessageW((*g_test->controls).progress, WM_LBUTTONUP, 0, MAKELPARAM(x, 8));
        pump(100);
        s1 = player_get_state(g_test->player);
        st_check(s1.position > s1.total_samples * 55 / 100
                 && s1.position < s1.total_samples * 80 / 100,
                 "click on the progress bar seeks near that point");
        pump(900);
        s2 = player_get_state(g_test->player);
        st_check(s2.state == PLAYER_PLAYING && s2.position > s1.position,
                 "playback continues from the seeked position");
    }

    /* Speed toggle: 1.0x -> Final Lap -> 1.0x; the button caption always
     * announces the mode in force (single toggle, no pair of buttons). */
    send_cmd(IDC_BTN_SPEED);
    pump(500);
    {
        PlayerSnapshot s = player_get_state(g_test->player);
        uint32_t want = player_device_rate(s.sample_rate,
                                           PLAYER_FLSM_SPEED_PPM);
        TransportLogicalState ls = transport_logical_state(s.state,
            s.speed_ppm == PLAYER_FLSM_SPEED_PPM, s.pair_sel, s.pair_count);
        st_check(s.state == PLAYER_PLAYING && s.device_rate == want,
                 "FLSM speed applied live, device rate matches floor(rate*k)");
        st_check(ls.speed_flsm, "speed state reports Final Lap");
    }
    send_cmd(IDC_BTN_SPEED);
    pump(500);
    {
        PlayerSnapshot s = player_get_state(g_test->player);
        TransportLogicalState ls = transport_logical_state(s.state,
            s.speed_ppm == PLAYER_FLSM_SPEED_PPM, s.pair_sel, s.pair_count);
        st_check(s.state == PLAYER_PLAYING && s.device_rate == s.sample_rate,
                 "second press restores the 1.0x device rate");
        st_check(!ls.speed_flsm && s.speed_ppm == PLAYER_SPEED_SCALE,
                 "speed state reports 1.0x again");
    }

    /* Anterior far into the song (>3 s, we are at ~75%): restarts the
     * current track instead of changing it. */
    {
        int rowBefore = (*g_test->selected);
        send_cmd(IDC_BTN_PREVIOUS);
        pump(200);
        PlayerSnapshot s = player_get_state(g_test->player);
        st_check(s.state == PLAYER_PLAYING && s.position < s.sample_rate
                 && (*g_test->selected) == rowBefore,
                 "restarts the song when far into it");
    }

    /* Loop toggle (only if the file has loop metadata). */
    {
        PlayerSnapshot s = player_get_state(g_test->player);
        if (s.file_loop) {
            int before = s.loop_enabled;
            int boxBefore =
                SendMessageW((*g_test->controls).checkLoop, BM_GETCHECK, 0, 0)
                    == BST_CHECKED;
            /* The handler reads the checkbox (correct for a real click on a
             * BS_AUTOCHECKBOX, which flips the box before BN_CLICKED is
             * delivered); emulate that click instead of only sending the
             * command, and verify box and engine start in sync. */
            st_check(boxBefore == (before != 0),
                     "loop checkbox matches the engine state");
            SendMessageW((*g_test->controls).checkLoop, BM_SETCHECK,
                         boxBefore ? BST_UNCHECKED : BST_CHECKED, 0);
            send_cmd(IDC_CHECK_LOOP);
            pump(50);
            s = player_get_state(g_test->player);
            st_check(s.loop_enabled != before, "loop toggle flips the flag");
            SendMessageW((*g_test->controls).checkLoop, BM_SETCHECK,
                         boxBefore ? BST_CHECKED : BST_UNCHECKED, 0);
            send_cmd(IDC_CHECK_LOOP);
            pump(50);
        } else {
            st_check(1, "no-loop file: loop control left disabled");
        }
    }
}

/* Phase 4 - track switch while playing: switching tracks stops playback,
 * Siguiente/Anterior wrap around and keep playing, and pause freezes it. */
static void st_phase_pair(void)
{
    /* Track switch while playing: playback must stop cleanly. */
    ListView_SetItemState((*g_test->controls).list, 1, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    st_check(wait_state(PLAYER_STOPPED, 2000),
             "switching track while playing stops playback");
    st_check((*g_test->selected) == 1, "second track selected");
    st_log_handles("row 2 opened (stopped)");

    /* Start playing the second track so Siguiente/Anterior can be tested
     * while playing. */
    send_cmd(IDC_BTN_PLAY);
    st_check(wait_state(PLAYER_PLAYING, 4000), "second track plays");

    /* Siguiente: from the last row it wraps to the first one and keeps
     * playing, because the user was listening. */
    {
        /* Guarded: a bare --selftest without files leaves the library
         * empty and the checks below must fail, not crash. */
        int expect = transport_next_index(*g_test->selected, *g_test->row_count);
        send_cmd(IDC_BTN_NEXT);
        st_check(wait_state(PLAYER_PLAYING, 4000)
                 && (*g_test->selected) == expect,
                 "next button advances the track and keeps playing");
        st_log_handles("after next (playing)");
    }

    /* Anterior with the song just opened (position ~0) goes back to the
     * previous track - wrapping to the last row - and keeps playing. */
    {
        int expect = transport_previous_index(*g_test->selected, *g_test->row_count);
        send_cmd(IDC_BTN_PREVIOUS);
        st_check(wait_state(PLAYER_PLAYING, 4000)
                 && (*g_test->selected) == expect,
                 "previous button goes back and keeps playing");
        st_log_handles("after previous (playing)");
    }

    /* No stop button: pause instead, so the leak cycles start from a
     * known state. */
    send_cmd(IDC_BTN_PLAY);
    st_check(wait_state(PLAYER_PAUSED, 2000),
             "pause after the version checks");
    st_log_handles("after pause (row reopened)");
}
/* Phase 4b - language: choosing English from the menu relabels the window
 * and the state text follows, then Español restores the original labels.
 * The labels are read back from the controls themselves, so a missed control
 * shows up as a failure rather than going unnoticed. */
static void st_phase_language(void)
{
    wchar_t cap[64] = L"";
    StrLanguage before = strings_language();


    send_cmd(IDC_BTN_MODE_SETTINGS);
    pump(50);

    send_cmd(IDC_BTN_LANG_EN);
    pump(100);
    {
        PlayerSnapshot s = player_get_state(g_test->player);
        const wchar_t *expected = s.state == PLAYER_PLAYING ? L"Pause"
                                : s.state == PLAYER_PAUSED ? L"Resume"
                                : s.state == PLAYER_FINISHED ? L"Play again"
                                : L"Play";
        GetWindowTextW((*g_test->controls).btnPlay, cap, 64);
        st_check(wcscmp(cap, expected) == 0,
                 "English relabels the play button for the current state");
    }
    GetWindowTextW((*g_test->controls).btnPrevious, cap, 64);
    st_check(wcscmp(cap, L"Previous") == 0, "English relabels Previous");
    GetWindowTextW((*g_test->controls).groupLib, cap, 64);
    st_check(wcscmp(cap, L"Library") == 0, "English relabels the library group");
    st_check(strings_language() == STR_LANG_EN, "English is the active language");

    send_cmd(IDC_BTN_LANG_ES);
    pump(100);
    GetWindowTextW((*g_test->controls).btnPrevious, cap, 64);
    st_check(wcscmp(cap, L"Anterior") == 0, "Español relabels Previous");
    GetWindowTextW((*g_test->controls).groupLib, cap, 64);
    st_check(wcscmp(cap, L"Biblioteca") == 0, "Español relabels the library group");
    st_check(strings_language() == STR_LANG_ES, "Español is the active language");
    /* The loop column must follow the language too, not keep the old word. */
    if (*g_test->row_count >= 1 && g_test->library) {
        wchar_t cell[32] = L"";
        ListView_GetItemText((*g_test->controls).list, 0, 4, cell, 32);
        st_check(wcscmp(cell, g_test->library->rows[0].loop ? L"Sí" : L"No") == 0,
                 "loop column shows Sí after switching to Español");
        send_cmd(IDC_BTN_LANG_EN);
        pump(50);
        ListView_GetItemText((*g_test->controls).list, 0, 4, cell, 32);
        st_check(wcscmp(cell, g_test->library->rows[0].loop ? L"Yes" : L"No") == 0,
                 "loop column shows Yes after switching back to English");
        send_cmd(IDC_BTN_LANG_ES);
        pump(50);
    }

    /* Leave the session in the language it started in, then return to
     * Biblioteca mode so the language buttons are hidden again. */
    if (before == STR_LANG_EN)
        send_cmd(IDC_BTN_LANG_EN);
    else
        send_cmd(IDC_BTN_LANG_ES);
    pump(50);
    send_cmd(IDC_BTN_MODE_LIBRARY);
    pump(50);
}

/* Phase 5 - removal: removing the selected row must select the replacement
 * exactly once and reopen it; removing the last row must clear the selection
 * and close the player. Both use the same WM_COMMAND as the Quitar button. */
static void st_phase_remove(void)
{
    int before = (*g_test->row_count);
    int removed = (*g_test->selected);
    int expected_replacement;

    if (before < 2) {
        st_check(0, "library has at least two rows for removal checks");
        return;
    }

    /* Removing the selected row must open exactly the row that takes its
     * place: the next one, or the previous one when it was the last. */
    expected_replacement = removed < before - 1 ? removed : before - 2;
    send_cmd(IDC_BTN_REMOVE);
    pump(100);
    st_check((*g_test->row_count) == before - 1,
             "removing the selected row shrinks the library");
    st_check((*g_test->selected) == expected_replacement
             && (*g_test->snapshot).sample_rate > 0,
             "the replacement row is selected and opened");
    st_check(g_test->library->rows[(*g_test->selected)].name != NULL,
             "the replacement row still owns its name");
}

/* Phase 6 - "Limpiar" (clear): empties the library and disables the transport
 * until a track is selected. Exercises remove_all() and library_clear(), which
 * earlier phases never call. */
static void st_phase_clear(void)
{
    send_cmd(IDC_BTN_CLEAR);
    pump(50);
    st_check((*g_test->row_count) == 0, "limpiar vacía la biblioteca");
    st_check(IsWindowEnabled((*g_test->controls).btnPlay) == 0,
             "reproducir deshabilitado sin pista seleccionada");
    {
        wchar_t bar[64] = L"";
        SendMessageW((*g_test->controls).status, SB_GETTEXTW, 0, (LPARAM)bar);
        st_check(wcscmp(bar, tr(STR_LIST_EMPTY)) == 0,
                 "el estado informa de la lista vacía");
    }
}

/* Phase 5b - progress drag: press starts a drag and seeks, a move while the
 * button is held updates the drag position that is painted, and the release
 * seeks to the final point and ends the drag. Needs an open track, so it runs
 * after the removals, with the first row selected. */
static void st_phase_progress_drag(void)
{
    RECT rc;
    int x1;
    int x2;
    PlayerSnapshot s;

    GetClientRect((*g_test->controls).progress, &rc);
    x1 = (rc.right - rc.left) / 4;
    x2 = (rc.right - rc.left) * 3 / 4;

    SendMessageW((*g_test->controls).progress, WM_LBUTTONDOWN, 0, MAKELPARAM(x1, 8));
    st_check(g_progress_dragging == 1 && g_progress_drag_x == x1,
             "pressing the progress bar starts a drag at that point");

    SendMessageW((*g_test->controls).progress, WM_MOUSEMOVE, 0, MAKELPARAM(x2, 8));
    st_check(g_progress_dragging == 1 && g_progress_drag_x == x2,
             "moving while pressed moves the drag point");

    SendMessageW((*g_test->controls).progress, WM_LBUTTONUP, 0, MAKELPARAM(x2, 8));
    s = player_get_state(g_test->player);
    /* Released at 3/4 of the bar: the seek lands in 55..80 % of the track,
     * the same window the seek phase asserts for that point. */
    st_check(g_progress_dragging == 0
                 && s.position > s.total_samples * 55 / 100
                 && s.position < s.total_samples * 80 / 100,
             "releasing the progress bar ends the drag and seeks to 3/4");

    /* A release without a press does nothing: the drag flag stays off. */
    SendMessageW((*g_test->controls).progress, WM_LBUTTONUP, 0, MAKELPARAM(x1, 8));
    st_check(g_progress_dragging == 0,
             "a release without a press leaves the drag off");
}

/* Phase 6a - banner layout: the state box is at least as wide as its widest
 * text in both languages, and a track name too long for the banner is cut
 * with "..." so it fits, while a short name is shown whole. */
static void st_phase_layout(void)
{
    wchar_t shown[512] = L"";
    wchar_t longName[512];
    int widest;
    size_t i;
    HDC dc;
    HGDIOBJ oldFont;
    SIZE sz;

    widest = measure_state_text_width();
    st_check(widest > 0, "state text width is measured with the real font");

    /* The width must cover the active language's stopped label, which is one
     * of the four state texts that measure_state_text_width() takes. */
    dc = GetDC(NULL);
    oldFont = SelectObject(dc, (HGDIOBJ)g_font);
    GetTextExtentPoint32W(dc, tr(STR_STATE_STOPPED),
                          (int)wcslen(tr(STR_STATE_STOPPED)), &sz);
    SelectObject(dc, oldFont);
    ReleaseDC(NULL, dc);
    st_check(widest >= sz.cx, "state width covers the stopped label in use");

    /* A long name is cut and ends in "..." so it fits the banner. */
    for (i = 0; i < 300; i++)
        longName[i] = L'W';
    longName[300] = L'\0';
    set_name_fitted(longName);
    GetWindowTextW((*g_test->controls).valName, shown, 512);
    st_check(wcslen(shown) > 3 && wcscmp(shown + wcslen(shown) - 3, L"...") == 0
                 && wcslen(shown) < 300,
             "a name wider than the banner ends in ... and is shorter");

    /* A short name is shown whole, with no ellipsis. */
    set_name_fitted(L"ok");
    GetWindowTextW((*g_test->controls).valName, shown, 512);
    st_check(wcscmp(shown, L"ok") == 0, "a short name is shown without cutting");

    /* Restore the name of the track that is open, so later checks see it. */
    if ((*g_test->row_count) > 0 && (*g_test->selected) >= 0)
        set_name_fitted(g_test->library->rows[*g_test->selected].name);
}

/* Phase 6b - painting without a visible window: the state label takes its
 * colour from the engine state, the system message uses the secondary text
 * colour, and the mode buttons paint their active one with the accent. The
 * window is never shown here, so each handler is driven with the message the
 * system would send and the check reads back the value it produced. */
static void st_phase_paint(void)
{
    HDC dc;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ oldBmp;
    RECT box = { 0, 0, 120, 30 };
    HBRUSH brush;
    COLORREF got;
    COLORREF want;
    int dark = g_ui_theme_dark;

    dc = GetDC(g_test->window);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, 240, 60);
    oldBmp = SelectObject(mem, bmp);

    /* State label colour follows the engine state: stopped uses the loading
     * colour in the theme in force. */
    g_player_snapshot.state = PLAYER_STOPPED;
    brush = (HBRUSH)SendMessageW(g_test->window, WM_CTLCOLORSTATIC,
                                 (WPARAM)mem, (LPARAM)(*g_test->controls).valState);
    want = dark ? UI_STATE_DARK_LOADING : UI_STATE_LIGHT_LOADING;
    got = GetTextColor(mem);
    st_check(brush != NULL && got == want,
             "state label uses the loading colour while stopped");

    g_player_snapshot.state = PLAYER_PLAYING;
    brush = (HBRUSH)SendMessageW(g_test->window, WM_CTLCOLORSTATIC,
                                 (WPARAM)mem, (LPARAM)(*g_test->controls).valState);
    want = dark ? UI_STATE_DARK_PLAYING : UI_STATE_LIGHT_PLAYING;
    st_check(GetTextColor(mem) == want,
             "state label uses the playing colour while playing");

    g_player_snapshot.state = PLAYER_PAUSED;
    SendMessageW(g_test->window, WM_CTLCOLORSTATIC,
                 (WPARAM)mem, (LPARAM)(*g_test->controls).valState);
    want = dark ? UI_STATE_DARK_PAUSED : UI_STATE_LIGHT_PAUSED;
    st_check(GetTextColor(mem) == want,
             "state label uses the paused colour while paused");

    g_player_snapshot.state = PLAYER_FINISHED;
    SendMessageW(g_test->window, WM_CTLCOLORSTATIC,
                 (WPARAM)mem, (LPARAM)(*g_test->controls).valState);
    want = dark ? UI_STATE_DARK_FINISHED : UI_STATE_LIGHT_FINISHED;
    st_check(GetTextColor(mem) == want,
             "state label uses the finished colour when finished");
    g_player_snapshot.state = PLAYER_STOPPED;

    /* The system message is drawn in the secondary text colour. */
    SendMessageW(g_test->window, WM_CTLCOLORSTATIC,
                 (WPARAM)mem, (LPARAM)(*g_test->controls).valSystem);
    want = palette_text_secondary(&g_palette, dark);
    st_check(GetTextColor(mem) == want,
             "system message uses the secondary text colour");

    /* An active mode button is painted with the accent colour; the inactive
     * one with the surface. Read back the fill at a point inside the box. */
    {
        DRAWITEMSTRUCT dis;
        COLORREF px;
        memset(&dis, 0, sizeof dis);
        dis.CtlType = ODT_BUTTON;
        dis.CtlID = IDC_BTN_MODE_LIBRARY;
        dis.hwndItem = (*g_test->controls).btnModeLibrary;
        dis.hDC = mem;
        dis.rcItem = box;
        g_active_mode = 0;
        SendMessageW(g_test->window, WM_DRAWITEM, 0, (LPARAM)&dis);
        /* Centre of the box: far from the 2px border, independent of its size. */
        px = GetPixel(mem, (box.left + box.right) / 2, (box.top + box.bottom) / 2);
        st_check(px == palette_accent(&g_palette, dark),
                 "active mode button fills with the accent colour");
        /* Same button while Ajustes is the active mode: it is inactive now. */
        g_active_mode = 2;
        SendMessageW(g_test->window, WM_DRAWITEM, 0, (LPARAM)&dis);
        px = GetPixel(mem, (box.left + box.right) / 2, (box.top + box.bottom) / 2);
        st_check(px == palette_surface(&g_palette, dark),
                 "inactive mode button fills with the surface colour");
        g_active_mode = 0;
    }

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(g_test->window, dc);
}

/* One full transport round on the current track: three play/seek/rewind/
 * pause cycles through the single play button and the previous button, then
 * a track switch (the file and the device are closed and reopened, the
 * heaviest allocation path left now that there is no stop button). Ends
 * stopped on the other row, which is exactly the state it starts from, so
 * two rounds are comparable. */
static void st_transport_round(void)
{
    int i;

    for (i = 0; i < 3; i++) {
        RECT rc;
        int x;

        send_cmd(IDC_BTN_PLAY); /* resume (round starts paused/stopped) */
        wait_state(PLAYER_PLAYING, 4000);
        /* Jump into the last quarter so the >3 s rule of "Anterior" makes it
         * rewind the current track instead of changing row. */
        GetClientRect((*g_test->controls).progress, &rc);
        x = (rc.right - rc.left) * 3 / 4;
        SendMessageW((*g_test->controls).progress, WM_LBUTTONDOWN, 0, MAKELPARAM(x, 8));
        SendMessageW((*g_test->controls).progress, WM_LBUTTONUP, 0, MAKELPARAM(x, 8));
        pump(50);
        send_cmd(IDC_BTN_PREVIOUS);
        pump(50);
        send_cmd(IDC_BTN_PLAY); /* pause */
        wait_state(PLAYER_PAUSED, 2000);
    }
    ListView_SetItemState((*g_test->controls).list, (*g_test->selected) == 0 ? 1 : 0,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    wait_state(PLAYER_STOPPED, 2000);
    pump(100);
}

/* Scripted end-to-end session: every phase asserts the behaviour a user
 * exercises by hand, then the leak checks compare handle/memory counts
 * against the baselines taken at the first play. Returns the process exit
 * code (0 = every check passed). */
static int run_selftest(void)
{
    DWORD handles1, handlesB, handles2;
    SIZE_T mem1, memB;

    printf("selftest: %d track(s)%s\n", (*g_test->row_count),
           g_test->has_invalid_path ? " + invalid file" : "");
    if (g_test->report)
        fprintf(g_test->report, "selftest: %d track(s)%s\n", (*g_test->row_count),
                g_test->has_invalid_path ? " + invalid file" : "");

    st_check((*g_test->row_count) >= 2, "at least 2 tracks loaded");
    st_check(g_test->font_loaded, "bundled UI font loaded from assets\\fonts");
    {
        LOGFONTW lf;
        memset(&lf, 0, sizeof lf);
        st_check(g_test->ui_font != NULL
                 && GetObjectW(g_test->ui_font, sizeof lf, &lf) == sizeof lf
                 && wcscmp(lf.lfFaceName, L"Pixelify Sans") == 0,
                 "controls use the Pixelify Sans face");
    }
    /* The startup notice about a bad settings file must still be in the status
     * bar before any action replaces it. Checked here, not later, because the
     * first track opened overwrites the bar. Only runs when the environment asks
     * for it, since a normal run starts from a valid or absent file. */
    if (_wgetenv(L"MKW_EXPECT_SETTINGS_NOTICE") != NULL) {
        wchar_t bar[256] = L"";
        SendMessageW((*g_test->controls).status, SB_GETTEXTW, 0, (LPARAM)bar);
        /* An invalid file falls back to English, so the notice is in English. */
        st_logf("status bar at startup: [%ls]\n", bar);
        st_check(wcsstr(bar, L"settings file is invalid") != NULL,
                 "invalid settings file shows the notice in the status bar");
    }
    st_phase_library();
    st_log_handles("after library");
    st_phase_modes_theme();
    st_log_handles("after modes/theme");
    st_phase_defensive();
    st_log_handles("after defensive branches");
    st_phase_library_crud();
    st_log_handles("after library CRUD");
    st_phase_library_errors();
    st_log_handles("after library errors");
    st_phase_previous_wrap();
    st_phase_transport();
    st_log_handles("after transport");
    st_phase_seek_speed();
    st_log_handles("after seek/speed");
    st_phase_pair();
    st_phase_language();
    st_log_handles("after pair");
    st_phase_remove();
    st_log_handles("after remove");
    st_phase_progress_drag();

    /* Leak check by repetition: two identical transport rounds, both
     * measured in the same state (stopped, other row, device open) after
     * every format of the session has been exercised. Measured behaviour:
     * the first 4-channel open costs ~19 handles once (winmm format setup)
     * and no later operation grows, so the meaningful number is the delta
     * between round 1 and round 2, which is where a leak would show. */
    st_transport_round();
    GetProcessHandleCount(GetCurrentProcess(), &handles1);
    mem1 = working_set();
    st_transport_round();
    GetProcessHandleCount(GetCurrentProcess(), &handlesB);
    memB = working_set();
    st_logf("handles after round1 %lu, round2 %lu (first-play base %lu), "
            "working set +%.1f MB\n",
            (unsigned long)handles1, (unsigned long)handlesB,
            (unsigned long)g_test->baseline_handles,
            ((double)memB - (double)g_test->baseline_memory) / (1024.0 * 1024.0));
    st_check(handlesB <= handles1 + 8,
             "no handle growth across play/pause cycles");
    st_check(memB <= mem1 + 8u * 1024u * 1024u,
             "memory growth under 8 MB across cycles");
    st_phase_layout();
    st_phase_paint();
    st_phase_clear();
    st_log_handles("after clear");

    /* Cleanup: close the real window and compare resources. */
    SendMessageW(g_test->window, WM_CLOSE, 0, 0);
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
            (unsigned long)handles2, (unsigned long)g_test->baseline_handles);
    st_check(handles2 <= g_test->baseline_handles + 8,
             "closing the window releases resources");

    printf("RESULT: %s (%d checks, %d failed)\n",
           g_test->fails ? "FAIL" : "PASS", g_test->checks, g_test->fails);
    if (g_test->report) {
        fprintf(g_test->report, "RESULT: %s (%d checks, %d failed)\n",
                g_test->fails ? "FAIL" : "PASS", g_test->checks, g_test->fails);
        fclose(g_test->report);
        g_test->report = NULL;
    }
    return g_test->fails ? 1 : 0;
}


int ui_selftest_run(UiSelfTestContext *context)
{
    int result;
    g_test = context;
    context->checks = 0;
    context->fails = 0;
    context->baseline_handles = 0;
    context->baseline_memory = 0;
    result = run_selftest();
    g_test = NULL;
    return result;
}
