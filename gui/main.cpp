#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "MainWindow.hpp"

#include <QApplication>
#include <QFont>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("AtomicRipper");
    app.setApplicationVersion("0.7.0");
    app.setOrganizationName("AtomicRipper");

    // Use a slightly larger default font on high-DPI screens
    QFont font = app.font();
    font.setPointSize(10);
    app.setFont(font);

    atomicripper::gui::MainWindow win;
    win.show();

    return app.exec();
}
