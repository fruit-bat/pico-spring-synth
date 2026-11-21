#pragma once
//
// ss_math.h - fast float helpers: 2^x, sine and cosine, clamping.
//
// Used at control rate (once per block per string or partial) and at note
// on, so they are written to compile to straight-line single precision
// code on the Cortex-M33 FPU: no libm calls, no doubles.
//
#include <stdint.h>
#include <math.h>

// 2^x. Relative error < 3e-6 for |x| < 126; returns 0 below -126 and
// saturates at 2^126 above. Splits x into a nearest integer n and a
// fraction f in [-0.5, 0.5], evaluates 2^f with a polynomial and adds n
// straight into the float's exponent bits.
static inline float ss_exp2f(float x) {
    if (x < -126.0f) return 0.0f;
    if (x > 126.0f) x = 126.0f;
    const float xr = floorf(x + 0.5f);
    const float f = x - xr; // [-0.5, 0.5]
    float p = 1.540353e-4f;
    p = p * f + 1.333355e-3f;
    p = p * f + 9.618129e-3f;
    p = p * f + 5.550411e-2f;
    p = p * f + 2.402265e-1f;
    p = p * f + 6.931472e-1f;
    p = p * f + 1.0f;
    union { float f; int32_t i; } u = { p };
    u.i += (int32_t)xr * (1 << 23);
    return u.f;
}

// log2(x) for x > 0, absolute error < 1e-4. Splits off the exponent bits
// and fits log2(1 + t), t in [0, 1), with a polynomial.
static inline float ss_log2f(float x) {
    union { float f; int32_t i; } u = { x };
    const float e = (float)((u.i >> 23) & 0xff) - 127.0f;
    u.i = (u.i & 0x7fffff) | 0x3f800000;
    const float t = u.f - 1.0f;
    float p = 4.3434335e-2f;
    p = p * t - 1.8774400e-1f;
    p = p * t + 4.0874967e-1f;
    p = p * t - 7.0571949e-1f;
    p = p * t + 1.4412708e+0f;
    return e + p * t;
}

// e^x, from ss_exp2f
static inline float ss_expf(float x) {
    return ss_exp2f(x * 1.4426950f);
}

// sin(x) for x in [-pi/2, pi/2] only, error < 1e-7 (Taylor series,
// degree 11).
static inline float ss_sin_half_pi(float x) {
    const float x2 = x * x;
    float p = -2.5052108e-8f;
    p = p * x2 + 2.7557319e-6f;
    p = p * x2 - 1.9841270e-4f;
    p = p * x2 + 8.3333333e-3f;
    p = p * x2 - 1.6666667e-1f;
    return x + x * x2 * p;
}

// sin and cos of w in [0, pi], written to *sn and *cs.
static inline void ss_sincos(float w, float *sn, float *cs) {
    const float half_pi = 1.5707963f;
    *sn = ss_sin_half_pi(w > half_pi ? 3.1415927f - w : w);
    *cs = ss_sin_half_pi(half_pi - w);
}

// sin(2 pi p) for a phase p in turns, p in [0, 1).
static inline float ss_sin_turns(float p) {
    float w = 6.2831853f * (p - 0.5f);   // [-pi, pi), sin(w) = -sin(2 pi p)
    if (w > 1.5707963f) w = 3.1415927f - w;
    else if (w < -1.5707963f) w = -3.1415927f - w;
    return -ss_sin_half_pi(w);
}

// x limited to [lo, hi].
static inline float ss_clampf(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

// Per-sample decay factor for a time to fall 60 dB of t60 seconds at
// sample rate fs: 10^(-3 / (t60 fs)). t60 <= 0 means "never" (1.0).
static inline float ss_t60_to_decay(float t60, float fs) {
    if (t60 <= 0.0f) return 1.0f;
    return ss_expf(-6.9077553f / (t60 * fs));
}
