#ifndef PALETTE_TABLES_H
#define PALETTE_TABLES_H

#include <stddef.h>

/* One raw catalog row. Colors stay as "#RRGGBB" strings. */
typedef struct {
    const char *name;
    const char *variant;          /* "Light" or "Dark" */
    const char *bg;
    const char *surface;
    const char *accent;
    const char *text;
    const char *text_on_accent;
    const char *playing;
    const char *text_on_playing;
    const char *paused;
    const char *text_on_paused;
    const char *finished;
    const char *text_on_finished;
} palette_entry_t;

/* Catalog: entry count and indexed access (NULL if out of range). */
size_t palette_table_count(void);
const palette_entry_t *palette_table_get(size_t i);

#endif /* PALETTE_TABLES_H */
