#ifndef PANEL_SLIDE_H
#define PANEL_SLIDE_H

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

#ifdef __cplusplus
}
#endif

#endif /* PANEL_SLIDE_H */