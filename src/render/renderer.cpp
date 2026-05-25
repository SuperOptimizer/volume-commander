#include "render/renderer.hpp"

#include <cmath>
#include <vector>
#include <algorithm>

namespace vc {
namespace {

// One voxel at level L, nearest, via the 256^3 chunk cache. Returns -1 if the
// chunk isn't resident (caller falls back to a coarser level).
inline int sampleNearestLevel(Volume& vol, int level, float sx, float sy, float sz)
{
    int ix = int(sx + 0.5f), iy = int(sy + 0.5f), iz = int(sz + 0.5f);
    auto shp = vol.shape(level);
    if (ix < 0 || iy < 0 || iz < 0 || iz >= shp[0] || iy >= shp[1] || ix >= shp[2]) return 0;
    const Block* b = vol.block(level, iz >> 4, iy >> 4, ix >> 4);
    if (!b) return -1;                       // not resident -> caller falls back coarser
    int lz = iz & 15, ly = iy & 15, lx = ix & 15;
    return b->v[(std::size_t(lz) * kBlock + ly) * kBlock + lx];
}

// Adaptive sample: world coords -> value, starting at desiredLevel and
// falling back to coarser levels when chunks are missing.
inline float sampleAdaptive(Volume& vol, int desiredLevel, Vec3f w, Sampling s)
{
    const int n = vol.numLevels();
    for (int lvl = desiredLevel; lvl < n; ++lvl) {
        float f = 1.0f / float(1 << lvl);
        float sx = w[0] * f, sy = w[1] * f, sz = w[2] * f;
        if (s == Sampling::Trilinear) {
            int x0 = int(std::floor(sx)), y0 = int(std::floor(sy)), z0 = int(std::floor(sz));
            int c[8];
            bool ok = true;
            const int dx[8] = {0,1,0,1,0,1,0,1}, dy[8] = {0,0,1,1,0,0,1,1}, dz[8] = {0,0,0,0,1,1,1,1};
            for (int k = 0; k < 8; ++k) {
                int v = sampleNearestLevel(vol, lvl, float(x0+dx[k]), float(y0+dy[k]), float(z0+dz[k]));
                if (v < 0) { ok = false; break; }
                c[k] = v;
            }
            if (!ok) continue;
            float fx = sx - x0, fy = sy - y0, fz = sz - z0;
            auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
            float c00 = lerp(c[0], c[1], fx), c10 = lerp(c[2], c[3], fx);
            float c01 = lerp(c[4], c[5], fx), c11 = lerp(c[6], c[7], fx);
            return lerp(lerp(c00, c10, fy), lerp(c01, c11, fy), fz);
        } else {
            int v = sampleNearestLevel(vol, lvl, sx, sy, sz);
            if (v < 0) continue;
            return float(v);
        }
    }
    return 0.0f;
}

inline bool maskAt(const MaskVolume* mask, Vec3f w)
{
    return mask && mask->at(int(w[0] + 0.5f), int(w[1] + 0.5f), int(w[2] + 0.5f));
}

inline std::uint32_t blendOver(std::uint32_t base, std::uint32_t over)
{
    unsigned a = (over >> 24) & 0xFF;
    if (!a) return base;
    unsigned ia = 255 - a;
    auto ch = [&](int sh) {
        unsigned o = (over >> sh) & 0xFF, b = (base >> sh) & 0xFF;
        return ((o * a + b * ia) / 255) << sh;
    };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

}  // namespace

std::array<std::uint32_t, 256> grayLut(float lo, float hi)
{
    std::array<std::uint32_t, 256> lut{};
    float range = std::max(1.0f, hi - lo);
    for (int i = 0; i < 256; ++i) {
        float t = (float(i) - lo) / range;
        int g = std::clamp(int(t * 255.0f + 0.5f), 0, 255);
        lut[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
    }
    return lut;
}

void renderSurface(Tensor32& fb, int w, int h, const RenderInput& in)
{
    fb.create({h, w});
    if (!in.surf || !in.volume) { std::fill(fb.data.begin(), fb.data.end(), 0xFF000000u); return; }
    Volume& vol = *in.volume;

    Tensor3f coords, normals;
    in.surf->gen(&coords, &normals, w, h, in.camera.surfacePtr, in.camera.scale,
                 {0, 0, in.camera.zOff});

    const auto& cs = in.composite;
    const auto& cp = cs.params;
    const bool plane = in.surf->isPlane();
    int front = plane ? cs.planeLayersFront : cs.layersFront;
    int behind = plane ? cs.planeLayersBehind : cs.layersBehind;
    if (!cs.enabled) { front = 1; behind = 0; }
    int nLayers = std::max(1, front + behind + 1);
    int zStart = -behind;
    if (cs.reverseDirection) zStart = -front;

    const int lvl = in.camera.dsIdx;
    auto lut = grayLut(in.windowLow, in.windowHigh);

    TensorU8 gray(h, w);
    std::vector<float> stack(nLayers);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Vec3f base = coords(y, x);
            if (base[0] == QuadSurface::kInvalid || !std::isfinite(base[0])) { gray(y, x) = 0; continue; }
            Vec3f nrm = normals(y, x);

            int valid = 0;
            for (int li = 0; li < nLayers; ++li) {
                float zo = float(zStart + li);
                Vec3f p = base + nrm * zo;
                float v = sampleAdaptive(vol, lvl, p, in.sampling);
                if (v < float(cp.isoCutoff)) v = 0.0f;
                stack[valid++] = v;
            }
            float val = compositeLayerStack({stack.data(), std::size_t(valid)}, cp);

            if (cp.lightingEnabled) val *= computeLightingFactor(nrm, cp);
            gray(y, x) = std::uint8_t(std::clamp(val, 0.0f, 255.0f));
        }
    }

    if (cs.postStretchValues) postStretch(gray);
    if (cs.postClaheEnabled)  postClahe(gray, cs.postClaheClipLimit, cs.postClaheTileSize);
    if (cs.postRakingEnabled) postRaking(gray, cs.postRakingAzimuth, cs.postRakingElevation,
                                         cs.postRakingStrength, cs.postRakingDepthScale);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint32_t px = lut[gray(y, x)];
            if (in.mask && maskAt(in.mask, coords(y, x))) px = blendOver(px, in.maskColor);
            fb(y, x) = px;
        }
    }
}

void postStretch(TensorU8& g)
{
    std::uint8_t lo = 255, hi = 0;
    for (auto v : g.data) { lo = std::min(lo, v); hi = std::max(hi, v); }
    float range = std::max(1, int(hi) - int(lo));
    for (auto& v : g.data) v = std::uint8_t(std::clamp((int(v) - lo) * 255 / int(range), 0, 255));
}

void postClahe(TensorU8& g, float clipLimit, int tile)
{
    if (tile < 2) return;
    const int H = g.rows(), W = g.cols();
    const int ntx = (W + tile - 1) / tile, nty = (H + tile - 1) / tile;
    // per-tile clipped-histogram CDF LUTs, then nearest-tile apply.
    std::vector<std::array<std::uint8_t, 256>> luts(std::size_t(ntx) * nty);
    for (int ty = 0; ty < nty; ++ty) {
        for (int tx = 0; tx < ntx; ++tx) {
            int x0 = tx * tile, y0 = ty * tile;
            int x1 = std::min(W, x0 + tile), y1 = std::min(H, y0 + tile);
            int hist[256] = {0}, count = 0;
            for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { hist[g(y, x)]++; count++; }
            if (!count) { auto& L = luts[ty*ntx+tx]; for (int i=0;i<256;++i) L[i]=std::uint8_t(i); continue; }
            int clip = std::max(1, int(clipLimit * count / 256.0f));
            int excess = 0;
            for (int& b : hist) if (b > clip) { excess += b - clip; b = clip; }
            int add = excess / 256;
            for (int& b : hist) b += add;
            auto& L = luts[ty*ntx+tx];
            int cdf = 0;
            for (int i = 0; i < 256; ++i) { cdf += hist[i]; L[i] = std::uint8_t(std::clamp(cdf * 255 / count, 0, 255)); }
        }
    }
    TensorU8 out(H, W);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        int tx = std::min(ntx - 1, x / tile), ty = std::min(nty - 1, y / tile);
        out(y, x) = luts[ty*ntx+tx][g(y, x)];
    }
    g = std::move(out);
}

void postRaking(TensorU8& g, float azDeg, float elDeg, float strength, float depth)
{
    const int H = g.rows(), W = g.cols();
    float az = azDeg * float(M_PI) / 180.0f, el = elDeg * float(M_PI) / 180.0f;
    Vec3f L = normalized(Vec3f{std::cos(az), std::sin(az), std::tan(el) + 1e-3f});
    TensorU8 out = g;
    auto at = [&](int y, int x) { return float(g(std::clamp(y,0,H-1), std::clamp(x,0,W-1))); };
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            // Scharr gradient
            float gx = (-3*at(y-1,x-1) + 3*at(y-1,x+1) - 10*at(y,x-1) + 10*at(y,x+1)
                        -3*at(y+1,x-1) + 3*at(y+1,x+1)) / 32.0f;
            float gy = (-3*at(y-1,x-1) - 10*at(y-1,x) - 3*at(y-1,x+1)
                        +3*at(y+1,x-1) + 10*at(y+1,x) + 3*at(y+1,x+1)) / 32.0f;
            Vec3f n = normalized(Vec3f{-gx * depth, -gy * depth, 1.0f});
            float shade = std::max(0.0f, dot(n, L));
            float lit = g(y, x) * shade;
            out(y, x) = std::uint8_t(std::clamp(g(y, x) * (1 - strength) + lit * strength, 0.0f, 255.0f));
        }
    }
    g = std::move(out);
}

}  // namespace vc
