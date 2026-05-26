#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QThreadPool>
#include <cstring>

#include "app/state.hpp"
#include "app/viewer_item.hpp"

int main(int argc, char** argv)
{
    // c3d is built without OpenMP (see top-level CMake): each decode is
    // single-threaded; parallelism comes from the Volume IO pool. Render
    // dispatch uses a small QThreadPool.
    QGuiApplication app(argc, argv);
    app.setOrganizationName("ScrollPrize");
    app.setOrganizationDomain("scrollprize.org");
    app.setApplicationName("volume-commander");

    QThreadPool::globalInstance()->setMaxThreadCount(4);   // cap render workers (4 viewers)

    QQmlApplicationEngine engine;
    engine.loadFromModule("VolumeCommander", "Main");
    if (engine.rootObjects().isEmpty()) return -1;

    // CLI: `volume_commander <volume-url> [segment-dir] [overlay-url]`
    if (argc > 1) {
        auto* root = engine.rootObjects().first();
        if (auto* st = root->findChild<vc::AppState*>()) {
            st->openVolume(QString::fromUtf8(argv[1]));
            if (argc > 2 && std::strlen(argv[2])) st->loadSegment(QString::fromUtf8(argv[2]));
            if (argc > 3 && std::strlen(argv[3])) st->openOverlay(QString::fromUtf8(argv[3]));
        }
    }
    return app.exec();
}
