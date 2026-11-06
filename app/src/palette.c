/* palette.c: Palette accessors declared in palette.h, and palette_load().
 *
 * The catalog table is a static array; this file reads it only through the
 * getters declared in palette_tables.h.
 */
#include "palette.h"
#include "palette_tables.h"

#include <stdio.h>
#include <string.h>

/* Palette fields, in struct Palette order. */
enum {
    FIELD_BACKGROUND = 0,
    FIELD_SURFACE,
    FIELD_TEXT,
    FIELD_TEXT_SECONDARY,
    FIELD_ACCENT,
    FIELD_LOADING,
    FIELD_PLAYING,
    FIELD_PAUSED,
    FIELD_FINISHED,
    FIELD_COUNT
};

COLORREF palette_color(const Palette *p, int field, int dark)
{
    const PaletteColor *c;

    if (p == NULL || field < 0 || field >= FIELD_COUNT)
        return RGB(0, 0, 0);

    switch (field) {
    case FIELD_BACKGROUND:     c = &p->background;     break;
    case FIELD_SURFACE:        c = &p->surface;        break;
    case FIELD_TEXT:           c = &p->text;           break;
    case FIELD_TEXT_SECONDARY: c = &p->text_secondary; break;
    case FIELD_ACCENT:         c = &p->accent;         break;
    case FIELD_LOADING:        c = &p->loading;        break;
    case FIELD_PLAYING:        c = &p->playing;        break;
    case FIELD_PAUSED:         c = &p->paused;         break;
    default:                   c = &p->finished;       break;
    }

    return dark ? c->dark : c->light;
}

COLORREF palette_background(const Palette *p, int dark)     { return palette_color(p, FIELD_BACKGROUND, dark); }
COLORREF palette_surface(const Palette *p, int dark)        { return palette_color(p, FIELD_SURFACE, dark); }
COLORREF palette_text(const Palette *p, int dark)           { return palette_color(p, FIELD_TEXT, dark); }
COLORREF palette_text_secondary(const Palette *p, int dark) { return palette_color(p, FIELD_TEXT_SECONDARY, dark); }
COLORREF palette_accent(const Palette *p, int dark)         { return palette_color(p, FIELD_ACCENT, dark); }
COLORREF palette_loading(const Palette *p, int dark)        { return palette_color(p, FIELD_LOADING, dark); }
COLORREF palette_playing(const Palette *p, int dark)        { return palette_color(p, FIELD_PLAYING, dark); }
COLORREF palette_paused(const Palette *p, int dark)         { return palette_color(p, FIELD_PAUSED, dark); }
COLORREF palette_finished(const Palette *p, int dark)       { return palette_color(p, FIELD_FINISHED, dark); }

/* Parses "#RRGGBB" into a COLORREF. Returns 0 on success, -1 on bad input. */
static int parse_hex_color(const char *hex, COLORREF *out)
{
    unsigned int r, g, b;

    if (hex == NULL || hex[0] != '#' || strlen(hex) != 7)
        return -1;
    if (sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b) != 3)
        return -1;

    *out = RGB(r, g, b);
    return 0;
}

/* Mixes two colors: `pct` percent of a, the rest of b. */
static COLORREF mix_colors(COLORREF a, COLORREF b, int pct)
{
    int r = (GetRValue(a) * pct + GetRValue(b) * (100 - pct)) / 100;
    int g = (GetGValue(a) * pct + GetGValue(b) * (100 - pct)) / 100;
    int bl = (GetBValue(a) * pct + GetBValue(b) * (100 - pct)) / 100;

    return RGB(r, g, bl);
}

/* Returns the catalog entry for name and variant, or NULL. */
static const palette_entry_t *find_in_tables(const char *name, const char *variant)
{
    size_t i, n = palette_table_count();

    for (i = 0; i < n; i++) {
        const palette_entry_t *e = palette_table_get(i);
        if (e != NULL && strcmp(e->name, name) == 0 && strcmp(e->variant, variant) == 0)
            return e;
    }
    return NULL;
}

/* Parses the seven colors of an entry: bg, surface, text, accent, playing, paused, finished. */
static int parse_entry(const palette_entry_t *e, COLORREF c[7])
{
    if (parse_hex_color(e->bg, &c[0]) != 0 || parse_hex_color(e->surface, &c[1]) != 0 ||
        parse_hex_color(e->text, &c[2]) != 0 || parse_hex_color(e->accent, &c[3]) != 0 ||
        parse_hex_color(e->playing, &c[4]) != 0 || parse_hex_color(e->paused, &c[5]) != 0 ||
        parse_hex_color(e->finished, &c[6]) != 0)
        return -1;
    return 0;
}

/* Fills both themes of `out` from the Claro and Oscuro entries of `name`.
 * `variant` is ignored: both sides are always loaded. A missing side copies the other. */
int palette_load(const char *name, const char *variant, Palette *out)
{
    const palette_entry_t *claro, *oscuro;
    COLORREF light[7], dark[7];

    (void)variant;
    if (name == NULL || out == NULL)
        return 0;

    claro = find_in_tables(name, "Claro");
    oscuro = find_in_tables(name, "Oscuro");
    if (claro == NULL && oscuro == NULL)
        return 0;
    if (claro == NULL)
        claro = oscuro;
    if (oscuro == NULL)
        oscuro = claro;

    if (parse_entry(claro, light) != 0 || parse_entry(oscuro, dark) != 0)
        return 0;

    out->background.light = light[0];
    out->background.dark = dark[0];
    out->surface.light = light[1];
    out->surface.dark = dark[1];
    out->text.light = light[2];
    out->text.dark = dark[2];
    out->accent.light = light[3];
    out->accent.dark = dark[3];
    out->loading.light = light[3];
    out->loading.dark = dark[3];
    out->playing.light = light[4];
    out->playing.dark = dark[4];
    out->paused.light = light[5];
    out->paused.dark = dark[5];
    out->finished.light = light[6];
    out->finished.dark = dark[6];

    /* Secondary text: fixed 70% text over 30% surface, per theme. Not taken from the table. */
    out->text_secondary.light = mix_colors(light[2], light[1], 70);
    out->text_secondary.dark = mix_colors(dark[2], dark[1], 70);

    return 1;
}
