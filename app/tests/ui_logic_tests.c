/* Automated tests for the T2 pure UI logic (console, no window, no audio).

   Covers, through public interfaces only:
     1. format_speed() text formatting (strings.c).
     2. library_add / library_contains_path / library_remove / library_clear
        (library.c).
     3. tr() / strings_set_language / strings_language (strings.c).

   Runs headless: no HWND, no waveOut, no BRSTM files needed.
 */
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
        { "Metroid Prime (visor)", RGB(0xD8, 0xC7, 0xA8), RGB(0x47, 0x31, 0x18) },
        { "Super Smash Bros. Melee", RGB(0x5E, 0x92, 0xC7), RGB(0x38, 0x58, 0x77) },
        { "Resident Evil 4 (apagado)", RGB(0xB8, 0xB4, 0xB0), RGB(0x5A, 0x1A, 0x22) },
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

    check(palette_load("Metroid Prime (visor)", "Claro", &p) == 1 &&
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

    check(palette_load("Metroid Prime (visor)", "Claro", &claro) == 1 &&
          palette_load("Metroid Prime (visor)", "Oscuro", &oscuro) == 1 &&
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
 * Metroid Prime (visor) Claro: text 000000, surface DECFB5 -> 66,62,54.
 * Oscuro: text FFFFFF, surface 3C2A14 -> 196,191,184. */
static void test_texto_secundario(void)
{
    Palette p;

    note("== text_secondary (70%% text over 30%% surface) ==");

    check(palette_load("Metroid Prime (visor)", "Claro", &p) == 1 &&
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
          strcmp(first->name, "Super Mario Galaxy 2 (clara)") == 0 &&
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
 * paused, finished) against the background. Q-120 approves exactly six
 * variants with a state text below 4.5:1; any other failure is a bug. */
static int is_q120_exception(const char *name, const char *variant)
{
    (void)name;
    (void)variant;
    return 0;
}

static void test_catalog_contrast(void)
{
    size_t i, total = 0, loaded = 0, below = 0, unexpected = 0;

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
            if (!is_q120_exception(e->name, e->variant))
                unexpected++;
        }
    }

    check(total == 38 && loaded == total, "%lu catalog variants, all load",
          (unsigned long)total);
    check(below == 0, "variants below 4.5:1: %lu (expected 0)", (unsigned long)below);
    check(unexpected == 0, "no variant outside the Q-120 list fails: %lu",
          (unsigned long)unexpected);
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

    {
        char line[200];
        snprintf(line, sizeof line, "RESULT: %s (%d checks, %d failed)",
                 g_fail ? "FAIL" : "PASS", g_pass + g_fail, g_fail);
        emit(line);
    }
    return g_fail ? 1 : 0;
}
