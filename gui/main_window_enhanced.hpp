#ifndef MAIN_WINDOW_ENHANCED_H
#define MAIN_WINDOW_ENHANCED_H

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
#include <functional>

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

// ==================== 增强的 GUI 主窗口 ====================
class MainWindowEnhanced : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindowEnhanced(QWidget *parent = nullptr);
    ~MainWindowEnhanced();

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

    // 页导航
    void onTabChanged(int index);
    void onPageSelected(QTreeWidgetItem* item, int column);

    // 日志过滤
    void onLogLevelChanged(int index);

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

    // 日志显示
    void displayLog(const LogEntry& entry);

    // 批量任务显示
    void displayBatchTasks();

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
    QWidget* batchTab_;
    QWidget* logTab_;

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

    // 结果显示
    QTextEdit* resultsTextEdit_;
    QLabel* recordsCountLabel_;
    QTreeWidget* resultsTree_;

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

// ==================== 增强的 GUI 主窗口实现 ====================

inline MainWindowEnhanced::MainWindowEnhanced(QWidget *parent)
    : QMainWindow(parent), state_(RecoveryState::IDLE) {
    initUI();
}

inline MainWindowEnhanced::~MainWindowEnhanced() {
    if (recoveryThread_ && recoveryThread_->isRunning()) {
        recoveryThread_->quit();
        recoveryThread_->wait();
    }
    if (deviceScanThread_ && deviceScanThread_->isRunning()) {
        deviceScanThread_->quit();
        deviceScanThread_->wait();
    }
}

inline void MainWindowEnhanced::initUI() {
    setWindowTitle("MySQL Recovery Pro - Enhanced Edition");
    resize(1200, 800);

    initMenuBar();
    initStatusBar();
    initCentralWidget();
    initDockWidgets();

    // 初始化状态
    setState(RecoveryState::IDLE);
    updateStatus("Ready");
}

inline void MainWindowEnhanced::initMenuBar() {
    menuBar_ = new QMenuBar(this);

    // 文件菜单
    fileMenu_ = menuBar_->addMenu(tr("&File"));
    openAction_ = fileMenu_->addAction(tr("&Open IBD File..."));
    connect(openAction_, &QAction::triggered, this, &MainWindowEnhanced::onActionOpenFile);
    fileMenu_->addSeparator();
    exitAction_ = fileMenu_->addAction(tr("E&xit"));
    connect(exitAction_, &QAction::triggered, this, &MainWindow::close);

    // 工具菜单
    toolsMenu_ = menuBar_->addMenu(tr("&Tools"));
    scanDeviceAction_ = toolsMenu_->addAction(tr("&Scan Device..."));
    connect(scanDeviceAction_, &QAction::triggered, this, &MainWindowEnhanced::onActionScanDevice);

    // 批量恢复菜单
    batchMenu_ = menuBar_->addMenu(tr("&Batch"));
    batchRecoveryAction_ = batchMenu_->addAction(tr("&Start Batch Recovery"));
    connect(batchRecoveryAction_, &QAction::triggered, this, &MainWindowEnhanced::onStartBatchRecovery);
    clearBatchAction_ = batchMenu_->addAction(tr("&Clear Tasks"));
    connect(clearBatchAction_, &QAction::triggered, this, &MainWindowEnhanced::onClearBatchTasks);

    // 帮助菜单
    helpMenu_ = menuBar_->addMenu(tr("&Help"));
    aboutAction_ = helpMenu_->addAction(tr("&About"));
    connect(aboutAction_, &QAction::triggered, this, &MainWindowEnhanced::onActionAbout);
    helpAction_ = helpMenu_->addAction(tr("&Help"));
    connect(helpAction_, &QAction::triggered, this, &MainWindowEnhanced::onActionHelp);

    setMenuBar(menuBar_);
}

inline void MainWindowEnhanced::initStatusBar() {
    statusBar_ = new QStatusBar(this);
    setStatusBar(statusBar_);

    statusLabel_ = new QLabel("Ready");
    statusBar_->addWidget(statusLabel_);

    progressBar_ = new QProgressBar();
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    statusBar_->addPermanentWidget(progressBar_);

    recoveryModeLabel_ = new QLabel("");
    statusBar_->addPermanentWidget(recoveryModeLabel_);

    recordsCountLabel_ = new QLabel("Records: 0");
    statusBar_->addPermanentWidget(recordsCountLabel_);
}

inline void MainWindowEnhanced::initCentralWidget() {
    tabWidget_ = new QTabWidget(this);

    // 文件信息标签
    fileInfoTab_ = new QWidget();
    QVBoxLayout* fileInfoLayout = new QVBoxLayout(fileInfoTab_);

    filePathLabel_ = new QLabel("File: ");
    fileSizeLabel_ = new QLabel("Size: ");
    pageCountLabel_ = new QLabel("Pages: ");
    pageSizeMode_ = new QLabel("Page Size: ");
    tableIdLabel_ = new QLabel("Table ID: ");
    rowFormatLabel_ = new QLabel("Row Format: ");

    fileInfoLayout->addWidget(filePathLabel_);
    fileInfoLayout->addWidget(fileSizeLabel_);
    fileInfoLayout->addWidget(pageCountLabel_);
    fileInfoLayout->addWidget(pageSizeMode_);
    fileInfoLayout->addWidget(tableIdLabel_);
    fileInfoLayout->addWidget(rowFormatLabel_);
    fileInfoLayout->addStretch();

    // 恢复标签
    recoveryTab_ = new QWidget();
    QVBoxLayout* recoveryLayout = new QVBoxLayout(recoveryTab_);

    QGroupBox* recoveryModeGroup = new QGroupBox("Recovery Mode");
    QVBoxLayout* modeLayout = new QVBoxLayout(recoveryModeGroup);
    recoveryModeCombo_ = new QComboBox();
    recoveryModeCombo_->addItem("Default Recovery");
    recoveryModeCombo_->addItem("Truncate Recovery");
    recoveryModeCombo_->addItem("rm -rf Recovery");
    recoveryModeCombo_->addItem("Header Repair");
    modeLayout->addWidget(recoveryModeCombo_);

    QGroupBox* outputGroup = new QGroupBox("Output Settings");
    QVBoxLayout* outputLayout = new QVBoxLayout(outputGroup);
    outputPathEdit_ = new QLineEdit("/tmp/recovered");
    browseButton_ = new QPushButton("Browse...");
    QHBoxLayout* outputPathLayout = new QHBoxLayout();
    outputPathLayout->addWidget(outputPathEdit_);
    outputPathLayout->addWidget(browseButton_);
    outputLayout->addLayout(outputPathLayout);

    outputFormatCombo_ = new QComboBox();
    outputFormatCombo_->addItem("CSV", "csv");
    outputFormatCombo_->addItem("SQL", "sql");
    outputFormatCombo_->addItem("JSON", "json");
    outputFormatCombo_->addItem("XML", "xml");
    outputFormatCombo_->addItem("YAML", "yaml");
    outputFormatCombo_->addItem("Avro", "avro");
    outputFormatCombo_->addItem("Text", "text");
    outputFormatCombo_->addItem("IBD (Repaired)", "ibd");
    outputLayout->addWidget(new QLabel("Format:"));
    outputLayout->addWidget(outputFormatCombo_);

    QGroupBox* optionsGroup = new QGroupBox("Options");
    QVBoxLayout* optionsLayout = new QVBoxLayout(optionsGroup);
    blobRecoveryCheck_ = new QCheckBox("Enable BLOB Recovery");
    headerRepairCheck_ = new QCheckBox("Repair Corrupted Headers");
    verboseCheck_ = new QCheckBox("Verbose Output");
    optionsLayout->addWidget(blobRecoveryCheck_);
    optionsLayout->addWidget(headerRepairCheck_);
    optionsLayout->addWidget(verboseCheck_);

    QGroupBox* pageSettingsGroup = new QGroupBox("Page Settings");
    QVBoxLayout* pageLayout = new QVBoxLayout(pageSettingsGroup);
    pageSizeSpin_ = new QSpinBox();
    pageSizeSpin_->setRange(4096, 65536);
    pageSizeSpin_->setSingleStep(4096);
    pageSizeSpin_->setValue(16384);
    tableIdSpin_ = new QSpinBox();
    tableIdSpin_->setRange(0, 0xFFFFFFFF);
    pageLayout->addWidget(new QLabel("Page Size:"));
    pageLayout->addWidget(pageSizeSpin_);
    pageLayout->addWidget(new QLabel("Table ID:"));
    pageLayout->addWidget(tableIdSpin_);

    QGroupBox* encryptionSettingsGroup = new QGroupBox("TDE Encryption Settings");
    QVBoxLayout* encryptionLayout = new QVBoxLayout(encryptionSettingsGroup);
    QLineEdit* encryptionKeyEdit = new QLineEdit();
    QLineEdit* tableKeyEdit = new QLineEdit();
    QLineEdit* tablespaceIdEdit = new QLineEdit();
    QCheckBox* detectEncryptionCheck = new QCheckBox("Auto-detect encryption");
    QCheckBox* tryNoKeyCheck = new QCheckBox("Try recovery without key");
    encryptionLayout->addWidget(new QLabel("Primary Key (hex):"));
    encryptionLayout->addWidget(encryptionKeyEdit);
    encryptionLayout->addWidget(new QLabel("Table Key (hex):"));
    encryptionLayout->addWidget(tableKeyEdit);
    encryptionLayout->addWidget(new QLabel("Tablespace ID:"));
    encryptionLayout->addWidget(tablespaceIdEdit);
    encryptionLayout->addWidget(detectEncryptionCheck);
    encryptionLayout->addWidget(tryNoKeyCheck);

    QGroupBox* versionSettingsGroup = new QGroupBox("MySQL Version Compatibility");
    QVBoxLayout* versionLayout = new QVBoxLayout(versionSettingsGroup);
    QSpinBox* mysqlVersionSpin = new QSpinBox();
    mysqlVersionSpin->setRange(0, 90);
    mysqlVersionSpin->setSingleStep(1);
    mysqlVersionSpin->setValue(80);
    QCheckBox* detectVersionCheck = new QCheckBox("Auto-detect version");
    QComboBox* rowFormatCombo = new QComboBox();
    rowFormatCombo->addItem("Dynamic (default)", "dynamic");
    rowFormatCombo->addItem("Compact", "compact");
    rowFormatCombo->addItem("Redundant", "redundant");
    rowFormatCombo->addItem("Compressed", "compressed");
    versionLayout->addWidget(new QLabel("MySQL Version:"));
    versionLayout->addWidget(mysqlVersionSpin);
    versionLayout->addWidget(detectVersionCheck);
    versionLayout->addWidget(new QLabel("Row Format:"));
    versionLayout->addWidget(rowFormatCombo);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    startButton_ = new QPushButton("Start Recovery");
    cancelButton_ = new QPushButton("Cancel");
    pauseButton_ = new QPushButton("Pause");
    resumeButton_ = new QPushButton("Resume");
    pauseButton_->setEnabled(false);
    resumeButton_->setEnabled(false);
    buttonLayout->addWidget(startButton_);
    buttonLayout->addWidget(cancelButton_);
    buttonLayout->addWidget(pauseButton_);
    buttonLayout->addWidget(resumeButton_);

    recoveryLayout->addWidget(recoveryModeGroup);
    recoveryLayout->addWidget(outputGroup);
    recoveryLayout->addWidget(optionsGroup);
    recoveryLayout->addWidget(pageSettingsGroup);
    recoveryLayout->addWidget(encryptionSettingsGroup);
    recoveryLayout->addWidget(versionSettingsGroup);
    recoveryLayout->addLayout(buttonLayout);
    recoveryLayout->addStretch();

    // 结果标签
    resultsTab_ = new QWidget();
    QVBoxLayout* resultsLayout = new QVBoxLayout(resultsTab_);

    resultsTree_ = new QTreeWidget();
    resultsTree_->setColumnCount(10);
    resultsTree_->setHeaderLabels({"Offset", "Field1", "Field2", "Field3", "Field4",
                                   "Field5", "Field6", "Field7", "Field8", "Field9"});
    resultsLayout->addWidget(resultsTree_);

    recordsCountLabel_ = new QLabel("Records: 0");
    resultsLayout->addWidget(recordsCountLabel_);

    // 设备扫描标签
    deviceScanTab_ = new QWidget();
    QVBoxLayout* deviceScanLayout = new QVBoxLayout(deviceScanTab_);

    devicePathEdit_ = new QLineEdit("/dev/sdX");
    scanDeviceButton_ = new QPushButton("Scan Device");
    cancelScanButton_ = new QPushButton("Cancel");
    QHBoxLayout* deviceLayout = new QHBoxLayout();
    deviceLayout->addWidget(new QLabel("Device Path:"));
    deviceLayout->addWidget(devicePathEdit_);
    deviceLayout->addWidget(scanDeviceButton_);
    deviceLayout->addWidget(cancelScanButton_);
    deviceScanLayout->addLayout(deviceLayout);

    scanResultsEdit_ = new QPlainTextEdit();
    scanResultsEdit_->setReadOnly(true);
    deviceScanLayout->addWidget(scanResultsEdit_);

    // 批量恢复标签
    batchTab_ = new QWidget();
    QVBoxLayout* batchLayout = new QVBoxLayout(batchTab_);

    batchTaskList_ = new QListWidget();
    batchLayout->addWidget(batchTaskList_);

    QHBoxLayout* batchButtonLayout = new QHBoxLayout();
    addBatchButton_ = new QPushButton("Add Task");
    removeBatchButton_ = new QPushButton("Remove Selected");
    startBatchButton_ = new QPushButton("Start Batch");
    clearBatchButton_ = new QPushButton("Clear All");
    batchButtonLayout->addWidget(addBatchButton_);
    batchButtonLayout->addWidget(removeBatchButton_);
    batchButtonLayout->addWidget(startBatchButton_);
    batchButtonLayout->addWidget(clearBatchButton_);
    batchLayout->addLayout(batchButtonLayout);

    batchProgressBar_ = new QProgressBar();
    batchProgressBar_->setRange(0, 100);
    batchProgressBar_->setValue(0);
    batchLayout->addWidget(batchProgressBar_);

    // 日志标签
    logTab_ = new QWidget();
    QVBoxLayout* logLayout = new QVBoxLayout(logTab_);

    QHBoxLayout* logFilterLayout = new QHBoxLayout();
    logLevelCombo_ = new QComboBox();
    logLevelCombo_->addItem("All");
    logLevelCombo_->addItem("INFO");
    logLevelCombo_->addItem("WARN");
    logLevelCombo_->addItem("ERROR");
    logLevelCombo_->addItem("DEBUG");
    clearLogButton_ = new QPushButton("Clear");
    saveLogButton_ = new QPushButton("Save");
    logFilterLayout->addWidget(new QLabel("Filter:"));
    logFilterLayout->addWidget(logLevelCombo_);
    logFilterLayout->addStretch();
    logFilterLayout->addWidget(clearLogButton_);
    logFilterLayout->addWidget(saveLogButton_);
    logLayout->addLayout(logFilterLayout);

    logTextEdit_ = new QPlainTextEdit();
    logTextEdit_->setReadOnly(true);
    logTextEdit_->setFontFamily("Monospace");
    logLayout->addWidget(logTextEdit_);

    tabWidget_->addTab(fileInfoTab_, "File Info");
    tabWidget_->addTab(recoveryTab_, "Recovery");
    tabWidget_->addTab(resultsTab_, "Results");
    tabWidget_->addTab(deviceScanTab_, "Device Scan");
    tabWidget_->addTab(batchTab_, "Batch");
    tabWidget_->addTab(logTab_, "Log");

    setCentralWidget(tabWidget_);

    // 连接信号
    connect(browseButton_, &QPushButton::clicked, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory");
        if (!dir.isEmpty()) {
            outputPathEdit_->setText(dir);
        }
    });

    connect(startButton_, &QPushButton::clicked, this, &MainWindowEnhanced::onStartRecovery);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindowEnhanced::onCancelRecovery);
    connect(pauseButton_, &QPushButton::clicked, this, &MainWindowEnhanced::onPauseRecovery);
    connect(resumeButton_, &QPushButton::clicked, this, &MainWindowEnhanced::onResumeRecovery);

    connect(addBatchButton_, &QPushButton::clicked, [this]() {
        QString filePath = QFileDialog::getOpenFileName(this, "Select IBD File");
        if (!filePath.isEmpty()) {
            BatchRecoveryTask task;
            task.file_path = filePath.toStdString();
            task.output_dir = "/tmp/recovered";
            task.format = "csv";
            task.recovery_mode = "default";
            task.blob_recovery = false;
            task.header_repair = false;
            task.page_size = 16384;
            batchTasks_.push_back(task);
            displayBatchTasks();
        }
    });

    connect(removeBatchButton_, &QPushButton::clicked, [this]() {
        int row = batchTaskList_->currentRow();
        if (row >= 0 && row < static_cast<int>(batchTasks_.size())) {
            batchTasks_.erase(batchTasks_.begin() + row);
            displayBatchTasks();
        }
    });

    connect(startBatchButton_, &QPushButton::clicked, this, &MainWindowEnhanced::onStartBatchRecovery);
    connect(clearBatchButton_, &QPushButton::clicked, this, &MainWindowEnhanced::onClearBatchTasks);

    connect(clearLogButton_, &QPushButton::clicked, this, &MainWindowEnhanced::clearLog);
    connect(saveLogButton_, &QPushButton::clicked, [this]() {
        QString filePath = QFileDialog::getSaveFileName(this, "Save Log", "recovery.log");
        if (!filePath.isEmpty()) {
            std::ofstream file(filePath.toStdString());
            if (file) {
                for (const auto& entry : logEntries_) {
                    file << entry.to_string().toStdString() << "\n";
                }
                file.close();
            }
        }
    });

    connect(logLevelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int index) { (void)index; /* TODO: implement filtering */ });

    connect(tabWidget_, &QTabWidget::currentChanged, this, &MainWindowEnhanced::onTabChanged);
}

inline void MainWindowEnhanced::initDockWidgets() {
    // 状态信息面板
    QDockWidget* statsDock = new QDockWidget("Recovery Statistics", this);
    QWidget* statsWidget = new QWidget();
    QVBoxLayout* statsLayout = new QVBoxLayout(statsWidget);

    QLabel* pagesLabel = new QLabel("Pages Scanned: 0");
    QLabel* validLabel = new QLabel("Valid Pages: 0");
    QLabel* recordsLabel = new QLabel("Records Recovered: 0");
    QLabel* timeLabel = new QLabel("Elapsed Time: 0.0s");

    statsLayout->addWidget(pagesLabel);
    statsLayout->addWidget(validLabel);
    statsLayout->addWidget(recordsLabel);
    statsLayout->addWidget(timeLabel);
    statsLayout->addStretch();

    statsDock->setWidget(statsWidget);
    addDockWidget(Qt::RightDockWidgetArea, statsDock);
}

inline void MainWindowEnhanced::addLog(const QString& message, const QString& level,
                                        const QString& source) {
    LogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.level = level;
    entry.message = message;
    entry.source = source;

    logEntries_.push_back(entry);
    displayLog(entry);
}

inline void MainWindowEnhanced::displayLog(const LogEntry& entry) {
    logTextEdit_->appendPlainText(entry.to_string());
}

inline void MainWindowEnhanced::clearLog() {
    logTextEdit_->clear();
    logEntries_.clear();
}

inline void MainWindowEnhanced::setState(RecoveryState state) {
    state_ = state;

    switch (state) {
        case RecoveryState::IDLE:
            startButton_->setEnabled(true);
            cancelButton_->setEnabled(false);
            pauseButton_->setEnabled(false);
            resumeButton_->setEnabled(false);
            updateStatus("Ready");
            break;
        case RecoveryState::SCANNING:
            startButton_->setEnabled(false);
            cancelButton_->setEnabled(true);
            pauseButton_->setEnabled(false);
            resumeButton_->setEnabled(false);
            updateStatus("Scanning...");
            break;
        case RecoveryState::RECOVERING:
            startButton_->setEnabled(false);
            cancelButton_->setEnabled(true);
            pauseButton_->setEnabled(true);
            resumeButton_->setEnabled(false);
            updateStatus("Recovering...");
            break;
        case RecoveryState::PAUSED:
            startButton_->setEnabled(false);
            cancelButton_->setEnabled(true);
            pauseButton_->setEnabled(false);
            resumeButton_->setEnabled(true);
            updateStatus("Paused");
            break;
        case RecoveryState::COMPLETED:
            startButton_->setEnabled(true);
            cancelButton_->setEnabled(false);
            pauseButton_->setEnabled(false);
            resumeButton_->setEnabled(false);
            updateStatus("Completed");
            break;
        case RecoveryState::CANCELLED:
            startButton_->setEnabled(true);
            cancelButton_->setEnabled(false);
            pauseButton_->setEnabled(false);
            resumeButton_->setEnabled(false);
            updateStatus("Cancelled");
            break;
    }
}

inline void MainWindowEnhanced::updateStatus(const QString& message) {
    statusLabel_->setText(message);
}

inline void MainWindowEnhanced::updateProgress(int value, int max) {
    progressBar_->setRange(0, max);
    progressBar_->setValue(value);
}

inline void MainWindowEnhanced::updateRecoveryStats(const QString& stats) {
    recoveryModeLabel_->setText(stats);
}

inline void MainWindowEnhanced::displayBatchTasks() {
    batchTaskList_->clear();
    for (const auto& task : batchTasks_) {
        batchTaskList_->addItem(task.toQString());
    }
}

} // namespace gui
} // namespace dbrecovery

#endif // MAIN_WINDOW_ENHANCED_H
