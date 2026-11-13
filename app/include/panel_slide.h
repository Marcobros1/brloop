#ifndef PANEL_SLIDE_H
#define PANEL_SLIDE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure panel slide math. No HWND, no timer.
 * progress: 0.0..1.0 (clamps if outside).
 * width: panel width in pixels.
 * direction: -1 = old panel leaves to the left, new enters from the right
 *            (Library -> Settings). +1 = old panel leaves to the right,
 *            new enters from the left (Settings -> Library).
 * Returns offset relative to layout's normal X.
 *   Old panel: 0 -> direction * width.
 *   New panel: -direction * width -> 0. */
int panel_slide_old_offset(double progress, int width, int direction);
int panel_slide_new_offset(double progress, int width, int direction);

/* Unión de las dos posiciones que ocupa un control entre dos frames
 * consecutivos del slide: `rc` desplazado por `off_prev` (donde estaba) y
 * por `off_curr` (dónde está). Sirve para invalidar solo el rastro del
 * control al moverse, sin tocar el resto de la ventana. No HWND, no timer. */
RECT panel_slide_frame_rect(const RECT *rc, int off_prev, int off_curr);

#ifdef __cplusplus
}
#endif

#endif /* PANEL_SLIDE_H */