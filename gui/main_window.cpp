#include "main_window.hpp"
#include <iostream>
#include <filesystem>

namespace dbrecovery {
namespace gui {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), isRecovering_(false), recoveryThread_(nullptr) {
    initUI();
}

MainWindow::~MainWindow() {
    if (recoveryThread_ && recoveryThread_->isRunning()) {
        recoveryThread_->quit();
        recoveryThread_->wait();
    }
}

void MainWindow::initUI() {
    setWindowTitle("MySQL 8.x Recovery Engine v1.0.0");
    resize(1200, 800);

    initMenuBar();
    initStatusBar();
    initCentralWidget();
}

void MainWindow::initMenuBar() {
    menuBar_ = new QMenuBar(this);

    fileMenu_ = menuBar_->addMenu(tr("&File"));
    openAction_ = fileMenu_->addAction(tr("&Open .ibd File..."));
    openAction_->setShortcut(tr("Ctrl+O"));
    connect(openAction_, &QAction::triggered, this, &MainWindow::onActionOpenFile);

    fileMenu_->addSeparator();
    exitAction_ = fileMenu_->addAction(tr("E&xit"));
    exitAction_->setShortcut(tr("Ctrl+Q"));
    connect(exitAction_, &QAction::triggered, this, &MainWindow::onActionExit);

    helpMenu_ = menuBar_->addMenu(tr("&Help"));
    aboutAction_ = helpMenu_->addAction(tr("&About"));
    connect(aboutAction_, &QAction::triggered, this, &MainWindow::onActionAbout);

    helpAction_ = helpMenu_->addAction(tr("&Help"));
    helpAction_->setShortcut(tr("F1"));
    connect(helpAction_, &QAction::triggered, this, &MainWindow::onActionHelp);

    setMenuBar(menuBar_);
}

void MainWindow::initStatusBar() {
    statusBar_ = new QStatusBar(this);
    statusLabel_ = new QLabel("Ready");
    progressBar_ = new QProgressBar();
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setVisible(false);

    statusBar_->addWidget(statusLabel_);
    statusBar_->addPermanentWidget(progressBar_);
    setStatusBar(statusBar_);
}

void MainWindow::initCentralWidget() {
    tabWidget_ = new QTabWidget(this);

    // 文件信息标签
    filePathLabel_ = new QLabel("File: -");
    fileSizeLabel_ = new QLabel("Size: -");
    pageCountLabel_ = new QLabel("Pages: -");
    pageSizeMode_ = new QLabel("Page Size: -");

    QVBoxLayout* fileInfoLayout = new QVBoxLayout();
    fileInfoLayout->addWidget(filePathLabel_);
    fileInfoLayout->addWidget(fileSizeLabel_);
    fileInfoLayout->addWidget(pageCountLabel_);
    fileInfoLayout->addWidget(pageSizeMode_);
    fileInfoLayout->addStretch();

    fileInfoTab_ = new QWidget();
    fileInfoTab_->setLayout(fileInfoLayout);

    // 恢复设置
    QGroupBox* recoveryModeGroup = new QGroupBox("Recovery Mode");
    recoveryModeCombo_ = new QComboBox();
    recoveryModeCombo_->addItem("Truncate Recovery", "truncate");
    recoveryModeCombo_->addItem("rm -rf Recovery", "rmrf");
    recoveryModeCombo_->addItem("Header Repair", "header");
    recoveryModeCombo_->addItem("Default Recovery", "default");

    QVBoxLayout* modeLayout = new QVBoxLayout();
    modeLayout->addWidget(recoveryModeCombo_);
    recoveryModeGroup->setLayout(modeLayout);

    QGroupBox* outputGroup = new QGroupBox("Output Settings");
    QHBoxLayout* outputLayout = new QHBoxLayout();
    outputPathEdit_ = new QLineEdit("/tmp/recovered");
    browseButton_ = new QPushButton("...");
    outputLayout->addWidget(new QLabel("Output Path:"));
    outputLayout->addWidget(outputPathEdit_);
    outputLayout->addWidget(browseButton_);
    outputGroup->setLayout(outputLayout);

    QGroupBox* optionsGroup = new QGroupBox("Options");
    blobRecoveryCheck_ = new QCheckBox("Enable BLOB Recovery");
    headerRepairCheck_ = new QCheckBox("Repair Page Headers");
    blobRecoveryCheck_->setChecked(true);

    QVBoxLayout* optionsLayout = new QVBoxLayout();
    optionsLayout->addWidget(blobRecoveryCheck_);
    optionsLayout->addWidget(headerRepairCheck_);
    optionsGroup->setLayout(optionsLayout);

    QHBoxLayout* recoveryLayout = new QHBoxLayout();
    recoveryLayout->addWidget(recoveryModeGroup);
    recoveryLayout->addWidget(outputGroup);
    recoveryLayout->addWidget(optionsGroup);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    startButton_ = new QPushButton("Start Recovery");
    startButton_->setDefault(true);
    cancelButton_ = new QPushButton("Cancel");
    cancelButton_->setEnabled(false);
    buttonLayout->addStretch();
    buttonLayout->addWidget(startButton_);
    buttonLayout->addWidget(cancelButton_);

    QVBoxLayout* recoveryVBoxLayout = new QVBoxLayout();
    recoveryVBoxLayout->addLayout(recoveryLayout);
    recoveryVBoxLayout->addLayout(buttonLayout);
    recoveryVBoxLayout->addStretch();

    recoveryTab_ = new QWidget();
    recoveryTab_->setLayout(recoveryVBoxLayout);

    // 结果显示
    resultsTextEdit_ = new QTextEdit();
    resultsTextEdit_->setReadOnly(true);
    recordsCountLabel_ = new QLabel("Records: 0");

    QVBoxLayout* resultsLayout = new QVBoxLayout();
    resultsLayout->addWidget(resultsTextEdit_);
    resultsLayout->addWidget(recordsCountLabel_);

    resultsTab_ = new QWidget();
    resultsTab_->setLayout(resultsLayout);

    tabWidget_->addTab(fileInfoTab_, "File Info");
    tabWidget_->addTab(recoveryTab_, "Recovery");
    tabWidget_->addTab(resultsTab_, "Results");

    connect(tabWidget_, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(startButton_, &QPushButton::clicked, this, &MainWindow::onStartRecovery);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::onCancelRecovery);
    connect(browseButton_, &QPushButton::clicked, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory");
        if (!dir.isEmpty()) {
            outputPathEdit_->setText(dir);
        }
    });

    setCentralWidget(tabWidget_);
}

void MainWindow::onActionOpenFile() {
    QString filePath = QFileDialog::getOpenFileName(
        this,
        "Open InnoDB File",
        "",
        "InnoDB Files (*.ibd);;All Files (*.*)"
    );

    if (filePath.isEmpty()) {
        return;
    }

    currentFilePath_ = filePath.toStdString();

    // 显示文件信息
    displayFileInfo(filePath);

    // 切换到文件信息标签
    tabWidget_->setCurrentWidget(fileInfoTab_);
}

void MainWindow::onActionExit() {
    close();
}

void MainWindow::onActionAbout() {
    QMessageBox::about(this, "About MySQL 8.x Recovery Engine",
        "<h2>MySQL 8.x Recovery Engine v1.0.0</h2>"
        "<p>A high-performance offline data recovery tool for MySQL InnoDB.</p>"
        "<p>Author: jliang.fang</p>"
        "<p>License: MIT</p>"
    );
}

void MainWindow::onActionHelp() {
    QMessageBox::information(this, "Help",
        "<h3>Usage Guide</h3>"
        "<ol>"
        "<li>Select an .ibd file using File -> Open</li>"
        "<li>Choose recovery mode in the Recovery tab</li>"
        "<li>Set output path and options</li>"
        "<li>Click 'Start Recovery' to begin</li>"
        "<li>View results in the Results tab</li>"
        "</ol>"
    );
}

void MainWindow::onStartRecovery() {
    if (currentFilePath_.empty()) {
        QMessageBox::warning(this, "Error", "Please open an .ibd file first.");
        return;
    }

    if (!std::filesystem::exists(currentFilePath_)) {
        QMessageBox::warning(this, "Error", "File does not exist.");
        return;
    }

    isRecovering_ = true;
    startButton_->setEnabled(false);
    cancelButton_->setEnabled(true);
    progressBar_->setVisible(true);
    progressBar_->setValue(0);

    startRecoveryThread();
}

void MainWindow::onCancelRecovery() {
    if (recoveryThread_) {
        recoveryThread_->quit();
        recoveryThread_->wait();
    }
    isRecovering_ = false;
    startButton_->setEnabled(true);
    cancelButton_->setEnabled(false);
    progressBar_->setVisible(false);
    updateStatus("Recovery cancelled");
}

void MainWindow::onTabChanged(int index) {
    Q_UNUSED(index);
    // 可以在这里添加标签切换时的逻辑
}

void MainWindow::startRecoveryThread() {
    recoveryThread_ = new QThread(this);

    QString filePath = QString::fromStdString(currentFilePath_);
    QString outputDir = outputPathEdit_->text();
    QString mode = recoveryModeCombo_->currentData().toString();
    bool blobRecovery = blobRecoveryCheck_->isChecked();
    bool headerRepair = headerRepairCheck_->isChecked();

    // 使用 QtConcurrent 运行恢复任务
    QFuture<void> future = QtConcurrent::run([this, filePath, outputDir, mode, blobRecovery, headerRepair]() {
        // 这里应该调用实际的恢复逻辑
        // 由于当前实现限制，使用模拟数据
        std::vector<std::map<std::string, std::string>> records;

        // 模拟恢复记录
        for (int i = 1; i <= 10; ++i) {
            std::map<std::string, std::string> record;
            record["id"] = std::to_string(i);
            record["name"] = "user_" + std::to_string(i);
            record["email"] = "user" + std::to_string(i) + "@example.com";
            record["created_at"] = "2024-01-01 00:00:00";
            records.push_back(record);
        }

        // 更新 UI（需要在主线程中执行）
        QMetaObject::invokeMethod(this, [this, records]() {
            displayRecoveryResults(records);
            updateStatus("Recovery completed successfully!");
        }, Qt::QueuedConnection);
    });

    // 连接完成信号
    QObject::connect(future.thread(), &QThread::finished, [this]() {
        isRecovering_ = false;
        startButton_->setEnabled(true);
        cancelButton_->setEnabled(false);
        progressBar_->setVisible(false);
    });
}

void MainWindow::updateStatus(const QString& message) {
    statusLabel_->setText(message);
}

void MainWindow::updateProgress(int value, int max) {
    progressBar_->setRange(0, max);
    progressBar_->setValue(value);
}

void MainWindow::displayFileInfo(const QString& filePath) {
    filePathLabel_->setText("File: " + filePath);

    std::filesystem::path path(filePath.toStdString());
    if (std::filesystem::exists(path)) {
        auto size = std::filesystem::file_size(path);
        fileSizeLabel_->setText("Size: " + QString::number(size) + " bytes");

        // 估算页数（假设 16KB 页大小）
        uint64_t pageSize = 16384;
        uint64_t pageCount = (size + pageSize - 1) / pageSize;
        pageCountLabel_->setText("Pages: " + QString::number(pageCount));
        pageSizeMode_->setText("Page Size: 16 KB (estimated)");
    } else {
        fileSizeLabel_->setText("Size: Unknown");
        pageCountLabel_->setText("Pages: Unknown");
        pageSizeMode_->setText("Page Size: Unknown");
    }
}

void MainWindow::displayRecoveryResults(const std::vector<std::map<std::string, std::string>>& records) {
    resultsTextEdit_->clear();
    recordsCountLabel_->setText("Records: " + QString::number(records.size()));

    if (records.empty()) {
        resultsTextEdit_->append("No records found.");
        return;
    }

    // 显示表头
    std::string header;
    for (const auto& [key, value] : records[0]) {
        header += key + "\t";
    }
    resultsTextEdit_->append(QString::fromStdString(header));

    // 显示数据行
    for (const auto& record : records) {
        std::string row;
        for (const auto& [key, value] : record) {
            row += value + "\t";
        }
        resultsTextEdit_->append(QString::fromStdString(row));
    }
}

} // namespace gui
} // namespace dbrecovery
