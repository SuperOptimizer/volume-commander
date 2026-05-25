#pragma once

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "render/renderer.hpp"

namespace vc {

// Frame-parameter tracing for the offline benchmark. When VC_TRACE=<path> is
// set, each render appends one JSONL line capturing everything renderSurface
// needs (minus the volume/surface, which the bench reloads by URL). Replaying
// the trace headless gives a repeatable, real-interaction perf workload.
// Off (zero cost) unless the env var is set.
class FrameTrace {
public:
    static FrameTrace& instance() { static FrameTrace t; return t; }

    bool enabled() const { return f_ != nullptr; }

    void log(const char* view, int w, int h, const RenderInput& in)
    {
        if (!f_) return;
        const Camera& c = in.camera;
        const CompositeRenderSettings& cs = in.composite;
        std::lock_guard lk(m_);
        std::fprintf(f_,
            "{\"view\":\"%s\",\"w\":%d,\"h\":%d,"
            "\"px\":%.3f,\"py\":%.3f,\"pz\":%.3f,\"scale\":%.6f,"
            "\"zOff\":%.3f,\"zdx\":%.5f,\"zdy\":%.5f,\"zdz\":%.5f,\"lvl\":%d,"
            "\"wl\":%.1f,\"wh\":%.1f,\"samp\":%d,"
            "\"comp\":%d,\"method\":%d,\"lf\":%d,\"lb\":%d,\"plf\":%d,\"plb\":%d,"
            "\"clahe\":%d,\"raking\":%d,\"stretch\":%d}\n",
            view, w, h,
            c.surfacePtr[0], c.surfacePtr[1], c.surfacePtr[2], c.scale,
            c.zOff, c.zOffDir[0], c.zOffDir[1], c.zOffDir[2], c.dsIdx,
            in.windowLow, in.windowHigh, int(in.sampling),
            cs.enabled, int(cs.params.method), cs.layersFront, cs.layersBehind,
            cs.planeLayersFront, cs.planeLayersBehind,
            cs.postClaheEnabled, cs.postRakingEnabled, cs.postStretchValues);
        std::fflush(f_);
    }

private:
    FrameTrace() {
        if (const char* p = std::getenv("VC_TRACE")) f_ = std::fopen(p, "w");
    }
    ~FrameTrace() { if (f_) std::fclose(f_); }
    std::FILE* f_ = nullptr;
    std::mutex m_;
};

}  // namespace vc
