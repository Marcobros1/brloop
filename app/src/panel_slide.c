#include "panel_slide.h"

static double clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

int panel_slide_old_offset(double progress, int width, int direction)
{
    return (int)(direction * width * clamp01(progress));
}

int panel_slide_new_offset(double progress, int width, int direction)
{
    return (int)(-direction * width * (1.0 - clamp01(progress)));
}

/* Unión de las dos posiciones que ocupa un control entre dos frames
 * consecutivos del slide: `rc` desplazado por `off_prev` (donde estaba) y
 * por `off_curr` (dónde está). El resultado cubre ambas posiciones y nada
 * más: sirve para invalidar solo el rastro del control al moverse. */
RECT panel_slide_frame_rect(const RECT *rc, int off_prev, int off_curr)
{
    RECT r;
    int l1 = rc->left + off_prev;
    int r1 = rc->right + off_prev;
    int l2 = rc->left + off_curr;
    int r2 = rc->right + off_curr;
    r.left = l1 < l2 ? l1 : l2;
    r.right = r1 > r2 ? r1 : r2;
    r.top = rc->top;
    r.bottom = rc->bottom;
    return r;
}