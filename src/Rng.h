#pragma once
#include <random>

struct Rng {
    std::mt19937 gen{std::random_device{}()};
    float f01() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(gen); }
    float range(float a, float b) { return std::uniform_real_distribution<float>(a, b)(gen); }
    bool chance(float p) { return f01() < p; }
};

inline Rng& rng() {
    static Rng r;
    return r;
}
