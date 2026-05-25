#include "render/surface.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <print>

#include <tiffio.h>

namespace vc {
namespace {

// Read one single-channel TIFF into channel `ch` of `pts` (allocating on the
// first band). Handles float32/64 and (u)int 8/16/32. Strip and tiled layouts.
bool readBand(const std::filesystem::path& f, Tensor3f& pts, int ch)
{
    TIFF* tif = TIFFOpen(f.string().c_str(), "r");
    if (!tif) { std::println(stderr, "tifxyz: cannot open {}", f.string()); return false; }
    uint32_t W = 0, H = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &W);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &H);
    uint16_t bps = 0, fmt = SAMPLEFORMAT_UINT;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &fmt);

    if (pts.empty()) {
        pts.reshape({int(H), int(W)});
        for (auto& p : pts.data) p = {QuadSurface::kInvalid, QuadSurface::kInvalid, QuadSurface::kInvalid};
    }
    auto toF = [&](const uint8_t* p) -> float {
        if (fmt == SAMPLEFORMAT_IEEEFP) {
            if (bps == 32) { float v; std::memcpy(&v, p, 4); return v; }
            if (bps == 64) { double d; std::memcpy(&d, p, 8); return float(d); }
        } else if (fmt == SAMPLEFORMAT_INT) {
            if (bps == 8)  return float(*reinterpret_cast<const int8_t*>(p));
            if (bps == 16) { int16_t v; std::memcpy(&v, p, 2); return float(v); }
            if (bps == 32) { int32_t v; std::memcpy(&v, p, 4); return float(v); }
        } else {
            if (bps == 8)  return float(*p);
            if (bps == 16) { uint16_t v; std::memcpy(&v, p, 2); return float(v); }
            if (bps == 32) { uint32_t v; std::memcpy(&v, p, 4); return float(v); }
        }
        return 0.0f;
    };
    const int bpc = (bps + 7) / 8;

    if (TIFFIsTiled(tif)) {
        uint32_t tw = 0, th = 0;
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
        std::vector<uint8_t> buf(TIFFTileSize(tif));
        for (uint32_t y0 = 0; y0 < H; y0 += th)
            for (uint32_t x0 = 0; x0 < W; x0 += tw) {
                if (TIFFReadEncodedTile(tif, TIFFComputeTile(tif, x0, y0, 0, 0), buf.data(), buf.size()) < 0)
                    continue;
                for (uint32_t ty = 0; ty < th && y0 + ty < H; ++ty)
                    for (uint32_t tx = 0; tx < tw && x0 + tx < W; ++tx)
                        pts(int(y0 + ty), int(x0 + tx))[ch] = toF(buf.data() + (ty * tw + tx) * bpc);
            }
    } else {
        std::vector<uint8_t> row(TIFFScanlineSize(tif));
        for (uint32_t y = 0; y < H; ++y) {
            if (TIFFReadScanline(tif, row.data(), y) < 0) continue;
            for (uint32_t x = 0; x < W; ++x)
                pts(int(y), int(x))[ch] = toF(row.data() + x * bpc);
        }
    }
    TIFFClose(tif);
    return true;
}

// Tiny scrape of "scale": [a, b] from meta.json — full JSON parse is overkill.
Vec2f readScale(const std::filesystem::path& metaPath)
{
    std::ifstream in(metaPath);
    if (!in) return {1, 1};
    std::string s((std::istreambuf_iterator<char>(in)), {});
    auto p = s.find("\"scale\"");
    if (p == s.npos) return {1, 1};
    p = s.find('[', p);
    if (p == s.npos) return {1, 1};
    float a = 1, b = 1;
    std::sscanf(s.c_str() + p, "[ %f , %f", &a, &b);
    return {a, b};
}

}  // namespace

std::shared_ptr<QuadSurface> QuadSurface::load(const std::string& dir)
{
    auto qs = std::make_shared<QuadSurface>();
    std::filesystem::path d(dir);
    if (!readBand(d / "x.tif", qs->points, 0) ||
        !readBand(d / "y.tif", qs->points, 1) ||
        !readBand(d / "z.tif", qs->points, 2)) {
        return nullptr;
    }
    qs->gridScale = readScale(d / "meta.json");
    qs->id = d.filename().string();
    return qs;
}

Vec3f QuadSurface::pointer() const
{
    return {points.cols() * 0.5f, points.rows() * 0.5f, 0};
}

void QuadSurface::gen(Tensor3f* coords, Tensor3f* normals, int w, int h,
                      Vec3f ptr, float scale, Vec3f offset) const
{
    if (coords) coords->create({h, w});
    if (normals) normals->create({h, w});
    const int gh = points.rows(), gw = points.cols();
    if (gh < 2 || gw < 2) return;

    // grid steps per output pixel; center the view on ptr (grid coords).
    const float sx = gridScale[0] / scale;
    const float sy = gridScale[1] / scale;
    const float cx = ptr[0] + offset[0];
    const float cy = ptr[1] + offset[1];

    auto valid = [&](const Vec3f& v) { return v[0] != kInvalid && std::isfinite(v[0]); };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float gx = cx + (x - w * 0.5f) * sx;
            float gy = cy + (y - h * 0.5f) * sy;
            int ix = int(std::floor(gx)), iy = int(std::floor(gy));
            Vec3f world{kInvalid, kInvalid, kInvalid};
            Vec3f nrm{0, 0, 1};
            if (ix >= 0 && iy >= 0 && ix < gw - 1 && iy < gh - 1) {
                const Vec3f& p00 = points(iy, ix);
                const Vec3f& p10 = points(iy, ix + 1);
                const Vec3f& p01 = points(iy + 1, ix);
                const Vec3f& p11 = points(iy + 1, ix + 1);
                if (valid(p00) && valid(p10) && valid(p01) && valid(p11)) {
                    float fx = gx - ix, fy = gy - iy;
                    Vec3f a = p00 + (p10 - p00) * fx;
                    Vec3f b = p01 + (p11 - p01) * fx;
                    world = a + (b - a) * fy;
                    nrm = normalized(cross(p10 - p00, p01 - p00));
                }
            }
            if (coords) (*coords)(y, x) = world;
            if (normals) (*normals)(y, x) = nrm;
        }
    }
}

}  // namespace vc
