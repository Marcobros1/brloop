#include "anim.h"

/* Linear progress from 0.0 to 1.0 according to elapsed time since
 * anim_start(). DWORD subtraction wraps safely at the 49-day GetTickCount
 * rollover, and no effect here lasts anywhere near that long. */
void anim_start(Anim *a, DWORD now, DWORD duration_ms)
{
    if (a) {
        a->start = now;
        a->duration_ms = duration_ms;
    }
}

void anim_stop(Anim *a)
{
    if (a)
        a->duration_ms = 0;
}

double anim_progress(const Anim *a, DWORD now)
{
    DWORD elapsed;

    if (!a || a->duration_ms == 0)
        return 0.0;
    elapsed = now - a->start;
    if (elapsed >= a->duration_ms)
        return 1.0;
    return (double)elapsed / (double)a->duration_ms;
}

int anim_is_active(const Anim *a, DWORD now)
{
    return a && a->duration_ms != 0 && anim_progress(a, now) < 1.0;
}
