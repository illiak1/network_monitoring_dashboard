/* anim.h - frame-rate independent smoothing and easing helpers.
 *
 * Header-only (static inline), so any module can animate a value without
 * a link dependency. Used by ui.c (animation slots, tooltip fade). Functions
 * are pure: time enters only through the dt argument.
 */
#ifndef ANIM_H
#define ANIM_H

#include <math.h>

/* Moves current toward target by a fraction of the remaining distance.
 * rate is in 1/s; after 1/rate seconds ~63% (1 - 1/e) of the gap is closed.
 * The exp() form gives the same result for one 33 ms step or two 16 ms
 * steps, so animation speed does not depend on the frame rate. */
static inline float anim_approach(float current, float target, float rate, float dt)
{
    return current + (target - current) * (1.0f - expf(-rate * dt));
}

#endif
