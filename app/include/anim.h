#ifndef ANIM_H
#define ANIM_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot animation timing. The UI state changes instantly on click; this
 * module only measures how far the visual accompaniment has progressed,
 * from 0.0 to 1.0. It returns 0.0 when no animation is active. Never
 * blocks: the clock value is passed in explicitly (GetTickCount in the app,
 * a fixed value in tests). */
typedef struct {
    DWORD start;          /* clock value when the animation started */
    DWORD duration_ms;    /* total effect length, milliseconds */
} Anim;

/* Start (or restart) a one-shot animation of `duration_ms` ms at clock
 * value `now`. */
void anim_start(Anim *a, DWORD now, DWORD duration_ms);

/* Stop the animation in progress; anim_progress() returns 0.0 after this. */
void anim_stop(Anim *a);

/* Progress from 0.0 (start) to 1.0 (end), linear in elapsed time, at clock
 * value `now`. Returns 0.0 when inactive and 1.0 once the duration elapses. */
double anim_progress(const Anim *a, DWORD now);

/* Returns 1 while an animation is in progress, 0 when it has ended or none
 * is running. */
int anim_is_active(const Anim *a, DWORD now);

#ifdef __cplusplus
}
#endif

#endif /* ANIM_H */
