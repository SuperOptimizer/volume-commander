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
    float scale = 0.5f;
    float zOff = 0.0f;          // normal-offset slice navigation
    int   dsIdx = 0;            // pyramid level index
    float dsScale = 1.0f;       // 1 / 2^dsIdx

    static constexpr float kMin = 0.01f, kMax = 10.0f;

    // Coarsest level whose voxels are still finer than one output pixel.
    void recalcLevel(int numLevels) noexcept {
        int lvl = 0;
        // each level is 2x coarser; pick so that (1/2^lvl) >= scale isn't over-zoomed
        while (lvl + 1 < numLevels && (1.0f / float(1 << (lvl + 1))) >= scale) ++lvl;
        dsIdx = std::clamp(lvl, 0, numLevels - 1);
        dsScale = 1.0f / float(1 << dsIdx);
    }
    static float roundScale(float s) noexcept { return std::clamp(s, kMin, kMax); }
    void zoom(int steps, float factor = 1.15f) noexcept {
        scale = roundScale(scale * std::pow(factor, float(steps)));
    }
};

}  // namespace vc
