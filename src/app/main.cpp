#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QThreadPool>
#include <cstdlib>
#include <omp.h>

#include "app/state.hpp"
#include "app/viewer_item.hpp"

int main(int argc, char** argv)
{
    // Bound parallelism: the Volume IO pool (6 jthreads) does S3 fetch + c3d
    // decode; render dispatch uses a small QThreadPool. c3d's OpenMP would
    // otherwise spawn nested threads per concurrent decode → hundreds of
    // threads. Pin OpenMP to 1 so each IO worker decodes single-threaded;
    // parallelism comes from the pool's width, not nested OMP.
    omp_set_num_threads(1);
    if (!getenv("OMP_NUM_THREADS")) setenv("OMP_NUM_THREADS", "1", 1);

    QGuiApplication app(argc, argv);
    app.setOrganizationName("ScrollPrize");
    app.setOrganizationDomain("scrollprize.org");
    app.setApplicationName("volume-commander");

    QThreadPool::globalInstance()->setMaxThreadCount(4);   // cap render workers (4 viewers)

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
