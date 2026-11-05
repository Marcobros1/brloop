/* Automated tests for the T2 pure UI logic (console, no window, no audio).

   Covers, through public interfaces only:
     1. format_speed() text formatting (strings.c).
     2. library_add / library_contains_path / library_remove / library_clear
        (library.c).
     3. tr() / strings_set_language / strings_language (strings.c).

   Runs headless: no HWND, no waveOut, no BRSTM files needed.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "library.h"
#include "player.h"
#include "strings.h"

static int g_pass, g_fail;
static FILE *g_report;

static void emit(const char *line)
{
    puts(line);
    fflush(stdout);
    if (g_report) {
        fputs(line, g_report);
        fputc('\n', g_report);
        fflush(g_report);
    }
}

static void check(int ok, const char *fmt, ...)
{
    char msg[1024], line[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "[%s] %s", ok ? "PASS" : "FAIL", msg);
    emit(line);
    if (ok)
        g_pass++;
    else
        g_fail++;
}

static void note(const char *fmt, ...)
{
    char msg[1024], line[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "  %s", msg);
    emit(line);
}

/* ----------------------------------------------------------------- speed --- */

/* format_speed() writes NUL-terminated text into `out`. The buffer is sized
 * well beyond any output so truncation cannot interfere with these checks. */
static void test_format_speed(void)
{
    wchar_t out[64];

    note("== format_speed (pure text formatting) ==");

    /* PPM scale: 1e6 = 1.0x, >=2 decimals, trailing zeros trimmed, comma. */
    format_speed(out, 64, PLAYER_SPEED_SCALE);
    check(wcscmp(out, L"1,00×") == 0, "1.0x -> %ls (expected 1,00×)", out);

    format_speed(out, 64, PLAYER_FLSM_SPEED_PPM);
    check(wcscmp(out, L"1,06812×") == 0, "FLSM -> %ls (expected 1,06812×)", out);

    format_speed(out, 64, 2000000u);
    check(wcscmp(out, L"2,00×") == 0, "2.0x -> %ls (expected 2,00×)", out);

    format_speed(out, 64, 1500000u);
    check(wcscmp(out, L"1,50×") == 0, "1.5x -> %ls (expected 1,50×)", out);

    format_speed(out, 64, 0u);
    check(wcscmp(out, L"0,00×") == 0, "0 -> %ls (expected 0,00×)", out);

    /* Small buffer must still be NUL-terminated and not overflow. */
    format_speed(out, 3, PLAYER_SPEED_SCALE);
    check(out[0] == L'1' && out[1] == L',' && out[2] == L'\0',
          "truncated to 3 bytes -> %ls (expected 1,)", out);
}

/* ------------------------------------------------------------- library --- */

static void test_library_add_contains(void)
{
    LibraryState lib = { NULL, 0, 0, -1 };
    LibraryTrackInfo info = { 44100, 2, 1000, 1, 100 };
    wchar_t name[] = L"track.brstm";

    note("== library add / contains-path ==");

    check(library_add(&lib, L"C:\\Music\\track.brstm", name, &info) == 0,
          "first add succeeds");
    check(lib.count == 1, "count is 1 after one add");
    check(library_contains_path(&lib, L"C:\\Music\\track.brstm"),
          "contains the added path");
    check(library_contains_path(&lib, L"c:\\music\\track.brstm"),
          "path compare ignores case");
    check(!library_contains_path(&lib, L"C:\\Music\\other.brstm"),
          "does not contain a missing path");

    /* library_add is a low-level append: it does not dedupe (dedupe is the
     * add_track layer). Adding a different path grows the library. */
    check(library_add(&lib, L"C:\\Music\\other.brstm", name, &info) == 0,
          "second different path is appended");
    check(lib.count == 2, "count is 2 after two adds");

    library_clear(&lib);
}

static void test_library_remove_clear(void)
{
    LibraryState lib = { NULL, 0, 0, -1 };
    LibraryTrackInfo info = { 48000, 2, 2000, 0, 0 };
    wchar_t name[] = L"t.brstm";
    int replacement;

    note("== library remove / clear ==");

    library_add(&lib, L"A.brstm", name, &info);
    library_add(&lib, L"B.brstm", name, &info);
    library_add(&lib, L"C.brstm", name, &info);
    lib.selected_index = 1;

    /* Remove the middle (selected) row: replacement is the next row. */
    replacement = library_remove(&lib, 1);
    check(replacement == 1, "remove middle row returns replacement index 1");
    check(lib.count == 2, "count is 2 after removal");
    check(lib.selected_index == -1, "selected_index reset to -1");
    check(library_contains_path(&lib, L"A.brstm"), "A still present");
    check(library_contains_path(&lib, L"C.brstm"), "C still present");
    check(!library_contains_path(&lib, L"B.brstm"), "B removed");

    /* Remove out of range: -1, no change. */
    check(library_remove(&lib, 99) == -1, "remove out of range returns -1");
    check(library_remove(&lib, -1) == -1, "remove negative returns -1");
    check(lib.count == 2, "count unchanged after bad removals");

    library_clear(&lib);
    check(lib.count == 0 && lib.selected_index == -1,
          "clear empties the library and resets selection");
}

/* ----------------------------------------------------------------- strings --- */

static void test_strings_tr_language(void)
{
    note("== tr() / language (strings.c) ==");

    strings_set_language(STR_LANG_EN);
    check(strings_language() == STR_LANG_EN, "language is English after set");
    check(wcscmp(tr(STR_LIBRARY_GROUP), L"Library") == 0,
          "English library group text");
    check(wcscmp(tr(STR_BUTTON_OPEN), L"Open…") == 0,
          "English open button text");

    strings_set_language(STR_LANG_ES);
    check(strings_language() == STR_LANG_ES, "language is Spanish after set");
    check(wcscmp(tr(STR_LIBRARY_GROUP), L"Biblioteca") == 0,
          "Spanish library group text");
    check(wcscmp(tr(STR_BUTTON_OPEN), L"Abrir…") == 0,
          "Spanish open button text");

    /* Out-of-range key never returns NULL. */
    check(tr((StrKey)STR_KEY_COUNT) != NULL, "out-of-range key non-NULL");
    check(wcscmp(tr((StrKey)STR_KEY_COUNT), L"") == 0,
          "out-of-range key returns empty string");

    /* Restore English so later checks are not affected by language state. */
    strings_set_language(STR_LANG_EN);
}

/* -------------------------------------------------------------------- main --- */

int main(void)
{
    emit("=== T2 UI logic tests (headless) ===");

    test_format_speed();
    test_library_add_contains();
    test_library_remove_clear();
    test_strings_tr_language();

    {
        char line[200];
        snprintf(line, sizeof line, "RESULT: %s (%d checks, %d failed)",
                 g_fail ? "FAIL" : "PASS", g_pass + g_fail, g_fail);
        emit(line);
    }
    return g_fail ? 1 : 0;
}
