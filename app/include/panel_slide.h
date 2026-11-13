#ifndef PANEL_SLIDE_H
#define PANEL_SLIDE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Pure panel slide math. No HWND, no timer.
 * progress: 0.0..1.0 (clamps if outside).
 * width: panel width in pixels.
 * Returns offset relative to layout's normal X.
 *   Old panel: 0.0 -> -width (leaves to the left).
 *   New panel: +width -> 0.0 (enters from the right). */
int panel_slide_old_offset(double progress, int width);
int panel_slide_new_offset(double progress, int width);

#ifdef __cplusplus
}
#endif

#endif /* PANEL_SLIDE_H */