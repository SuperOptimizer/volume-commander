#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration>
#include <memory>

#include "data/volume.hpp"
#include "data/mask.hpp"
#include "render/surface.hpp"
#include "render/compositing.hpp"
#include "render/renderer.hpp"

namespace vc {

// Shared application state bound to QML and read by all four ViewerItems.
// Holds the open volume, the loaded segment, and the global render knobs.
// Changing a knob bumps renderRev so viewers know to re-render.
class AppState : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString volumeUrl READ volumeUrl NOTIFY volumeChanged)
    Q_PROPERTY(QString segmentDir READ segmentDir NOTIFY segmentChanged)
    Q_PROPERTY(float windowLow READ windowLow WRITE setWindowLow NOTIFY renderChanged)
    Q_PROPERTY(float windowHigh READ windowHigh WRITE setWindowHigh NOTIFY renderChanged)
    Q_PROPERTY(int layersFront READ layersFront WRITE setLayersFront NOTIFY renderChanged)
    Q_PROPERTY(int layersBehind READ layersBehind WRITE setLayersBehind NOTIFY renderChanged)
    Q_PROPERTY(QString compositeMethod READ compositeMethod WRITE setCompositeMethod NOTIFY renderChanged)
    Q_PROPERTY(bool compositeEnabled READ compositeEnabled WRITE setCompositeEnabled NOTIFY renderChanged)
    Q_PROPERTY(bool rakingEnabled READ rakingEnabled WRITE setRakingEnabled NOTIFY renderChanged)
    Q_PROPERTY(bool claheEnabled READ claheEnabled WRITE setClaheEnabled NOTIFY renderChanged)
    Q_PROPERTY(bool maskPaint READ maskPaint WRITE setMaskPaint NOTIFY maskPaintChanged)
    Q_PROPERTY(QString interpolation READ interpolation WRITE setInterpolation NOTIFY renderChanged)
    Q_PROPERTY(float brushRadius READ brushRadius WRITE setBrushRadius NOTIFY maskPaintChanged)

public:
    explicit AppState(QObject* parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE bool openVolume(const QString& url);
    Q_INVOKABLE bool loadSegment(const QString& dir);
    Q_INVOKABLE void saveMask(const QString& dir);

    Volume* volume() const { return volume_.get(); }
    std::shared_ptr<QuadSurface> segment() const { return segment_; }
    MaskVolume* mask() const { return mask_.get(); }

    // Paint into the 3D mask at a world voxel position; erase removes.
    void paintAt(Vec3f world, bool erase);
    const CompositeRenderSettings& composite() const { return composite_; }
    int renderRev() const { return renderRev_; }

    QString volumeUrl() const { return volumeUrl_; }
    QString segmentDir() const { return segmentDir_; }
    float windowLow() const { return windowLow_; }
    float windowHigh() const { return windowHigh_; }
    int layersFront() const { return composite_.layersFront; }
    int layersBehind() const { return composite_.layersBehind; }
    QString compositeMethod() const { return methodName_; }
    bool compositeEnabled() const { return composite_.enabled; }
    bool rakingEnabled() const { return composite_.postRakingEnabled; }
    bool claheEnabled() const { return composite_.postClaheEnabled; }
    bool maskPaint() const { return maskPaint_; }
    float brushRadius() const { return brushRadius_; }
    Sampling sampling() const { return sampling_; }
    QString interpolation() const { return interpName_; }

    void setWindowLow(float v) { windowLow_ = v; bump(); }
    void setWindowHigh(float v) { windowHigh_ = v; bump(); }
    void setLayersFront(int v) { composite_.layersFront = composite_.planeLayersFront = v; bump(); }
    void setLayersBehind(int v) { composite_.layersBehind = composite_.planeLayersBehind = v; bump(); }
    void setCompositeMethod(const QString& m);
    void setCompositeEnabled(bool e) { composite_.enabled = e; composite_.planeEnabled = e; bump(); }
    void setRakingEnabled(bool e) { composite_.postRakingEnabled = e; bump(); }
    void setClaheEnabled(bool e) { composite_.postClaheEnabled = e; bump(); }
    void setMaskPaint(bool e) { if (maskPaint_ != e) { maskPaint_ = e; emit maskPaintChanged(); } }
    void setBrushRadius(float r) { if (brushRadius_ != r) { brushRadius_ = r; emit maskPaintChanged(); } }
    void setInterpolation(const QString& m) {
        interpName_ = m;
        sampling_ = m == "trilinear" ? Sampling::Trilinear : Sampling::Nearest;
        bump();
    }

signals:
    void volumeChanged();
    void segmentChanged();
    void renderChanged();
    void maskPaintChanged();
    void chunkArrived();   // a fetched chunk decoded; viewers should refine

private:
    void bump() { ++renderRev_; emit renderChanged(); }

    std::shared_ptr<Volume> volume_;
    std::shared_ptr<QuadSurface> segment_;
    std::shared_ptr<MaskVolume> mask_;   // 3D binary label mask (lazily created)
    float brushRadius_ = 3.0f;
    Sampling sampling_ = Sampling::Nearest;
    QString interpName_ = "nearest";
    CompositeRenderSettings composite_;
    QString methodName_ = "max";
    QString volumeUrl_, segmentDir_;
    float windowLow_ = 0.0f, windowHigh_ = 255.0f;
    int renderRev_ = 0;
    bool maskPaint_ = false;
};

}  // namespace vc
