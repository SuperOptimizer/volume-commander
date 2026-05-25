#include "app/state.hpp"
#include <print>

namespace vc {

bool AppState::openVolume(const QString& url)
{
    auto v = Volume::open(url.toStdString());
    if (!v) { std::println(stderr, "openVolume failed: {}", url.toStdString()); return false; }
    volume_ = v;
    volumeUrl_ = url;
    mask_.reset();  // mask is sized to the new volume on first paint
    emit volumeChanged();
    bump();
    return true;
}

bool AppState::loadSegment(const QString& dir)
{
    auto s = QuadSurface::load(dir.toStdString());
    if (!s) { std::println(stderr, "loadSegment failed: {}", dir.toStdString()); return false; }
    segment_ = s;
    segmentDir_ = dir;
    emit segmentChanged();
    bump();
    return true;
}

void AppState::saveMask(const QString& dir)
{
    // TODO(#8): serialize mask_ chunks to a c3d-sharded zarr at `dir`.
    std::println("saveMask -> {} (not yet implemented)", dir.toStdString());
}

void AppState::setCompositeMethod(const QString& m)
{
    methodName_ = m;
    composite_.params.method = parseComposite(m.toStdString());
    bump();
}

}  // namespace vc
