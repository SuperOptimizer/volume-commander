#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "util/math.hpp"
#include "render/compositing.hpp"
#include "render/surface.hpp"
#include "render/camera.hpp"
#include "data/volume.hpp"
#include "data/mask.hpp"

namespace vc {

enum class Sampling : std::uint8_t { Nearest, Trilinear };

// Build a 256-entry ARGB grayscale LUT for window/level.
std::array<std::uint32_t, 256> grayLut(float winLow, float winHigh);

struct RenderInput {
    const Surface* surf = nullptr;
    std::shared_ptr<const Surface> surfHold;  // keeps surf alive for async renders
    Volume* volume = nullptr;
    Camera camera;
    CompositeRenderSettings composite;
    float windowLow = 0.0f, windowHigh = 255.0f;
    Sampling sampling = Sampling::Nearest;
    const MaskVolume* mask = nullptr; // optional 3D label mask (binary)
    std::uint32_t maskColor = 0x80FF3030;  // ARGB overlay tint for labeled voxels
};

// Reusable per-viewer scratch. Passing the same instance across frames keeps
// the coord/normal/gray buffers allocated (no per-frame page-fault + zero-fill
// of fresh pages — ~15% of cache misses came from this churn).
struct RenderScratch {
    Tensor3f coords, normals;
    TensorU8 gray;
};

// Render `surf` through `volume` into an ARGB framebuffer (w*h, row-major).
// Walks composite layers along the surface normal, applies the composite
// method + optional Lambertian light, then post-process + LUT.
// Returns true if any pixel fell back to a coarser level (data still
// streaming) — caller should schedule one more refine render.
bool renderSurface(Tensor32& fb, int w, int h, const RenderInput& in, RenderScratch& scratch);

// Convenience overload with throwaway scratch (tests / one-off renders).
bool renderSurface(Tensor32& fb, int w, int h, const RenderInput& in);

// Post-process passes on a grayscale buffer in place (before LUT).
void postStretch(TensorU8& g);
void postClahe(TensorU8& g, float clipLimit, int tileSize);
void postRaking(TensorU8& g, float azimuthDeg, float elevDeg, float strength, float depthScale);

}  // namespace vc
