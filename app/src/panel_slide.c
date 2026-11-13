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