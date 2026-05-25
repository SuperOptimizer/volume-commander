// Mirror the S3 chunks a trace touches into a real local c3d-sharded zarr
// (verbatim shard + zarr.json copy). The result is a normal local zarr that
// replay/tests/GUI open via the filesystem path — zero S3, fully reproducible.
//
//   mirror <trace.jsonl> <s3-volume-url> <out-dir> [seg-url] [seg-out-dir]
//
// Walks each trace frame, figures out which shards it samples, and copies
// those whole shards (+ each level's zarr.json) to <out-dir>/<level>/...

#include "render/renderer.hpp"
#include "data/volume.hpp"
#include "render/surface.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <filesystem>
#include <print>

extern "C" {
#include <libs3.h>
}

using namespace vc;
namespace fs = std::filesystem;

static double num(const std::string& s, const char* key, double dflt = 0) {
    auto p = s.find(std::string("\"") + key + "\":");
    if (p == s.npos) return dflt;
    return std::strtod(s.c_str() + p + std::strlen(key) + 3, nullptr);
}
static std::string str(const std::string& s, const char* key) {
    auto p = s.find(std::string("\"") + key + "\":\"");
    if (p == s.npos) return {};
    p += std::strlen(key) + 4;
    return s.substr(p, s.find('"', p) - p);
}

// fetch an S3 object to a local file, mkdir -p the parents. Skips if exists.
static bool copyObject(s3_client* c, const std::string& url, const fs::path& out) {
    if (fs::exists(out)) return true;
    s3_response r{};
    bool ok = s3_get(c, url.c_str(), &r) == S3_OK && r.status == 200 && r.body;
    if (ok) {
        fs::create_directories(out.parent_path());
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(r.body), r.body_len);
    }
    s3_response_free(&r);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::println(stderr, "usage: mirror <trace.jsonl> <s3-volume-url> <out-dir> [seg-url] [seg-out-dir]");
        return 1;
    }
    std::string traceP = argv[1], volUrl = argv[2], outDir = argv[3];
    while (volUrl.size() && volUrl.back() == '/') volUrl.pop_back();

    auto vol = Volume::open(volUrl);   // for level shapes + surface geometry
    if (!vol) { std::println(stderr, "open volume failed"); return 1; }
    auto shp0 = vol->shape(0);
    Vec3f ctr{shp0[2]*0.5f, shp0[1]*0.5f, shp0[0]*0.5f};
    PlaneSurface xy(ctr,{0,0,1}), xz(ctr,{0,1,0}), yz(ctr,{1,0,0});
    std::shared_ptr<QuadSurface> seg;
    if (argc > 4) seg = QuadSurface::load(argv[4]);

    s3_config cfg{}; s3_credentials_load("default", &cfg.creds);
    s3_client* s3 = s3_client_new(&cfg);

    // copy each level's zarr.json
    for (int l = 0; l < vol->numLevels(); ++l)
        copyObject(s3, volUrl + "/" + std::to_string(l) + "/zarr.json",
                   fs::path(outDir) / std::to_string(l) / "zarr.json");

    // walk frames -> sample coords -> shard ids touched
    std::set<std::array<int,4>> shards;   // {level, sz, sy, sx}
    std::ifstream in(traceP);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 5) continue;
        std::string view = str(line, "view");
        int w = int(num(line,"w")), h = int(num(line,"h"));
        Camera cam;
        cam.surfacePtr = {float(num(line,"px")),float(num(line,"py")),float(num(line,"pz"))};
        cam.scale = float(num(line,"scale",1)); cam.dsIdx = int(num(line,"lvl"));
        const Surface* s = view=="seg"?(Surface*)seg.get():view=="xz"?(Surface*)&xz:view=="yz"?(Surface*)&yz:(Surface*)&xy;
        if (!s) continue;
        Tensor3f coords;
        s->gen(&coords, nullptr, w, h, cam.surfacePtr, cam.scale, cam.zOff, cam.zOffDir);
        int L = cam.dsIdx; float f = 1.0f/float(1<<L); auto ls = vol->shape(L);
        for (auto& p : coords.data) {
            if (p[0]==QuadSurface::kInvalid || !std::isfinite(p[0])) continue;
            int ix=int(p[0]*f), iy=int(p[1]*f), iz=int(p[2]*f);
            if (ix<0||iy<0||iz<0||iz>=ls[0]||iy>=ls[1]||ix>=ls[2]) continue;
            // sample a small z-neighborhood too (composite layers cross shards)
            for (int dz=-32; dz<=32; dz+=16) {
                int z=iz+dz; if (z<0||z>=ls[0]) continue;
                shards.insert({L, z/4096, iy/4096, ix/4096});
            }
        }
    }
    std::println("trace touches {} shards across {} levels", shards.size(), vol->numLevels());

    int copied = 0, missing = 0;
    for (auto& [L,sz,sy,sx] : shards) {
        std::string key = "/" + std::to_string(L) + "/c/" + std::to_string(sz) + "/"
                        + std::to_string(sy) + "/" + std::to_string(sx);
        if (copyObject(s3, volUrl + key, fs::path(outDir) / (std::to_string(L)) / "c"
                       / std::to_string(sz) / std::to_string(sy) / std::to_string(sx)))
            ++copied;
        else ++missing;
        if ((copied+missing) % 20 == 0) std::print("\r  {}/{} shards", copied+missing, shards.size()), std::fflush(stdout);
    }
    std::println("\nmirrored {} shards ({} absent) to {}", copied, missing, outDir);

    // mirror the segment tifxyz too, if given
    if (seg && argc > 5) {
        std::string segUrl = argv[4]; while (segUrl.size()&&segUrl.back()=='/') segUrl.pop_back();
        for (const char* fn : {"x.tif","y.tif","z.tif","meta.json"})
            copyObject(s3, segUrl + "/" + fn, fs::path(argv[5]) / fn);
        std::println("mirrored segment -> {}", argv[5]);
    }
    s3_client_free(s3);
    return 0;
}
