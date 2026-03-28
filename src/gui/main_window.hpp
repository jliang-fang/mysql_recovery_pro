#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    // UI components
    class MainWindowPrivate;
    std::unique_ptr<MainWindowPrivate> d;
};

#endif // MAIN_WINDOW_H
