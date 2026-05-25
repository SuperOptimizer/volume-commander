#include "app/viewer_item.hpp"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>

namespace vc {

ViewerItem::ViewerItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(false);
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
    }
    camera_.recalcLevel(state_->volume()->numLevels());
}

void ViewerItem::scheduleRender()
{
    if (busy_.load()) { pending_ = true; return; }
    renderNow();
}

void ViewerItem::renderNow()
{
    const int w = int(width()), h = int(height());
    if (!state_ || !surface_ || !state_->volume() || w <= 0 || h <= 0) { update(); return; }

    RenderInput in;
    in.surf = surface_.get();
    in.volume = state_->volume();
    in.camera = camera_;
    in.composite = state_->composite();
    in.windowLow = state_->windowLow();
    in.windowHigh = state_->windowHigh();
    in.mask = state_->mask();

    Tensor32 fb;
    renderSurface(fb, w, h, in);

    QImage img(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y)
        std::memcpy(img.scanLine(y), fb.row(y), std::size_t(w) * 4);
    image_ = std::move(img);
    update();
}

void ViewerItem::paint(QPainter* p)
{
    if (image_.isNull()) { p->fillRect(boundingRect(), Qt::black); return; }
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
    surface_->gen(&coords, nullptr, w, h, camera_.surfacePtr, camera_.scale, {0, 0, camera_.zOff});
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
        camera_.surfacePtr[0] -= float(d.x() * inv);
        camera_.surfacePtr[1] -= float(d.y() * inv);
    }
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
        camera_.zOff += float(steps);      // slice through normal
    } else {
        camera_.zoom(steps);
        if (state_ && state_->volume()) camera_.recalcLevel(state_->volume()->numLevels());
    }
    scheduleRender();
}

}  // namespace vc
