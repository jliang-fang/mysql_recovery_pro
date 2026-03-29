#include <QApplication>
#include "main_window.hpp"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    dbrecovery::gui::MainWindow window;
    window.show();

    return app.exec();
}
