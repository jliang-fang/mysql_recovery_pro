#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QMenuBar>
#include <QAction>
#include <QFileDialog>
#include <QTextEdit>
#include <QStatusBar>
#include <QProgressBar>
#include <QTabWidget>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>
#include <QFuture>
#include <QtConcurrent>

#include <string>
#include <vector>
#include <map>

namespace dbrecovery {
namespace gui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // 菜单动作
    void onActionOpenFile();
    void onActionExit();
    void onActionAbout();
    void onActionHelp();

    // 恢复操作
    void onStartRecovery();
    void onCancelRecovery();

    // 页导航
    void onTabChanged(int index);

private:
    // 初始化 UI
    void initUI();
    void initMenuBar();
    void initStatusBar();
    void initCentralWidget();

    // 恢复线程
    void startRecoveryThread();

    // 更新状态
    void updateStatus(const QString& message);
    void updateProgress(int value, int max);

    // 文件信息显示
    void displayFileInfo(const QString& filePath);

    // 恢复结果展示
    void displayRecoveryResults(const std::vector<std::map<std::string, std::string>>& records);

private:
    // UI Components
    QMenuBar* menuBar_;
    QMenu* fileMenu_;
    QMenu* helpMenu_;
    QAction* openAction_;
    QAction* exitAction_;
    QAction* aboutAction_;
    QAction* helpAction_;

    QStatusBar* statusBar_;
    QLabel* statusLabel_;
    QProgressBar* progressBar_;

    QTabWidget* tabWidget_;
    QWidget* fileInfoTab_;
    QWidget* recoveryTab_;
    QWidget* resultsTab_;

    // 文件信息标签
    QLabel* filePathLabel_;
    QLabel* fileSizeLabel_;
    QLabel* pageCountLabel_;
    QLabel* pageSizeMode_;

    // 恢复设置
    QComboBox* recoveryModeCombo_;
    QComboBox* outputFormatCombo_;
    QLineEdit* outputPathEdit_;
    QPushButton* browseButton_;
    QCheckBox* blobRecoveryCheck_;
    QCheckBox* headerRepairCheck_;
    QPushButton* startButton_;
    QPushButton* cancelButton_;

    // 结果显示
    QTextEdit* resultsTextEdit_;
    QLabel* recordsCountLabel_;

    // 内部状态
    std::string currentFilePath_;
    bool isRecovering_;
    QThread* recoveryThread_;
    QProgressDialog* progressDialog_;
};

} // namespace gui
} // namespace dbrecovery

#endif // MAIN_WINDOW_H
