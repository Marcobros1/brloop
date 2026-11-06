#ifndef PALETTE_H
#define PALETTE_H

#include <windows.h>

/* One palette colour: light variant and dark variant. */
typedef struct {
    COLORREF light;
    COLORREF dark;
} PaletteColor;

/* Full palette. Each field holds a light/dark pair.
 * Fields: background, surface, text, secondary text, accent and the four
 * state colours (loading, playing, paused, finished). */
typedef struct {
    PaletteColor background;
    PaletteColor surface;
    PaletteColor text;
    PaletteColor text_secondary;
    PaletteColor accent;
    PaletteColor loading;
    PaletteColor playing;
    PaletteColor paused;
    PaletteColor finished;
} Palette;

/* Returns the active (light or dark) colour of one palette field.
 * `dark` is 1 for the dark theme, 0 for the light one. */
COLORREF palette_color(const Palette *p, int field, int dark);

/* Per-field accessors. */
COLORREF palette_background(const Palette *p, int dark);
COLORREF palette_surface(const Palette *p, int dark);
COLORREF palette_text(const Palette *p, int dark);
COLORREF palette_text_secondary(const Palette *p, int dark);
COLORREF palette_accent(const Palette *p, int dark);
COLORREF palette_loading(const Palette *p, int dark);
COLORREF palette_playing(const Palette *p, int dark);
COLORREF palette_paused(const Palette *p, int dark);
COLORREF palette_finished(const Palette *p, int dark);

/* Fills *out from the catalog entry name/variant. Returns 1 if found, 0 otherwise. */
int palette_load(const char *name, const char *variant, Palette *out);

#endif /* PALETTE_H */
