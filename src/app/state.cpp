#include "app/state.hpp"
#include <print>
#include <fstream>
#include <filesystem>
#include <cstdio>

namespace vc {

bool AppState::openVolume(const QString& url)
{
    auto v = Volume::open(url.toStdString());
    if (!v) { std::println(stderr, "openVolume failed: {}", url.toStdString()); return false; }
    // chunk-ready fires on an IOPool thread; hop to GUI via a queued signal.
    v->setChunkReady([this] { QMetaObject::invokeMethod(this, "chunkArrived", Qt::QueuedConnection); });
    volume_ = v;
    volumeUrl_ = url;
    mask_.reset();  // mask is sized to the new volume on first paint
    emit volumeChanged();
    bump();
    return true;
}

bool AppState::openOverlay(const QString& url)
{
    auto v = Volume::open(url.toStdString());
    if (!v) { std::println(stderr, "openOverlay failed: {}", url.toStdString()); return false; }
    v->setChunkReady([this] { QMetaObject::invokeMethod(this, "chunkArrived", Qt::QueuedConnection); });
    overlay_ = v;
    overlayUrl_ = url;
    emit overlayChanged();
    bump();
    return true;
}

bool AppState::loadSegment(const QString& dir)
{
    auto s = QuadSurface::load(dir.toStdString());
    if (!s) { std::println(stderr, "loadSegment failed: {}", dir.toStdString()); return false; }
    std::println("loaded segment {} grid {}x{}", dir.toStdString(), s->points.rows(), s->points.cols());
    segment_ = s;
    segmentDir_ = dir;
    emit segmentChanged();
    bump();
    return true;
}

void AppState::paintAt(Vec3f world, bool erase)
{
    if (!volume_) return;
    if (!mask_) mask_ = std::make_shared<MaskVolume>(volume_->shape(0));
    mask_->paintSphere(world, brushRadius_, erase ? 0 : 255);
    // no bump(): the painting viewer repaints itself; others repaint on next nav.
}

void AppState::saveMask(const QString& dir)
{
    if (!mask_) { std::println("saveMask: nothing painted"); return; }
    namespace fs = std::filesystem;
    fs::create_directories(dir.toStdString());
    // Sidecar dump: one raw 256^3 file per painted chunk named z_y_x.bin, plus
    // shape.txt. Compact c3d-sharded export can replace this later.
    auto root = fs::path(dir.toStdString());
    {
        std::ofstream sh(root / "shape.txt");
        auto s = mask_->shape();
        sh << s[0] << ' ' << s[1] << ' ' << s[2] << '\n';
    }
    int n = 0;
    mask_->forEachChunk([&](const ChunkId& id, const std::uint8_t* data) {
        char name[64];
        std::snprintf(name, sizeof name, "%d_%d_%d.bin", id.iz, id.iy, id.ix);
        std::ofstream f(root / name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data), std::size_t(kChunk)*kChunk*kChunk);
        ++n;
    });
    mask_->clearDirty();
    std::println("saveMask -> {} ({} chunks)", dir.toStdString(), n);
}

void AppState::setCompositeMethod(const QString& m)
{
    methodName_ = m;
    composite_.params.method = parseComposite(m.toStdString());
    bump();
}

}  // namespace vc
