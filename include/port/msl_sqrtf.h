#ifndef PARTYBOARD_MSL_SQRTF_H
#define PARTYBOARD_MSL_SQRTF_H

/*
 * Force-included into every game C source (see CMakeLists.txt), never into the
 * port's own C++ or into Aurora.
 *
 * Mario Party 4 was built against the console libc's inline sqrtf
 * (libc/math.h), which is used on the MWCC path and does this:
 *
 *     if (x > 0.0f) { ...frsqrte and three Newton steps...; return y; }
 *     return x;
 *
 * so a zero or negative argument comes back unchanged. The #else branch of that
 * same header -- the one every other compiler takes -- only declares the system
 * sqrtf, which returns NaN for a negative argument. Code that took the square
 * root of a quantity that can dip below zero was harmless on the console and
 * produces NaN here.
 *
 * That is how Makin' Waves lost its pool rim: the wave weight for the ring
 * between radius 750 and 850 is sqrtf of a negative number, every rim vertex
 * got a NaN weight, and a NaN position as soon as a wave reached it. The
 * decompiled code has 320 sqrtf calls; the original developers guarded some of
 * them with ABS(), which shows the argument really can go negative, and left
 * the rest unguarded because on their platform it did not matter.
 *
 * Only the x <= 0 case changes. A positive argument still goes to the system
 * sqrtf, so every result that is finite today stays bit-identical -- which
 * keeps existing behaviour, and netplay's lockstep between peers, unaffected
 * except where the port was producing NaN. The double-precision sqrt is left
 * alone on purpose: libc/math.h returns NaN for a negative there too, exactly
 * like the system one.
 */

#include <math.h>

/* Defined before the macro below, so the sqrtf it calls is the real one. */
static inline float partyboard_msl_sqrtf(float x)
{
    /* "Not greater than zero" rather than "less than or equal", so a NaN
     * argument is handed back unchanged too -- which is what the console's
     * `if (x > 0.0f)` did. */
    return x > 0.0f ? sqrtf(x) : x;
}

#define sqrtf(x) partyboard_msl_sqrtf(x)

#endif
