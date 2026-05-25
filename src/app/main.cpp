#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "app/state.hpp"
#include "app/viewer_item.hpp"

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName("ScrollPrize");
    app.setOrganizationDomain("scrollprize.org");
    app.setApplicationName("volume-commander");

    QQmlApplicationEngine engine;
    engine.loadFromModule("VolumeCommander", "Main");
    if (engine.rootObjects().isEmpty()) return -1;

    // CLI convenience: `volume_commander <volume-url> [segment-dir]`
    if (argc > 1) {
        auto* root = engine.rootObjects().first();
        if (auto* st = root->findChild<vc::AppState*>()) {
            st->openVolume(QString::fromUtf8(argv[1]));
            if (argc > 2) st->loadSegment(QString::fromUtf8(argv[2]));
        }
    }
    return app.exec();
}
