#include "app/viewer_item.hpp"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QThreadPool>

#include "app/trace.hpp"

namespace vc {

ViewerItem::ViewerItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(false);
    // worker thread -> GUI thread frame handoff
    connect(this, &ViewerItem::frameReady, this, &ViewerItem::onFrameReady, Qt::QueuedConnection);
}
ViewerItem::~ViewerItem() = default;

void ViewerItem::setState(AppState* s)
{
    if (state_ == s) return;
    if (state_) disconnect(state_, nullptr, this, nullptr);
    state_ = s;
    if (state_) {
        connect(state_, &AppState::renderChanged, this, [this] { rebuildSurface(); scheduleRender(); });
        connect(state_, &AppState::volumeChanged, this, [this] { rebuildSurface(); scheduleRender(); });
        connect(state_, &AppState::segmentChanged, this, [this] { rebuildSurface(); scheduleRender(); });
        // Progressive refinement: when a queued chunk finishes decoding into
        // the block cache, re-render so the visible region sharpens to its
        // best resident level. Coalesced (one re-render per burst of arrivals)
        // so streaming many chunks doesn't fire a render each. This converges
        // — it stops re-rendering once no new chunks land — because the level
        // selection bounds demand to what the current zoom needs.
        connect(state_, &AppState::chunkArrived, this, [this] {
            if (!refineTimer_) {
                refineTimer_ = new QTimer(this);
                refineTimer_->setSingleShot(true);
                connect(refineTimer_, &QTimer::timeout, this, [this] { scheduleRender(); });
            }
            if (!refineTimer_->isActive()) refineTimer_->start(60);
        });
    }
    emit stateChanged();
    rebuildSurface();
    scheduleRender();
}

void ViewerItem::setView(const QString& v)
{
    if (viewName_ == v) return;
    viewName_ = v;
    emit viewChanged();
    rebuildSurface();
    scheduleRender();
}

void ViewerItem::rebuildSurface()
{
    if (!state_ || !state_->volume()) { surface_.reset(); return; }
    if (viewName_ == "seg") {
        surface_ = state_->segment();
        if (surface_) camera_.surfacePtr = surface_->pointer();
    } else {
        auto shp = state_->volume()->shape(0);  // {z,y,x}
        Vec3f c{shp[2] * 0.5f, shp[1] * 0.5f, shp[0] * 0.5f};  // world is (x,y,z)
        Vec3f n = viewName_ == "xy" ? Vec3f{0, 0, 1}
                : viewName_ == "xz" ? Vec3f{0, 1, 0}
                                    : Vec3f{1, 0, 0};
        auto pl = std::make_shared<PlaneSurface>(c, n);
        surface_ = pl;
        camera_.surfacePtr = {0, 0, 0};
        // Default zoom: fit the whole in-plane extent into the viewport, so we
        // open at the COARSEST pyramid level (few chunks) instead of level 0.
        int W = std::max(1, int(width())), H = std::max(1, int(height()));
        float extentX = float(viewName_ == "yz" ? shp[1] : shp[2]);   // in-plane width (voxels)
        float extentY = float(viewName_ == "xy" ? shp[1] : shp[0]);   // in-plane height
        float fit = std::min(W / extentX, H / extentY);
        camera_.scale = Camera::roundScale(fit > 0 ? fit : 0.01f);
    }
    camera_.recalcLevel(state_->volume()->numLevels());
}

void ViewerItem::beginInteraction()
{
    interactive_ = true;
    if (!idleTimer_) {
        idleTimer_ = new QTimer(this);
        idleTimer_->setSingleShot(true);
        connect(idleTimer_, &QTimer::timeout, this, [this] {
            interactive_ = false;   // motion stopped -> one full-res render
            scheduleRender();
        });
    }
    idleTimer_->start(140);
}

// 60fps coalescing gate. Every trigger (pan, zoom, refine, settings) calls
// this; it dispatches at most one render per 16ms. An input storm (mouseMove
// faster than 60Hz) collapses to <=60 renders/sec instead of queueing.
void ViewerItem::scheduleRender()
{
    using namespace std::chrono;
    auto now = steady_clock::now();
    double sinceMs = duration<double, std::milli>(now - lastDispatch_).count();
    if (sinceMs >= 16.0) { lastDispatch_ = now; dispatchRender(); return; }
    if (!frameTimer_) {
        frameTimer_ = new QTimer(this);
        frameTimer_->setSingleShot(true);
        connect(frameTimer_, &QTimer::timeout, this, [this] {
            lastDispatch_ = steady_clock::now(); dispatchRender();
        });
    }
    if (!frameTimer_->isActive()) frameTimer_->start(int(16.0 - sinceMs) + 1);
}

void ViewerItem::dispatchRender()
{
    int w = int(width()), h = int(height());
    if (!state_ || !surface_ || !state_->volume() || w <= 0 || h <= 0) return;
    if (busy_.exchange(true)) { pending_ = true; return; }   // one render in flight

    // Interactive downscale: while panning/zooming, render at 1/2 res (4x fewer
    // samples) for a responsive feel, then a full-res pass once motion stops
    // (idle timer below). paint() scales the framebuffer up to the item rect.
    // Both the pixel dims AND the camera scale are divided by ds, so the
    // half-res frame covers the SAME world area (fewer pixels, same extent) —
    // otherwise it would appear zoomed in and "pop" back on release.
    int ds = interactive_ ? 2 : 1;
    w = std::max(1, w / ds);
    h = std::max(1, h / ds);

    // Immutable snapshot for the worker — GUI thread mutates camera_ freely.
    auto snap = std::make_shared<RenderInput>();
    snap->surf = surface_.get();        // surfaces live as long as state_; ok for snapshot
    snap->surfHold = surface_;          // keep alive across the worker run
    snap->volume = state_->volume();
    snap->camera = camera_;
    snap->camera.scale = camera_.scale / float(ds);
    // Coarser pyramid level while moving: 1/8 the voxels + far fewer block
    // fetches. Snaps back to the correct level when idle (free static quality,
    // like the downscale). Clamp to the coarsest available level.
    if (interactive_ && state_->volume())
        snap->camera.dsIdx = std::min(camera_.dsIdx + 1, state_->volume()->numLevels() - 1);
    snap->composite = state_->composite();
    snap->windowLow = state_->windowLow();
    snap->windowHigh = state_->windowHigh();
    snap->sampling = state_->sampling();
    // Ray subsampling: aggressive (4) while moving, light (2) when idle. For
    // max/mean over many layers stride-2 is near-indistinguishable; full
    // quality (1) is available but the heavy seg frames don't need it.
    snap->layerStride = interactive_ ? 4 : 2;
    snap->mask = state_->mask();

    if (FrameTrace::instance().enabled())
        FrameTrace::instance().log(viewName_.toUtf8().constData(), w, h, *snap);

    QThreadPool::globalInstance()->start([this, snap, w, h] {
        // scratch_/fb_ persist across frames (busy_ gate => one render at a
        // time per item) so same-size frames reuse buffers — no per-frame
        // page-fault/zero-fill churn.
        bool missed = renderSurface(fb_, w, h, *snap, scratch_);
        const Tensor32& fb = fb_;
        QImage img(w, h, QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y)
            std::memcpy(img.scanLine(y), fb.row(y), std::size_t(w) * 4);
        emit frameReady(img, missed);  // queued -> onFrameReady on GUI thread
    });
}

void ViewerItem::onFrameReady(QImage img, bool /*missed*/)
{
    image_ = std::move(img);
    busy_.store(false);
    update();
    if (pending_) { pending_ = false; scheduleRender(); }
    // No refine loop. We rendered the best data available (camera level, else
    // coarser, else black) and queued any missing blocks. When the IO pool
    // finishes one, Volume fires chunkArrived -> a single re-render. Done.
}

void ViewerItem::paint(QPainter* p)
{
    if (image_.isNull()) { p->fillRect(boundingRect(), Qt::black); return; }
    if (image_.width() != int(width()) || image_.height() != int(height())) {
        p->drawImage(boundingRect(), image_);   // scale downscaled frame to fill
        return;
    }
    p->drawImage(0, 0, image_);
}

void ViewerItem::requestRender() { scheduleRender(); }

void ViewerItem::geometryChange(const QRectF& n, const QRectF& o)
{
    QQuickPaintedItem::geometryChange(n, o);
    if (n.size() != o.size()) scheduleRender();
}

bool ViewerItem::worldAt(QPointF pos, Vec3f& out) const
{
    if (!surface_) return false;
    const int w = int(width()), h = int(height());
    int px = std::clamp(int(pos.x()), 0, std::max(0, w - 1));
    int py = std::clamp(int(pos.y()), 0, std::max(0, h - 1));
    Tensor3f coords;
    surface_->gen(&coords, nullptr, w, h, camera_.surfacePtr, camera_.scale, camera_.zOff, camera_.zOffDir);
    if (coords.empty()) return false;
    Vec3f c = coords(py, px);
    if (c[0] == QuadSurface::kInvalid || !std::isfinite(c[0])) return false;
    out = c;
    return true;
}

void ViewerItem::mousePressEvent(QMouseEvent* e)
{
    lastPan_ = e->position();
    const bool paint = state_ && state_->maskPaint();
    if (paint && (e->button() == Qt::LeftButton || e->button() == Qt::RightButton)) {
        Vec3f w;
        if (worldAt(e->position(), w)) { state_->paintAt(w, e->button() == Qt::RightButton); scheduleRender(); }
        painting_ = true; eraseStroke_ = (e->button() == Qt::RightButton);
        return;
    }
    panning_ = (e->button() == Qt::LeftButton);
}

void ViewerItem::mouseMoveEvent(QMouseEvent* e)
{
    if (painting_) {
        Vec3f w;
        if (worldAt(e->position(), w)) { state_->paintAt(w, eraseStroke_); scheduleRender(); }
        return;
    }
    if (!panning_ || !surface_) return;
    QPointF d = e->position() - lastPan_;
    lastPan_ = e->position();
    float inv = 1.0f / camera_.scale;
    if (surface_->isPlane()) {
        auto* pl = static_cast<PlaneSurface*>(surface_.get());
        camera_.surfacePtr -= pl->vx * float(d.x() * inv);
        camera_.surfacePtr -= pl->vy * float(d.y() * inv);
    } else {
        // QuadSurface ptr is in GRID coords; gen maps one pixel to
        // gridScale/scale grid steps, so the pan delta needs that factor too
        // (without it pan is ~1/gridScale too fast — e.g. 20x at gridScale .05).
        auto* qs = static_cast<QuadSurface*>(surface_.get());
        camera_.surfacePtr[0] -= float(d.x()) * qs->gridScale[0] * inv;
        camera_.surfacePtr[1] -= float(d.y()) * qs->gridScale[1] * inv;
        // Clamp to the grid so a drag can't fling the view into empty space.
        camera_.surfacePtr[0] = std::clamp(camera_.surfacePtr[0], 0.0f, float(qs->points.cols()));
        camera_.surfacePtr[1] = std::clamp(camera_.surfacePtr[1], 0.0f, float(qs->points.rows()));
    }
    beginInteraction();
    scheduleRender();
}

void ViewerItem::mouseReleaseEvent(QMouseEvent*)
{
    panning_ = false;
    painting_ = false;
}

void ViewerItem::wheelEvent(QWheelEvent* e)
{
    int steps = e->angleDelta().y() / 120;
    if (!steps) return;
    if (e->modifiers() & Qt::ShiftModifier) {
        // Capture the push direction ONCE (at the first scroll off zero) from
        // the surface normal at view center; hold it fixed so later pans don't
        // reshape the sheet. Plane views ignore zOffDir (gen uses their normal).
        if (camera_.zOff == 0.0f && !surface_->isPlane()) {
            auto* qs = static_cast<QuadSurface*>(surface_.get());
            Vec3f n = qs->normalAt(camera_.surfacePtr);
            if (vc::norm(n) > 1e-6f) camera_.zOffDir = n;
        }
        camera_.zOff += float(steps);      // slice through normal
        if (camera_.zOff == 0.0f) camera_.zOffDir = {0, 0, 0};
    } else {
        camera_.zoom(steps);
        if (state_ && state_->volume()) camera_.recalcLevel(state_->volume()->numLevels());
    }
    beginInteraction();      // low-res while zooming; idle timer does full-res
    scheduleRender();        // 60fps gate coalesces the wheel burst
}

}  // namespace vc
