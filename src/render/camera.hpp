#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include "util/math.hpp"

namespace vc {

// View state for one viewer. surfacePtr is the surface-space center; scale is
// zoom (output px per world/grid unit); dsIdx is the pyramid level to sample.
struct Camera {
    Vec3f surfacePtr{0, 0, 0};
    float scale = 1.0f;          // 1 volume voxel (at level 0) per window pixel
    float zOff = 0.0f;          // normal-offset slice navigation
    int   dsIdx = 0;            // pyramid level index
    float dsScale = 1.0f;       // 1 / 2^dsIdx

    static constexpr float kMin = 0.01f, kMax = 10.0f;

    // Pyramid level for the current zoom. One output pixel should map to one
    // level-L voxel, so level L covers zoom in (0.5^L, 0.5^(L-1)]:
    //   scale >= 1     -> 0   (full res)
    //   0.5  .. 1      -> 1
    //   0.25 .. 0.5    -> 2
    //   0.125.. 0.25   -> 3   ... i.e. L = ceil(-log2(scale)).
    void recalcLevel(int numLevels) noexcept {
        int lvl = 0;
        if (scale < 1.0f) lvl = int(std::ceil(-std::log2(scale)));
        dsIdx = std::clamp(lvl, 0, numLevels - 1);
        dsScale = 1.0f / float(1 << dsIdx);
    }
    static float roundScale(float s) noexcept { return std::clamp(s, kMin, kMax); }
    void zoom(int steps, float factor = 1.15f) noexcept {
        scale = roundScale(scale * std::pow(factor, float(steps)));
    }
};

}  // namespace vc
