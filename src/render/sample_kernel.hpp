#pragma once

// Vectorizable composite ray kernel. Renders a horizontal run of pixels with
// per-pixel rays in lockstep: for each composite layer, gather one voxel per
// pixel and fold into a per-pixel accumulator (max/mean/min/alpha). Written as
// plain loops over `n` pixels so the compiler auto-vectorizes the per-pixel
// arithmetic at the -m flags of whichever TU instantiates it (AVX2 / AVX-512).
//
// The block lookup stays scalar (storage is block-scattered) but the value
// processing — iso threshold, max/mean accumulate, lighting, clamp — runs SIMD
// across the pixel run. With median 52 layers/pixel that arithmetic dominates.

#include <cstdint>
#include <cmath>
#include <algorithm>
#include "util/math.hpp"
#include "render/compositing.hpp"
#include "data/volume.hpp"
#include "render/surface.hpp"

namespace vc {

// Inputs for one pixel run (length n, n <= MAXRUN). Coords/normals already
// generated; this just walks rays + composites.
struct RowKernelArgs {
    Volume* vol;
    int lvl;
    int nLayers;
    int zStart;
    int stride = 1;   // voxels advanced per sampled layer (ray subsampling)
    CompositeMethod method;
    float iso;
    float alphaLo, alphaInvRange, alphaOpacity, alphaCutoff;
    bool lightingEnabled;
    CompositeParams lightParams;
};

inline constexpr int kMaxRun = 16;   // AVX-512 lane count

using CompositeRunFn = void(*)(const struct RowKernelArgs&, int,
                               const Vec3f*, const Vec3f*, std::uint8_t*, bool&);
// Returns the best run kernel for this CPU (AVX-512 / AVX2 / scalar).
CompositeRunFn pickCompositeRun();

// Render `n` pixels. base[i]/nrm[i] are world coord + normal for pixel i.
// Writes gray[i]. `cur` is the scalar block cursor (shared, reset per call).
template <int LANES>
void compositeRun(const RowKernelArgs& a, int n,
                  const Vec3f* base, const Vec3f* nrm,
                  std::uint8_t* gray, bool& missed)
{
    Volume& vol = *a.vol;
    const float f = 1.0f / float(1 << a.lvl);
    auto shp = vol.shape(a.lvl);
    const int sz = shp[0], sy = shp[1], sx = shp[2];

    // per-pixel ray state + accumulators
    float qx[kMaxRun], qy[kMaxRun], qz[kMaxRun], dx[kMaxRun], dy[kMaxRun], dz[kMaxRun];
    float acc[kMaxRun], amax[kMaxRun], amin[kMaxRun], aalpha[kMaxRun];
    int   cnt[kMaxRun];
    bool  valid[kMaxRun];
    for (int i = 0; i < n; ++i) {
        valid[i] = base[i][0] != QuadSurface::kInvalid && std::isfinite(base[i][0]);
        Vec3f q = (base[i] + nrm[i] * float(a.zStart)) * f;
        Vec3f d = nrm[i] * (f * float(a.stride));
        qx[i]=q[0]; qy[i]=q[1]; qz[i]=q[2]; dx[i]=d[0]; dy[i]=d[1]; dz[i]=d[2];
        acc[i]=0; amax[i]=0; amin[i]=255; aalpha[i]=0; cnt[i]=0;
    }

    // Per-lane last-block cache (consecutive layers usually hit the same
    // block). Gather stays scalar (blocks are scattered); SIMD win is the
    // per-lane arithmetic below.
    struct Cur { int level=-1,bz=-1,by=-1,bx=-1; const Block* blk=nullptr; };
    Cur cur[kMaxRun];

    for (int li = 0; li < a.nLayers; ++li) {
        float sample[kMaxRun];
        // gather: one voxel per lane (scalar block resolve + index)
        for (int i = 0; i < n; ++i) {
            if (!valid[i]) { sample[i] = -1.0f; continue; }
            int ix = int(qx[i]+0.5f), iy = int(qy[i]+0.5f), iz = int(qz[i]+0.5f);
            int bz = iz>>4, by = iy>>4, bx = ix>>4;
            Cur& c = cur[i];
            if (a.lvl!=c.level||bz!=c.bz||by!=c.by||bx!=c.bx) {
                if (ix<0||iy<0||iz<0||iz>=sz||iy>=sy||ix>=sx) { c.blk=nullptr; c.level=a.lvl;c.bz=bz;c.by=by;c.bx=bx; sample[i]=0; continue; }
                c.level=a.lvl;c.bz=bz;c.by=by;c.bx=bx;
                c.blk = vol.block(a.lvl,bz,by,bx);
                if (!c.blk) { missed = true; sample[i] = 0; continue; }
            } else if (!c.blk) { sample[i]=0; continue; }
            sample[i] = float(c.blk->v[(std::size_t(iz&15)*kBlock + (iy&15))*kBlock + (ix&15)]);
        }
        // SIMD arithmetic across lanes: iso + accumulate per method
        const float iso = a.iso;
        switch (a.method) {
            case CompositeMethod::max:
                // branchless max (the priority path): invalid lanes sample 0,
                // their amax is discarded in finalize, so no valid[] guard.
                for (int i=0;i<n;++i){ float v=sample[i]<iso?0.f:sample[i]; amax[i]=v>amax[i]?v:amax[i]; }
                break;
            case CompositeMethod::min:
                for (int i=0;i<n;++i){ float v=sample[i]<iso?0.f:sample[i]; if(valid[i]){ if(v<amin[i])amin[i]=v; cnt[i]=1; } }
                break;
            case CompositeMethod::alpha:
                for (int i=0;i<n;++i){ if(!valid[i])continue; float v=sample[i]<iso?0.f:sample[i];
                    float nn=(v-a.alphaLo)*a.alphaInvRange; if(nn>0){ nn=nn>1?1:nn; float op=nn*a.alphaOpacity; op=op>1?1:op;
                    float w=(1.f-aalpha[i])*op; acc[i]+=w*nn; aalpha[i]+=w; } }
                break;
            default: // mean
                for (int i=0;i<n;++i){ if(!valid[i])continue; float v=sample[i]<iso?0.f:sample[i]; acc[i]+=v; cnt[i]++; }
                break;
        }
        for (int i=0;i<n;++i){ qx[i]+=dx[i]; qy[i]+=dy[i]; qz[i]+=dz[i]; }
    }

    // finalize per pixel
    for (int i = 0; i < n; ++i) {
        if (!valid[i]) { gray[i] = 0; continue; }
        float val;
        switch (a.method) {
            case CompositeMethod::max: val = amax[i]; break;
            case CompositeMethod::min: val = cnt[i]?amin[i]:0; break;
            case CompositeMethod::alpha: val = std::clamp(acc[i]*255.f,0.f,255.f); break;
            default: val = cnt[i] ? acc[i]/float(cnt[i]) : 0; break;
        }
        if (a.lightingEnabled) val *= computeLightingFactor(nrm[i], a.lightParams);
        gray[i] = std::uint8_t(std::clamp(val, 0.0f, 255.0f));
    }
}

}  // namespace vc
