// Headless render benchmark: replays a VC_TRACE frame log through
// renderSurface and reports per-frame timings. Repeatable, GUI-free perf
// workload built from real interaction.
//
//   replay <trace.jsonl> <volume-url> [segment-url]
//
// Generate a trace by running the GUI with VC_TRACE=/tmp/trace.jsonl.

#include "render/renderer.hpp"
#include "data/volume.hpp"
#include "render/surface.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <print>

using namespace vc;
using Clock = std::chrono::steady_clock;

// minimal JSONL field scrapers (the trace is machine-written, fixed shape)
static double num(const std::string& s, const char* key, double dflt = 0) {
    auto p = s.find(std::string("\"") + key + "\":");
    if (p == s.npos) return dflt;
    return std::strtod(s.c_str() + p + std::strlen(key) + 3, nullptr);
}
static std::string str(const std::string& s, const char* key) {
    auto p = s.find(std::string("\"") + key + "\":\"");
    if (p == s.npos) return {};
    p += std::strlen(key) + 4;
    auto e = s.find('"', p);
    return s.substr(p, e - p);
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::println(stderr, "usage: replay <trace.jsonl> <volume-url> [segment-url]"); return 1; }
    auto vol = Volume::open(argv[2]);
    if (!vol) { std::println(stderr, "open volume failed"); return 1; }
    std::shared_ptr<QuadSurface> seg;
    if (argc > 3) seg = QuadSurface::load(argv[3]);

    // axis-aligned planes, built lazily like the GUI does
    auto shp0 = vol->shape(0);
    Vec3f c{shp0[2]*0.5f, shp0[1]*0.5f, shp0[0]*0.5f};
    PlaneSurface xy(c, {0,0,1}), xz(c, {0,1,0}), yz(c, {1,0,0});

    std::ifstream in(argv[1]);
    if (!in) { std::println(stderr, "open trace failed"); return 1; }

    struct Frame { RenderInput in; int w, h; std::string view; };
    std::vector<Frame> frames;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 5) continue;
        Frame fr;
        fr.view = str(line, "view");
        fr.w = int(num(line, "w")); fr.h = int(num(line, "h"));
        Camera cam;
        cam.surfacePtr = {float(num(line,"px")), float(num(line,"py")), float(num(line,"pz"))};
        cam.scale = float(num(line, "scale", 1));
        cam.zOff = float(num(line, "zOff"));
        cam.zOffDir = {float(num(line,"zdx")), float(num(line,"zdy")), float(num(line,"zdz"))};
        cam.dsIdx = int(num(line, "lvl"));
        fr.in.camera = cam;
        fr.in.volume = vol.get();
        fr.in.windowLow = float(num(line,"wl")); fr.in.windowHigh = float(num(line,"wh", 255));
        fr.in.sampling = Sampling(int(num(line,"samp")));
        auto& cs = fr.in.composite;
        cs.enabled = num(line,"comp") != 0;
        cs.params.method = CompositeMethod(int(num(line,"method")));
        cs.layersFront = int(num(line,"lf")); cs.layersBehind = int(num(line,"lb"));
        cs.planeLayersFront = int(num(line,"plf")); cs.planeLayersBehind = int(num(line,"plb"));
        cs.postClaheEnabled = num(line,"clahe") != 0;
        cs.postRakingEnabled = num(line,"raking") != 0;
        cs.postStretchValues = num(line,"stretch") != 0;
        fr.in.surf = fr.view == "seg" ? (Surface*)seg.get()
                   : fr.view == "xz"  ? (Surface*)&xz
                   : fr.view == "yz"  ? (Surface*)&yz : (Surface*)&xy;
        if (fr.in.surf) frames.push_back(std::move(fr));
    }
    std::println("loaded {} frames", frames.size());
    if (frames.empty()) return 0;

    // Warm the cache (one full pass, untimed) so we benchmark the renderer,
    // not S3 latency. Then time N passes.
    Tensor32 fb;
    for (auto& f : frames) renderSurface(fb, f.w, f.h, f.in);

    const int passes = 3;
    std::vector<double> times;
    times.reserve(frames.size() * passes);
    auto t0 = Clock::now();
    for (int p = 0; p < passes; ++p)
        for (auto& f : frames) {
            auto a = Clock::now();
            renderSurface(fb, f.w, f.h, f.in);
            times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-a).count());
        }
    double total = std::chrono::duration<double,std::milli>(Clock::now()-t0).count();

    std::sort(times.begin(), times.end());
    double sum = 0; for (double t : times) sum += t;
    auto pct = [&](double q){ return times[std::min(times.size()-1, size_t(q*times.size()))]; };
    std::println("frames={} mean={:.2f}ms p50={:.2f} p95={:.2f} max={:.2f} total={:.0f}ms fps_mean={:.0f}",
        times.size(), sum/times.size(), pct(0.50), pct(0.95), times.back(), total, 1000.0/(sum/times.size()));
    return 0;
}
