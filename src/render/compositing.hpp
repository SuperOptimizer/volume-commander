#pragma once

#include <cstdint>
#include <cmath>
#include <numbers>
#include <algorithm>
#include <span>
#include <string_view>

#include "util/math.hpp"

// Composite settings + per-pixel dispatch. Only four methods: mean, max,
// min, alpha. Nothing fancy — this is an ink labeler, not a volume renderer.

enum class CompositeMethod : std::uint8_t { mean, max, min, alpha };

inline CompositeMethod parseComposite(std::string_view s) noexcept {
    if (s == "max")   return CompositeMethod::max;
    if (s == "min")   return CompositeMethod::min;
    if (s == "alpha") return CompositeMethod::alpha;
    return CompositeMethod::mean;
}

struct CompositeParams {
    CompositeMethod method = CompositeMethod::mean;

    // alpha blend knobs (front-to-back over the layer stack)
    float alphaMin = 0.0f, alphaMax = 1.0f, alphaOpacity = 1.0f, alphaCutoff = 1.0f;

    // lighting (Lambertian, optional) — used by the renderer, not the composite
    bool  lightingEnabled = false;
    int   lightNormalSource = 0;       // 0 = surface normal, 1 = volume gradient
    float lightAzimuth = 45.0f, lightElevation = 45.0f;
    float lightDiffuse = 0.7f, lightAmbient = 0.3f;
    float lightDirX = 0.5f, lightDirY = 0.5f, lightDirZ = 0.70710678f;

    std::uint8_t isoCutoff = 0;        // zero out samples below this before compositing

    void updateLightDir() noexcept {
        float az = lightAzimuth * (std::numbers::pi_v<float> / 180.0f);
        float el = lightElevation * (std::numbers::pi_v<float> / 180.0f);
        float ce = std::cos(el);
        lightDirX = ce * std::cos(az); lightDirY = ce * std::sin(az); lightDirZ = std::sin(el);
    }
    bool operator==(const CompositeParams&) const = default;
};

struct CompositeRenderSettings {
    bool enabled = false;
    int  layersFront = 8, layersBehind = 0;
    bool reverseDirection = false;

    bool planeEnabled = false;
    int  planeLayersFront = 4, planeLayersBehind = 4;
    bool useVolumeGradients = false;

    CompositeParams params;

    bool postStretchValues = false;

    bool  postClaheEnabled = false;
    float postClaheClipLimit = 2.0f;
    int   postClaheTileSize = 8;

    bool  postRakingEnabled = false;
    float postRakingAzimuth = 30.0f, postRakingElevation = 20.0f;
    float postRakingStrength = 0.8f, postRakingDepthScale = 4.0f;

    bool operator==(const CompositeRenderSettings&) const = default;
};

// Composite one pixel's layer stack to a [0,255] value. `layers` are raw
// [0,255] samples along the ray.
inline float compositeLayerStack(std::span<const float> layers, const CompositeParams& p) noexcept
{
    if (layers.empty()) return 0.0f;
    switch (p.method) {
        case CompositeMethod::max: {
            float m = 0.0f; for (float v : layers) m = std::max(m, v); return m;
        }
        case CompositeMethod::min: {
            float m = 255.0f; for (float v : layers) m = std::min(m, v); return m;
        }
        case CompositeMethod::alpha: {
            // front-to-back: map each sample to [0,1] via [min,max], blend.
            const float lo = p.alphaMin * 255.0f, hi = p.alphaMax * 255.0f;
            const float range = hi - lo;
            if (range <= 0.0f) return 0.0f;
            const float inv = 1.0f / range;
            float a = 0.0f, acc = 0.0f;
            for (float v : layers) {
                float n = (v - lo) * inv;
                if (n <= 0.0f) continue;
                n = std::min(n, 1.0f);
                if (a >= p.alphaCutoff) break;
                float op = std::min(n * p.alphaOpacity, 1.0f);
                float w = (1.0f - a) * op;
                acc += w * n; a += w;
            }
            return std::clamp(acc * 255.0f, 0.0f, 255.0f);
        }
        case CompositeMethod::mean:
        default: {
            float s = 0.0f; for (float v : layers) s += v; return s / float(layers.size());
        }
    }
}

inline float computeLightingFactor(const vc::Vec3f& n, const CompositeParams& p) noexcept
{
    if (!p.lightingEnabled) return 1.0f;
    float len = vc::norm(n);
    if (len < 1e-4f) return p.lightAmbient;
    float inv = 1.0f / len;
    float nDotL = (n[0]*inv)*p.lightDirX + (n[1]*inv)*p.lightDirY + (n[2]*inv)*p.lightDirZ;
    if (nDotL < 0.0f) nDotL = 0.0f;
    return std::clamp(p.lightAmbient + p.lightDiffuse * nDotL, 0.0f, 1.0f);
}
