// Self-contained regression tests for the S3-independent render logic:
// pyramid level selection, the streaming compositor, and surface gen. These
// guard the math that perf optimizations are most likely to break. No S3.
//
// The full-pipeline perf/correctness check lives in bench/replay (needs S3).

#include "render/camera.hpp"
#include "render/compositing.hpp"
#include "render/surface.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace vc;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
static bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

static void testLevelForZoom()
{
    // L = ceil(-log2(scale)), clamped [0, n-1]. The bug we fixed: everything
    // resolving to level 0. Lock these boundaries.
    auto lvl = [](float s){ Camera c; c.scale = s; c.recalcLevel(6); return c.dsIdx; };
    CHECK(lvl(2.0f)   == 0);
    CHECK(lvl(1.0f)   == 0);
    CHECK(lvl(0.7f)   == 1);
    CHECK(lvl(0.5f)   == 1);
    CHECK(lvl(0.4f)   == 2);
    CHECK(lvl(0.25f)  == 2);
    CHECK(lvl(0.125f) == 3);
    CHECK(lvl(0.01f)  == 5);   // clamped to coarsest
    CHECK(lvl(0.001f) == 5);
}

static void testCompositor()
{
    float v[] = {10, 200, 50, 255, 0};
    {
        CompositeParams p; p.method = CompositeMethod::max;
        Compositor c(p); for (float x : v) c.add(x);
        CHECK(approx(c.value(), 255.0f));
    }
    {
        CompositeParams p; p.method = CompositeMethod::min;
        Compositor c(p); for (float x : v) c.add(x);
        CHECK(approx(c.value(), 0.0f));
    }
    {
        CompositeParams p; p.method = CompositeMethod::mean;
        Compositor c(p); for (float x : v) c.add(x);
        CHECK(approx(c.value(), (10+200+50+255+0)/5.0f));
    }
    {
        // alpha must early-out once accumulated alpha hits the cutoff
        CompositeParams p; p.method = CompositeMethod::alpha;
        p.alphaMin = 0; p.alphaMax = 1; p.alphaOpacity = 1; p.alphaCutoff = 0.5f;
        Compositor c(p);
        bool stopped = false;
        for (float x : {255.0f, 255.0f, 255.0f}) if (!c.add(x)) { stopped = true; break; }
        CHECK(stopped);                 // saturated before consuming all layers
        CHECK(c.value() > 0.0f);
    }
    {
        // empty ray -> 0 for every method
        CompositeParams p; p.method = CompositeMethod::mean;
        Compositor c(p);
        CHECK(approx(c.value(), 0.0f));
    }
}

static void testPlaneGen()
{
    // Plane centered at origin, +Z normal. coords should be origin + basis*px,
    // and the z-offset must shift the whole plane along its normal.
    PlaneSurface pl({100, 100, 100}, {0, 0, 1});
    Tensor3f coords, normals;
    pl.gen(&coords, &normals, 4, 4, {0,0,0}, 1.0f, 0.0f);
    CHECK(coords.rows() == 4 && coords.cols() == 4);
    // center pixel ~ origin
    Vec3f ctr = coords(2, 2);
    CHECK(approx(ctr[0], 100.0f, 1.5f) && approx(ctr[1], 100.0f, 1.5f) && approx(ctr[2], 100.0f));
    // normals all +Z
    CHECK(approx(normals(0,0)[2], 1.0f));
    // z-offset moves along normal (+Z)
    Tensor3f c2;
    pl.gen(&c2, nullptr, 4, 4, {0,0,0}, 1.0f, 10.0f);
    CHECK(approx(c2(2,2)[2], coords(2,2)[2] + 10.0f));
}

int main()
{
    testLevelForZoom();
    testCompositor();
    testPlaneGen();
    if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
    std::printf("all tests passed\n");
    return 0;
}
