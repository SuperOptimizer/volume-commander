#include <QApplication>
#include <QMainWindow>
#include <QLabel>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QMainWindow win;
    win.setWindowTitle("volume-commander");
    win.resize(1280, 800);
    win.setCentralWidget(new QLabel("volume-commander — 3D ink labeler", &win));
    win.show();
    return app.exec();
}
