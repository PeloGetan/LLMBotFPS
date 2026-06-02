#pragma once
#include "raylib.h"
#include <cmath>

// Small Vector2 helpers (raylib's Vector2 is just {float x, y}).

inline Vector2 operator+(Vector2 a, Vector2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vector2 operator-(Vector2 a, Vector2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vector2 operator*(Vector2 a, float s) { return {a.x * s, a.y * s}; }
inline Vector2 operator/(Vector2 a, float s) { return {a.x / s, a.y / s}; }
inline Vector2& operator+=(Vector2& a, Vector2 b) { a.x += b.x; a.y += b.y; return a; }

inline float vDot(Vector2 a, Vector2 b) { return a.x * b.x + a.y * b.y; }
inline float vLenSq(Vector2 a) { return a.x * a.x + a.y * a.y; }
inline float vLen(Vector2 a) { return std::sqrt(vLenSq(a)); }
inline float vDist(Vector2 a, Vector2 b) { return vLen(b - a); }
inline float vDistSq(Vector2 a, Vector2 b) { return vLenSq(b - a); }

inline Vector2 vNorm(Vector2 a) {
    float l = vLen(a);
    if (l < 1e-6f) return {0, 0};
    return {a.x / l, a.y / l};
}

inline float vAngle(Vector2 a) { return std::atan2(a.y, a.x); }
inline Vector2 vFromAngle(float rad, float len = 1.0f) {
    return {std::cos(rad) * len, std::sin(rad) * len};
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Smallest absolute difference between two angles (radians).
inline float angleDiff(float a, float b) {
    float d = std::fmod(b - a + PI, 2.0f * PI);
    if (d < 0) d += 2.0f * PI;
    return d - PI;
}
