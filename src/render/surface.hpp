#pragma once

#include <memory>
#include <string>
#include "util/math.hpp"

namespace vc {

// A surface maps 2D output pixels to world (x,y,z) sample positions + normals.
// gen() fills two row-major grids the renderer walks per pixel.
//   coords[y,x] = world xyz to sample at the surface
//   normals[y,x] = unit surface normal (sample walks along this for layers)
// ptr/offset move the surface origin in its own param space; scale is the
// output resolution (pixels per world unit at level 0).
struct Surface {
    virtual ~Surface() = default;
    // zOff: shift the sampled sheet along its normal by zOff world voxels
    // (shift+scroll). Plane: moves the slice in/out along the view axis.
    // Quad: rigidly pushes the surface along zOffDir (a FIXED world direction
    // captured at scroll time; if zero, gen samples the center normal). Passing
    // a fixed dir keeps the push rigid so panning doesn't reshape the surface.
    virtual void gen(Tensor3f* coords, Tensor3f* normals, int w, int h,
                     Vec3f ptr, float scale, float zOff, Vec3f zOffDir = {0,0,0}) const = 0;
    virtual Vec3f pointer() const { return {0, 0, 0}; }
    virtual bool isPlane() const { return false; }
};

// Axis-aligned (or arbitrarily-oriented) infinite plane through `origin`
// with unit `normal`. Basis (vx,vy) spans the plane; coord = origin + vx*x + vy*y.
struct PlaneSurface : Surface {
    Vec3f origin{0, 0, 0};
    Vec3f normal{0, 0, 1};
    Vec3f vx{1, 0, 0};
    Vec3f vy{0, 1, 0};

    PlaneSurface() = default;
    PlaneSurface(Vec3f o, Vec3f n) : origin(o) { setNormal(n); }

    bool isPlane() const override { return true; }

    void setNormal(Vec3f n) {
        normal = normalized(n);
        // pick an up hint least aligned with normal, build orthonormal basis
        Vec3f up = std::abs(normal[2]) < 0.9f ? Vec3f{0, 0, 1} : Vec3f{0, 1, 0};
        vx = normalized(cross(up, normal));
        vy = normalized(cross(normal, vx));
    }

    void gen(Tensor3f* coords, Tensor3f* normals, int w, int h,
             Vec3f ptr, float scale, float zOff, Vec3f = {0,0,0}) const override
    {
        const float inv = 1.0f / scale;
        Vec3f o = origin + ptr + normal * zOff;   // slice along the view axis
        if (coords) coords->create({h, w});
        if (normals) normals->create({h, w});
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float fx = (x - w * 0.5f) * inv;
                float fy = (y - h * 0.5f) * inv;
                if (coords) (*coords)(y, x) = o + vx * fx + vy * fy;
                if (normals) (*normals)(y, x) = normal;
            }
        }
    }
    Vec3f pointer() const override { return origin; }
};

// Flattened scroll segment: a grid of world points loaded from a tifxyz dir.
// gen() bilinearly resamples the point grid to the requested output size and
// derives per-pixel normals from the local grid tangents.
struct QuadSurface : Surface {
    Tensor3f points;        // {gh, gw} grid of world xyz; x==-1 marks invalid
    Vec2f gridScale{1, 1};  // grid steps per output pixel at scale 1 (meta "scale")
    std::string id;

    static constexpr float kInvalid = -1.0f;

    static std::shared_ptr<QuadSurface> load(const std::string& tifxyzDir);

    void gen(Tensor3f* coords, Tensor3f* normals, int w, int h,
             Vec3f ptr, float scale, float zOff, Vec3f zOffDir = {0,0,0}) const override;
    Vec3f pointer() const override;
    // Surface normal at a grid-coord view center (for capturing zOffDir).
    Vec3f normalAt(Vec3f ptr) const;
};

}  // namespace vc
