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
#include <QSpinBox>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QTreeWidget>
#include <QSplitter>
#include <QTimer>
#include <QDateTime>
#include <QFrame>
#include <QScrollArea>
#include <QDockWidget>

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>

namespace dbrecovery {
namespace gui {

// ==================== 增强的 GUI 状态 ====================
enum class RecoveryState {
    IDLE,
    SCANNING,
    RECOVERING,
    PAUSED,
    COMPLETED,
    CANCELLED
};

// ==================== 实时日志条目 ====================
struct LogEntry {
    QDateTime timestamp;
    QString level;  // INFO, WARN, ERROR, DEBUG
    QString message;
    QString source;

    QString to_string() const {
        return QString("[%1] %2: %3")
            .arg(timestamp.toString("HH:mm:ss.zzz"))
            .arg(level)
            .arg(message);
    }
};

// ==================== 批量恢复任务 ====================
struct BatchRecoveryTask {
    std::string file_path;
    std::string output_dir;
    std::string format;
    std::string recovery_mode;
    bool blob_recovery;
    bool header_repair;
    uint32_t page_size;

    QString toQString() const {
        return QString::fromStdString(file_path);
    }
};

// ==================== GUI 主窗口 ====================
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    /**
     * @brief 添加日志条目
     */
    void addLog(const QString& message, const QString& level = "INFO",
                const QString& source = "Core");

    /**
     * @brief 批量添加恢复任务
     */
    void addBatchTask(const BatchRecoveryTask& task);

    /**
     * @brief 清空日志
     */
    void clearLog();

    /**
     * @brief 获取当前状态
     */
    RecoveryState getState() const { return state_; }

    /**
     * @brief 设置状态
     */
    void setState(RecoveryState state);

    /**
     * @brief 扫描目录
     */
    void scanDirectory(const QString& dirPath);

    /**
     * @brief 搜索表
     */
    void searchTable(const QString& tableName);

private slots:
    // 菜单动作
    void onActionOpenFile();
    void onActionExit();
    void onActionAbout();
    void onActionHelp();

    // 恢复操作
    void onStartRecovery();
    void onCancelRecovery();
    void onPauseRecovery();
    void onResumeRecovery();

    // 批量恢复
    void onStartBatchRecovery();
    void onClearBatchTasks();

    // 设备扫描
    void onActionScanDevice();
    void onStartDeviceScan();
    void onCancelDeviceScan();

    // 目录扫描
    void onActionScanDirectory();
    void onStartDirectoryScan();
    void onCancelDirectoryScan();

    // 表搜索
    void onActionSearchTable();
    void onStartTableSearch();
    void onCancelTableSearch();

    // 页导航
    void onTabChanged(int index);
    void onPageSelected(QTreeWidgetItem* item, int column);

    // 日志过滤
    void onLogLevelChanged(int index);

    // 导出结果
    void onExportResults();
    void onSaveResults();

private:
    // 初始化 UI
    void initUI();
    void initMenuBar();
    void initStatusBar();
    void initCentralWidget();
    void initDockWidgets();

    // 恢复线程
    void startRecoveryThread();
    void startBatchRecoveryThread();

    // 更新状态
    void updateStatus(const QString& message);
    void updateProgress(int value, int max);
    void updateRecoveryStats(const QString& stats);

    // 文件信息显示
    void displayFileInfo(const QString& filePath);

    // 恢复结果展示
    void displayRecoveryResults(const std::vector<std::map<std::string, std::string>>& records);
    void displayRecoveryResults(const QString& resultPath, int recordCount);

    // 日志显示
    void displayLog(const LogEntry& entry);

    // 批量任务显示
    void displayBatchTasks();

    // 目录扫描结果显示
    void displayScanResults(const std::vector<std::string>& files);

    // 表搜索结果显示
    void displaySearchResults(const std::vector<std::map<std::string, std::string>>& results);

private:
    // UI Components
    QMenuBar* menuBar_;
    QMenu* fileMenu_;
    QMenu* helpMenu_;
    QMenu* toolsMenu_;
    QMenu* batchMenu_;

    QAction* openAction_;
    QAction* exitAction_;
    QAction* aboutAction_;
    QAction* helpAction_;
    QAction* scanDeviceAction_;
    QAction* scanDirectoryAction_;
    QAction* searchTableAction_;
    QAction* batchRecoveryAction_;
    QAction* clearBatchAction_;

    QStatusBar* statusBar_;
    QLabel* statusLabel_;
    QProgressBar* progressBar_;
    QLabel* recoveryModeLabel_;
    QLabel* recordsCountLabel_;

    QTabWidget* tabWidget_;
    QWidget* fileInfoTab_;
    QWidget* recoveryTab_;
    QWidget* resultsTab_;
    QWidget* deviceScanTab_;
    QWidget* directoryScanTab_;
    QWidget* batchTab_;
    QWidget* logTab_;
    QWidget* searchTab_;

    // 文件信息标签
    QLabel* filePathLabel_;
    QLabel* fileSizeLabel_;
    QLabel* pageCountLabel_;
    QLabel* pageSizeMode_;
    QLabel* tableIdLabel_;
    QLabel* rowFormatLabel_;

    // 恢复设置
    QComboBox* recoveryModeCombo_;
    QComboBox* outputFormatCombo_;
    QLineEdit* outputPathEdit_;
    QPushButton* browseButton_;
    QCheckBox* blobRecoveryCheck_;
    QCheckBox* headerRepairCheck_;
    QCheckBox* verboseCheck_;
    QSpinBox* pageSizeSpin_;
    QSpinBox* tableIdSpin_;
    QLineEdit* tableNameEdit_;
    QLineEdit* databaseNameEdit_;
    QLineEdit* userNameEdit_;
    QPushButton* startButton_;
    QPushButton* cancelButton_;
    QPushButton* pauseButton_;
    QPushButton* resumeButton_;

    // TDE 加密设置
    QLineEdit* encryptionKeyEdit_;
    QLineEdit* tableKeyEdit_;
    QLineEdit* tablespaceIdEdit_;
    QCheckBox* detectEncryptionCheck_;
    QCheckBox* tryNoKeyCheck_;

    // MySQL 版本设置
    QSpinBox* mysqlVersionSpin_;
    QCheckBox* detectVersionCheck_;
    QComboBox* rowFormatCombo_;

    // 设备扫描设置
    QLineEdit* devicePathEdit_;
    QPushButton* scanDeviceButton_;
    QPlainTextEdit* scanResultsEdit_;
    QPushButton* cancelScanButton_;

    // 目录扫描设置
    QLineEdit* directoryPathEdit_;
    QPushButton* scanDirectoryButton_;
    QSpinBox* scanDepthSpin_;
    QTreeWidget* scanResultsTree_;
    QPushButton* cancelDirectoryScanButton_;

    // 表搜索设置
    QLineEdit* searchTableNameEdit_;
    QPushButton* searchTableButton_;
    QTreeWidget* searchResultsTree_;
    QPushButton* cancelSearchButton_;

    // 结果显示
    QTextEdit* resultsTextEdit_;
    QLabel* recordsCountLabel_;
    QTreeWidget* resultsTree_;
    QPushButton* exportButton_;
    QPushButton* saveButton_;

    // 日志显示
    QPlainTextEdit* logTextEdit_;
    QComboBox* logLevelCombo_;
    QPushButton* clearLogButton_;
    QPushButton* saveLogButton_;

    // 批量恢复设置
    QListWidget* batchTaskList_;
    QPushButton* addBatchButton_;
    QPushButton* removeBatchButton_;
    QPushButton* startBatchButton_;
    QPushButton* clearBatchButton_;
    QProgressBar* batchProgressBar_;

    // 目录扫描状态
    bool isScanningDirectory_;
    QThread* directoryScanThread_;
    std::atomic<bool> shouldCancelDirectoryScan_;
    std::vector<std::string> scanResults_;

    // 表搜索状态
    bool isSearching_;
    QThread* searchThread_;
    std::atomic<bool> shouldCancelSearch_;

    // 内部状态
    std::string currentFilePath_;
    RecoveryState state_;
    QThread* recoveryThread_;
    QProgressDialog* progressDialog_;

    // 设备扫描状态
    bool isScanningDevice_;
    QThread* deviceScanThread_;
    std::atomic<bool> shouldCancelScan_;

    // 日志
    std::vector<LogEntry> logEntries_;
    QTimer* logUpdateTimer_;

    // 批量恢复
    std::vector<BatchRecoveryTask> batchTasks_;
    int completedBatchTasks_;
    int totalBatchTasks_;

    // 恢复统计
    uint64_t totalPagesScanned_;
    uint64_t validPages_;
    uint64_t recordsRecovered_;
    double elapsedSeconds_;
};

} // namespace gui
} // namespace dbrecovery

#endif // MAIN_WINDOW_H
