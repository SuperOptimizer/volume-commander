#pragma once

#include <QQuickPaintedItem>
#include <QImage>
#include <QPointF>
#include <atomic>
#include <memory>

#include "render/renderer.hpp"
#include "app/state.hpp"

namespace vc {

// One of the four viewers. Owns a Camera + Surface; renders the volume to an
// ARGB framebuffer on a worker thread and paints it. Pan/zoom via mouse.
// Surface/volume/settings come from the shared AppState bound in QML.
class ViewerItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(vc::AppState* state READ state WRITE setState NOTIFY stateChanged)
    Q_PROPERTY(QString view READ view WRITE setView NOTIFY viewChanged)  // "xy","xz","yz","seg"

public:
    explicit ViewerItem(QQuickItem* parent = nullptr);
    ~ViewerItem() override;

    void paint(QPainter* p) override;

    AppState* state() const { return state_; }
    void setState(AppState* s);
    QString view() const { return viewName_; }
    void setView(const QString& v);

    Q_INVOKABLE void requestRender();

signals:
    void stateChanged();
    void viewChanged();

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void geometryChange(const QRectF&, const QRectF&) override;

private:
    void rebuildSurface();
    void renderNow();          // synchronous render into image_ (worker-dispatched)
    void scheduleRender();
    bool worldAt(QPointF pos, Vec3f& out) const;  // screen px -> world voxel

    AppState* state_ = nullptr;
    QString viewName_ = "xy";
    std::shared_ptr<Surface> surface_;
    Camera camera_;
    QImage image_;
    std::atomic<bool> busy_{false};
    bool pending_ = false;
    QPointF lastPan_;
    bool panning_ = false;
    bool painting_ = false;
    bool eraseStroke_ = false;
};

}  // namespace vc
