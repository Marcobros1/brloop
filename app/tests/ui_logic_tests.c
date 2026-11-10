/* Automated tests for the T2 pure UI logic (console, no window, no audio).

   Covers, through public interfaces only:
     1. format_speed() text formatting (strings.c).
     2. library_add / library_contains_path / library_remove / library_clear
        (library.c).
     3. tr() / strings_set_language / strings_language (strings.c).
     4. settings_load_palette / settings_save_palette (settings.c), in a
        sandbox folder set through MKW_SETTINGS_DIR.

   Runs headless: no HWND, no waveOut, no BRSTM files needed.
 */
#include <windows.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "library.h"
#include "palette.h"
#include "palette_tables.h"
#include "player.h"
#include "settings.h"
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

/* Field i gets light RGB(i+1,0,0) and dark RGB(0,i+1,0), so a selector that
 * returns the wrong field or the wrong theme does not match. */
static void test_palette_selectors(void)
{
    static const struct {
        const char *name;
        COLORREF (*fn)(const Palette *, int);
    } sel[] = {
        { "palette_background", palette_background },
        { "palette_surface", palette_surface },
        { "palette_text", palette_text },
        { "palette_text_secondary", palette_text_secondary },
        { "palette_accent", palette_accent },
        { "palette_loading", palette_loading },
        { "palette_playing", palette_playing },
        { "palette_paused", palette_paused },
        { "palette_finished", palette_finished },
    };
    Palette p;
    size_t i;

    note("== palette selectors and palette_color ==");

    /* Field order follows the FIELD_ enum in palette.c. */
    p.background.light = RGB(1, 0, 0);            p.background.dark = RGB(0, 1, 0);
    p.surface.light = RGB(2, 0, 0);       p.surface.dark = RGB(0, 2, 0);
    p.text.light = RGB(3, 0, 0);            p.text.dark = RGB(0, 3, 0);
    p.text_secondary.light = RGB(4, 0, 0); p.text_secondary.dark = RGB(0, 4, 0);
    p.accent.light = RGB(5, 0, 0);           p.accent.dark = RGB(0, 5, 0);
    p.loading.light = RGB(6, 0, 0);         p.loading.dark = RGB(0, 6, 0);
    p.playing.light = RGB(7, 0, 0);    p.playing.dark = RGB(0, 7, 0);
    p.paused.light = RGB(8, 0, 0);            p.paused.dark = RGB(0, 8, 0);
    p.finished.light = RGB(9, 0, 0);        p.finished.dark = RGB(0, 9, 0);

    for (i = 0; i < sizeof(sel) / sizeof(sel[0]); i++) {
        COLORREF light = RGB((BYTE)(i + 1), 0, 0);
        COLORREF dark = RGB(0, (BYTE)(i + 1), 0);

        check(sel[i].fn(&p, 0) == light && sel[i].fn(&p, 1) == dark &&
              palette_color(&p, (int)i, 0) == light && palette_color(&p, (int)i, 1) == dark,
              "%s and palette_color return light/dark by flag", sel[i].name);
    }

    check(palette_color(&p, -1, 0) == RGB(0, 0, 0) && palette_color(&p, 9, 1) == RGB(0, 0, 0) &&
          palette_color(NULL, 0, 0) == RGB(0, 0, 0),
          "palette_color returns black for a bad field or a NULL palette");
}

/* Both sides come from different catalog tables, so each lookup is checked. */
static void test_palette_load_two_variants(void)
{
    static const struct {
        const char *name;
        COLORREF light, dark;
    } cases[] = {
        { "Metroid Prime", RGB(0xD8, 0xC7, 0xA8), RGB(0x47, 0x31, 0x18) },
        { "Super Smash Bros. Melee", RGB(0x5E, 0x92, 0xC7), RGB(0x38, 0x58, 0x77) },
        { "Resident Evil 4", RGB(0xB8, 0xB4, 0xB0), RGB(0x5A, 0x1A, 0x22) },
        { "GameCube Spice", RGB(0xFD, 0xF6, 0xEF), RGB(0x1E, 0x14, 0x10) },
    };
    Palette p;
    size_t i;

    note("== palette_load, name with Claro and Oscuro ==");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check(palette_load(cases[i].name, "Claro", &p) == 1 &&
              p.background.light == cases[i].light && p.background.dark == cases[i].dark,
              "%s: background light/dark are its Claro/Oscuro bg", cases[i].name);
    }

    check(palette_load("Metroid Prime", "Claro", &p) == 1 &&
          p.surface.light == RGB(0xDE, 0xCF, 0xB5) && p.surface.dark == RGB(0x3C, 0x2A, 0x14) &&
          p.text.light == RGB(0x00, 0x00, 0x00) && p.text.dark == RGB(0xFF, 0xFF, 0xFF) &&
          p.accent.light == RGB(0x0A, 0x5D, 0x6B) && p.accent.dark == RGB(0x5F, 0xD3, 0xE6) &&
          p.loading.light == RGB(0x0A, 0x5D, 0x6B) && p.loading.dark == RGB(0x5F, 0xD3, 0xE6) &&
          p.playing.light == RGB(0x1B, 0x61, 0x2F) && p.playing.dark == RGB(0x8F, 0xE3, 0x9A) &&
          p.paused.light == RGB(0x82, 0x45, 0x00) && p.paused.dark == RGB(0xFF, 0xB3, 0x6B) &&
          p.finished.light == RGB(0x1F, 0x4E, 0x8C) && p.finished.dark == RGB(0x9D, 0xC3, 0xFF),
          "Metroid Prime (visor): every field maps to its row's Claro/Oscuro value");
}

/* A name with one variant copies that variant to the missing side. */
static void test_palette_load_single_variant(void)
{
    Palette p;

    note("== palette_load, single-variant names ==");

    check(palette_load("Wii Blanco", "Claro", &p) == 1 &&
          p.background.light == RGB(0xE3, 0xE3, 0xE3) && p.background.dark == RGB(0xE3, 0xE3, 0xE3) &&
          p.accent.light == RGB(0x0A, 0x6B, 0xA1) && p.accent.dark == RGB(0x0A, 0x6B, 0xA1) &&
          p.text.dark == RGB(0x00, 0x00, 0x00),
          "Wii Blanco (Claro only): dark side copies Claro");

    check(palette_load("GameCube Jet Black", "Oscuro", &p) == 1 &&
          p.background.light == RGB(0x0F, 0x0F, 0x12) && p.background.dark == RGB(0x0F, 0x0F, 0x12) &&
          p.accent.light == RGB(0x7C, 0xB8, 0xFF) && p.accent.dark == RGB(0x7C, 0xB8, 0xFF) &&
          p.text.light == RGB(0xFF, 0xFF, 0xFF),
          "GameCube Jet Black (Oscuro only): light side copies Oscuro");
}

/* The variant argument is ignored: both sides load whatever it says. */
static void test_palette_load_ignores_variant(void)
{
    Palette claro, oscuro;

    note("== palette_load, variant argument is ignored ==");

    check(palette_load("Metroid Prime", "Claro", &claro) == 1 &&
          palette_load("Metroid Prime", "Oscuro", &oscuro) == 1 &&
          memcmp(&claro, &oscuro, sizeof claro) == 0,
          "Metroid Prime (visor): Claro and Oscuro requests fill the same palette");
}

static void test_palette_load_rejects(void)
{
    Palette p;

    note("== palette_load, invalid input ==");

    check(palette_load("Nombre inexistente", "Claro", &p) == 0,
          "unknown name returns 0");
    check(palette_load(NULL, "Claro", &p) == 0, "NULL name returns 0");
    check(palette_load("Wii Blanco", "Claro", NULL) == 0, "NULL out returns 0");
}

/* Secondary text: 70% text over 30% surface, per channel, integer division.
 * Metroid Prime Claro: text 000000, surface DECFB5 -> 66,62,54.
 * Oscuro: text FFFFFF, surface 3C2A14 -> 196,191,184. */
static void test_texto_secundario(void)
{
    Palette p;

    note("== text_secondary (70%% text over 30%% surface) ==");

    check(palette_load("Metroid Prime", "Claro", &p) == 1 &&
          palette_text_secondary(&p, 0) == RGB(66, 62, 54) &&
          palette_text_secondary(&p, 1) == RGB(196, 191, 184),
          "Metroid Prime (visor): text_secondary is the 70/30 mix per theme");
}

static void test_catalog_getters(void)
{
    size_t n = palette_table_count();
    const palette_entry_t *first = palette_table_get(0);

    note("== catalog table getters ==");

    check(n == 38 && first != NULL &&
          strcmp(first->name, "Super Mario Galaxy 2") == 0 &&
          palette_table_get(n - 1) != NULL && palette_table_get(n) == NULL,
          "catalog: %lu rows, first row matches, get(count) is NULL",
          (unsigned long)n);
}

/* WCAG 2.x relative luminance and contrast ratio. */
static double srgb_to_linear(int c)
{
    double v = c / 255.0;

    return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double luminance(COLORREF c)
{
    return 0.2126 * srgb_to_linear(GetRValue(c)) +
           0.7152 * srgb_to_linear(GetGValue(c)) +
           0.0722 * srgb_to_linear(GetBValue(c));
}

static double contrast_ratio(COLORREF a, COLORREF b)
{
    double la = luminance(a), lb = luminance(b);

    if (la < lb) {
        double t = la;
        la = lb;
        lb = t;
    }
    return (la + 0.05) / (lb + 0.05);
}

/* Every catalog variant: the main text and the three state texts (playing,
 * paused, finished) against the background. Every variant, including all
 * state texts, must be above 4.5:1. */
static void test_catalog_contrast(void)
{
    size_t i, total = 0, loaded = 0, below = 0;

    note("== WCAG contrast, every catalog variant (main text and state texts) ==");

    for (i = 0; i < palette_table_count(); i++) {
        const palette_entry_t *e = palette_table_get(i);
        int dark = strcmp(e->variant, "Oscuro") == 0;
        int bad = 0;
        Palette p;
        COLORREF bg;
        double r[4];
        int k;

        total++;
        if (palette_load(e->name, e->variant, &p) != 1)
            continue;
        loaded++;

        bg = palette_background(&p, dark);
        r[0] = contrast_ratio(palette_text(&p, dark), bg);
        r[1] = contrast_ratio(palette_playing(&p, dark), bg);
        r[2] = contrast_ratio(palette_paused(&p, dark), bg);
        r[3] = contrast_ratio(palette_finished(&p, dark), bg);
        for (k = 0; k < 4; k++)
            if (r[k] < 4.5)
                bad = 1;

        if (bad) {
            below++;
            note("below 4.5:1: %s (%s)", e->name, e->variant);
        }
    }

    check(loaded == total && total > 0, "all %lu catalog variants load, at least one",
          (unsigned long)total);
    check(below == 0, "all variants above 4.5:1 (main text and states); "
          "%lu variant(s) below", (unsigned long)below);
}

/* ------------------------------------------------------------- settings --- */

/* Settings tests use a folder next to the test exe, never %APPDATA%. */
static wchar_t g_settings_dir[MAX_PATH];
static wchar_t g_settings_file[MAX_PATH];

static int settings_sandbox_setup(void)
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
    if (wcscpy_s(g_settings_dir, MAX_PATH, exe) != 0 ||
        wcscat_s(g_settings_dir, MAX_PATH, L"settings-sandbox") != 0)
        return 0;
    if (wcscpy_s(g_settings_file, MAX_PATH, g_settings_dir) != 0 ||
        wcscat_s(g_settings_file, MAX_PATH, L"\\settings.json") != 0)
        return 0;
    if (!SetEnvironmentVariableW(L"MKW_SETTINGS_DIR", g_settings_dir))
        return 0;
    CreateDirectoryW(g_settings_dir, NULL);
    DeleteFileW(g_settings_file);
    return 1;
}

static void settings_sandbox_teardown(void)
{
    DeleteFileW(g_settings_file);
    RemoveDirectoryW(g_settings_dir);
}

static void write_settings_text(const char *text)
{
    FILE *f = _wfopen(g_settings_file, L"wb");
    if (!f)
        return;
    fputs(text, f);
    fclose(f);
}

/* Returns 0 when the file cannot be opened; buf is then empty. */
static int read_settings_text(char *buf, size_t cap)
{
    FILE *f = _wfopen(g_settings_file, L"rb");
    size_t n;
    buf[0] = '\0';
    if (!f)
        return 0;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 1;
}

static void test_settings_palette(void)
{
    char name[128];
    char text[512];
    wchar_t long_name[131];

    note("== settings palette (settings.c, MKW_SETTINGS_DIR sandbox) ==");

    if (!settings_sandbox_setup()) {
        check(0, "settings sandbox folder could not be set up");
        return;
    }

    /* No file yet: first run keeps the default palette. */
    check(settings_load_palette(name, sizeof name) == SETTINGS_NOT_FOUND,
          "no settings file: status is NOT_FOUND (expected 1)");
    check(strcmp(name, "Wii Blanco") == 0,
          "no settings file: palette is %s (expected Wii Blanco)", name);

    /* Save, then load the same name back. */
    check(settings_save_palette(L"Metroid Prime") == 0,
          "save Metroid Prime (visor) returns 0");
    check(settings_load_palette(name, sizeof name) == SETTINGS_OK,
          "load after save: status is OK (expected 0)");
    check(strcmp(name, "Metroid Prime") == 0,
          "load after save: palette is %s (expected Metroid Prime (visor))", name);

    /* Saving the palette keeps the saved language and theme in the same file. */
    write_settings_text("{\"language\":\"es\",\"theme\":\"dark\"}\n");
    check(settings_save_palette(L"Wii Azul") == 0,
          "save Wii Azul over a file with language and theme returns 0");
    read_settings_text(text, sizeof text);
    check(strstr(text, "\"language\":\"es\"") != NULL,
          "file keeps language es after a palette save");
    check(strstr(text, "\"theme\":\"dark\"") != NULL,
          "file keeps theme dark after a palette save");
    check(strstr(text, "\"palette\":\"Wii Azul\"") != NULL,
          "file stores palette Wii Azul after a palette save");

    /* A saved pre-Q-146 name is translated to the current one on read. */
    write_settings_text("{\"language\":\"es\",\"theme\":\"dark\",\"palette\":\"Metroid Prime (visor)\"}\n");
    check(settings_load_palette(name, sizeof name) == SETTINGS_OK &&
          strcmp(name, "Metroid Prime") == 0,
          "old name Metroid Prime (visor) is read as Metroid Prime");
    write_settings_text("{\"language\":\"es\",\"theme\":\"dark\",\"palette\":\"Zelda Wind Waker (mar)\"}\n");
    check(settings_load_palette(name, sizeof name) == SETTINGS_OK &&
          strcmp(name, "Zelda Wind Waker") == 0,
          "old name Zelda Wind Waker (mar) is read as Zelda Wind Waker");

    /* A file without the palette key gives the default, reported as invalid. */
    write_settings_text("{\"language\":\"en\",\"theme\":\"light\"}\n");
    check(settings_load_palette(name, sizeof name) == SETTINGS_INVALID,
          "file without palette key: status is INVALID (expected 2)");
    check(strcmp(name, "Wii Blanco") == 0,
          "file without palette key: palette is %s (expected Wii Blanco)", name);

    /* Names with escapes or empty values are not palettes. */
    write_settings_text("{\"palette\":\"Wii \\\"Azul\\\"\"}\n");
    settings_load_palette(name, sizeof name);
    check(strcmp(name, "Wii Blanco") == 0,
          "escaped quotes in file: palette is %s (expected Wii Blanco)", name);

    write_settings_text("{\"palette\":\"Wii\\\\Azul\"}\n");
    settings_load_palette(name, sizeof name);
    check(strcmp(name, "Wii Blanco") == 0,
          "backslash in file: palette is %s (expected Wii Blanco)", name);

    write_settings_text("{\"palette\":\"\"}\n");
    settings_load_palette(name, sizeof name);
    check(strcmp(name, "Wii Blanco") == 0,
          "empty palette in file: palette is %s (expected Wii Blanco)", name);

    /* Saving a name with a quote or backslash fails and keeps the old file. */
    check(settings_save_palette(L"Wii Azul") == 0,
          "save Wii Azul again returns 0");
    check(settings_save_palette(L"Wii \"X\"") == -1,
          "save with a quote returns -1 (expected -1)");
    check(settings_save_palette(L"Wii \\X") == -1,
          "save with a backslash returns -1 (expected -1)");
    settings_load_palette(name, sizeof name);
    check(strcmp(name, "Wii Azul") == 0,
          "rejected saves keep the file: palette is %s (expected Wii Azul)", name);

    /* A name longer than the 128-byte UTF-8 buffer does not fit. */
    wmemset(long_name, L'A', 130);
    long_name[130] = L'\0';
    check(settings_save_palette(long_name) == -1,
          "save of a 130-character name returns -1 (expected -1)");

    settings_sandbox_teardown();
}

/* Compares the whole file with the exact text write_settings must produce:
 * key order, quotes and the trailing newline. */
static void check_settings_text(const char *expected, const char *what)
{
    char text[512];
    read_settings_text(text, sizeof text);
    check(strcmp(text, expected) == 0, "%s: file text matches the expected JSON", what);
}

/* write_settings is static: its format is reached through the public save
 * calls, which rewrite all three keys. */
static void test_settings_write_format(void)
{
    note("== settings write format (write_settings via the save calls) ==");

    if (!settings_sandbox_setup()) {
        check(0, "settings sandbox folder could not be set up");
        return;
    }

    /* No file: theme falls back to system, palette to the default. */
    check(settings_save_language(STR_LANG_ES) == 0,
          "save language es without a file returns 0");
    check_settings_text("{\"language\":\"es\",\"theme\":\"system\",\"palette\":\"Wii Blanco\"}\n",
                        "first save, language es");

    /* Invalid file: the save replaces it with the defaults for the other keys. */
    write_settings_text("not json\n");
    check(settings_save_language(STR_LANG_EN) == 0,
          "save language en over an invalid file returns 0");
    check_settings_text("{\"language\":\"en\",\"theme\":\"system\",\"palette\":\"Wii Blanco\"}\n",
                        "invalid file replaced, language en");

    /* Language en keeps the saved theme and palette. */
    write_settings_text("{\"language\":\"es\",\"theme\":\"dark\",\"palette\":\"Metroid Prime (visor)\"}\n");
    check(settings_save_language(STR_LANG_EN) == 0,
          "save language en over a dark file returns 0");
    check_settings_text("{\"language\":\"en\",\"theme\":\"dark\",\"palette\":\"Metroid Prime\"}\n",
                        "language en keeps dark and Metroid");

    /* Theme light keeps the saved language and palette. */
    write_settings_text("{\"language\":\"es\",\"theme\":\"dark\",\"palette\":\"Wii Azul\"}\n");
    check(settings_save_theme(THEME_LIGHT) == 0, "save theme light returns 0");
    check_settings_text("{\"language\":\"es\",\"theme\":\"light\",\"palette\":\"Wii Azul\"}\n",
                        "theme light keeps es and Wii Azul");

    /* Theme system is the fallback branch of the theme text. */
    write_settings_text("{\"language\":\"en\",\"theme\":\"light\",\"palette\":\"Wii Azul\"}\n");
    check(settings_save_theme(THEME_SYSTEM) == 0, "save theme system returns 0");
    check_settings_text("{\"language\":\"en\",\"theme\":\"system\",\"palette\":\"Wii Azul\"}\n",
                        "theme system keeps en and Wii Azul");

    /* Theme dark. */
    write_settings_text("{\"language\":\"en\",\"theme\":\"light\",\"palette\":\"Wii Azul\"}\n");
    check(settings_save_theme(THEME_DARK) == 0, "save theme dark returns 0");
    check_settings_text("{\"language\":\"en\",\"theme\":\"dark\",\"palette\":\"Wii Azul\"}\n",
                        "theme dark keeps en and Wii Azul");

    /* Palette save keeps language es and theme light. */
    write_settings_text("{\"language\":\"es\",\"theme\":\"light\",\"palette\":\"Wii Blanco\"}\n");
    check(settings_save_palette(L"Metroid Prime") == 0,
          "save palette Metroid over es and light returns 0");
    check_settings_text("{\"language\":\"es\",\"theme\":\"light\",\"palette\":\"Metroid Prime\"}\n",
                        "palette save keeps es and light");

    settings_sandbox_teardown();
}

/* -------------------------------------------------------------------- main --- */

int main(void)
{
    emit("=== T2 UI logic tests (headless) ===");

    test_format_speed();
    test_library_add_contains();
    test_library_remove_clear();
    test_strings_tr_language();
    test_palette_selectors();
    test_palette_load_two_variants();
    test_palette_load_single_variant();
    test_palette_load_ignores_variant();
    test_palette_load_rejects();
    test_texto_secundario();
    test_catalog_getters();
    test_catalog_contrast();
    test_settings_palette();
    test_settings_write_format();

    {
        char line[200];
        snprintf(line, sizeof line, "RESULT: %s (%d checks, %d failed)",
                 g_fail ? "FAIL" : "PASS", g_pass + g_fail, g_fail);
        emit(line);
    }
    return g_fail ? 1 : 0;
}
