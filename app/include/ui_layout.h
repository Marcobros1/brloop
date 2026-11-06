#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

#include "ui_state.h"

/* Reads the window DPI and the text line height of g_font. Called once in
 * WM_CREATE, before any control is positioned. */
void layout_init(UINT dpi);

/* Recompute every control position from the client size. Called on WM_SIZE
 * and once after creation. */
void layout(void);

/* Widest state-text pixel width (all states, both languages), measured
 * with g_font. */
int measure_state_text_width(void);

/* Writes g_nameFull into the banner box, fitting it with "..." if needed. */
void name_fit_to_box(void);

/* Sets the track name shown in the banner box and refits it to the box. */
void set_name_fitted(const wchar_t *name);

#endif /* UI_LAYOUT_H */
