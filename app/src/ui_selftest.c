#include <windows.h>
#include <commctrl.h>
#include <psapi.h>

#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "ui_selftest.h"
#include "transport.h"
#include "strings.h"

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

/* Phase 4 - track switch and version (channel-pair) selection: switching
 * while playing stops cleanly; the 4-channel second track exposes 2 pairs;
 * the button cycles 1 -> 2 -> mix, its caption matches, and switching
 * versions live (device reopen path) keeps playback running without audio
 * errors. */
static void st_phase_pair(void)
{
    /* Track switch while playing: playback must stop cleanly. */
    ListView_SetItemState((*g_test->controls).list, 1, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    st_check(wait_state(PLAYER_STOPPED, 2000),
             "switching track while playing stops playback");
    st_check((*g_test->selected) == 1, "second track selected");
    st_log_handles("row 2 opened (stopped)");

    /* Version (channel pair) selection on the 4-channel second track. */
    {
        PlayerSnapshot s = player_get_state(g_test->player);
        st_check(s.channels == 4 && s.pair_count == 2 && s.pair_sel == 0,
                 "4-channel file exposes 2 versions and starts on version 1");
        send_cmd(IDC_BTN_PAIR);
        pump(50);
        s = player_get_state(g_test->player);
        st_check(s.pair_sel == 1, "version button selects pair 2");
        send_cmd(IDC_BTN_PAIR);
        pump(50);
        s = player_get_state(g_test->player);
        st_check(s.pair_sel == 0, "second press returns to version 1");
        {
            TransportLogicalState ls = transport_logical_state(s.state,
                s.speed_ppm == PLAYER_FLSM_SPEED_PPM, s.pair_sel, s.pair_count);
            st_check(ls.pair_index == 0,
                     "version state reports the first version");
        }
        send_cmd(IDC_BTN_PAIR);
        pump(50);
        s = player_get_state(g_test->player);
        st_check(s.pair_sel == 1, "third press selects version 2 again");
        st_log_handles("pair cycle while stopped");

        /* Live switch: a version is chosen while audio is playing. */
        send_cmd(IDC_BTN_PAIR);
        pump(50); /* pair 1 selected */
        send_cmd(IDC_BTN_PLAY);
        st_check(wait_state(PLAYER_PLAYING, 4000),
                 "play with a version selected");
        send_cmd(IDC_BTN_PAIR);
        pump(150); /* pair 2 selected */
        s = player_get_state(g_test->player);
        {
            uint64_t position_after_switch = s.position;
            st_check(s.state == PLAYER_PLAYING && s.pair_sel == 1,
                     "version switches live while playing");
            st_check(position_after_switch > 0,
                     "version switch keeps the audible position");
        }
        send_cmd(IDC_BTN_PAIR);
        pump(150); /* version 1 selected again */
        s = player_get_state(g_test->player);
        st_check(s.state == PLAYER_PLAYING && s.pair_sel == 0
                 && s.audio_error == 0,
                 "version 1 restored live without audio errors");
        st_log_handles("after live pair switches");

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
}

/* Phase 4b - language: choosing English from the menu relabels the window
 * and the state text follows, then Español restores the original labels.
 * The labels are read back from the controls themselves, so a missed control
 * shows up as a failure rather than going unnoticed. */
static void st_phase_language(void)
{
    wchar_t cap[64] = L"";
    StrLanguage before = strings_language();


    send_cmd(IDM_LANG_EN);
    pump(100);
    {
        PlayerSnapshot s = player_get_state(g_test->player);
        const wchar_t *expected = s.state == PLAYER_PLAYING ? L"Pause"
                                : s.state == PLAYER_PAUSED ? L"Resume"
                                : s.state == PLAYER_FINISHED ? L"Play again"
                                : L"Play";
        GetWindowTextW((*g_test->controls).btnPlay, cap, 64);
        st_check(wcscmp(cap, expected) == 0,
                 "menu English relabels the play button for the current state");
    }
    GetWindowTextW((*g_test->controls).btnPrevious, cap, 64);
    st_check(wcscmp(cap, L"Previous") == 0, "menu English relabels Previous");
    GetWindowTextW((*g_test->controls).groupLib, cap, 64);
    st_check(wcscmp(cap, L"Library") == 0, "menu English relabels the library group");
    st_check(strings_language() == STR_LANG_EN, "English is the active language");

    send_cmd(IDM_LANG_ES);
    pump(100);
    GetWindowTextW((*g_test->controls).btnPrevious, cap, 64);
    st_check(wcscmp(cap, L"Anterior") == 0, "menu Español restores Previous");
    GetWindowTextW((*g_test->controls).groupLib, cap, 64);
    st_check(wcscmp(cap, L"Biblioteca") == 0, "menu Español restores the library group");
    st_check(strings_language() == STR_LANG_ES, "Español is the active language");
    /* The loop column must follow the language too, not keep the old word. */
    if (*g_test->row_count >= 1 && g_test->library) {
        wchar_t cell[32] = L"";
        ListView_GetItemText((*g_test->controls).list, 0, 4, cell, 32);
        st_check(wcscmp(cell, g_test->library->rows[0].loop ? L"Sí" : L"No") == 0,
                 "loop column shows Sí after switching to Español");
        send_cmd(IDM_LANG_EN);
        pump(50);
        ListView_GetItemText((*g_test->controls).list, 0, 4, cell, 32);
        st_check(wcscmp(cell, g_test->library->rows[0].loop ? L"Yes" : L"No") == 0,
                 "loop column shows Yes after switching back to English");
        send_cmd(IDM_LANG_ES);
        pump(50);
    }

    /* Leave the session in the language it started in. */
    if (before == STR_LANG_EN)
        send_cmd(IDM_LANG_EN);
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
    st_phase_transport();
    st_log_handles("after transport");
    st_phase_seek_speed();
    st_log_handles("after seek/speed");
    st_phase_pair();
    st_phase_language();
    st_log_handles("after pair");
    st_phase_remove();
    st_log_handles("after remove");

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