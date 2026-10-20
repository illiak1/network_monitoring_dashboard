/* anim.h - frame-rate independent smoothing and easing helpers.
 *
 * Header-only (static inline), so any module can animate a value without
 * a link dependency. Used by ui.c (animation slots, scrolling) and charts.c
 * (axis, bar and gauge-zone blending). Functions are pure: time enters only
 * through the dt argument.
 */
#ifndef ANIM_H
#define ANIM_H

#include <math.h>

/* M_PI is not part of ISO C99 (the project builds with -std=c99 -pedantic),
 * so the constant is defined here. Used for degree-to-radian conversion. */
#define ANIM_PI 3.14159265358979323846f

/* Moves current toward target by a fraction of the remaining distance.
 * rate is in 1/s; after 1/rate seconds ~63% (1 - 1/e) of the gap is closed.
 * The exp() form gives the same result for one 33 ms step or two 16 ms
 * steps, so animation speed does not depend on the frame rate. */
static inline float anim_approach(float current, float target, float rate, float dt)
{
    return current + (target - current) * (1.0f - expf(-rate * dt));
}

/* Clamps x to [0, 1]. */
static inline float anim_clamp01(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

/* Smoothstep 3x^2 - 2x^3 on [0, 1]: zero slope at both ends, so colour
 * blends start and finish without a visible kink. Input is clamped. */
static inline float anim_smoothstep(float x)
{
    x = anim_clamp01(x);
    return x * x * (3.0f - 2.0f * x);
}

#endif
