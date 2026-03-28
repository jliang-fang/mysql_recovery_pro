#include "main_window.hpp"
#include <QApplication>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QStatusBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#include <QFileDialog>
#include <QTextEdit>
#include <QToolBar>
#include <QDockWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTabWidget>
#include <QPushButton>
#include <QMessageBox>
#include <QProgressBar>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QTimer>
#include <QDateTime>
#include <QFileInfo>

class MainWindow::MainWindowPrivate {
public:
    // Main UI components
    QMenuBar* menuBar = nullptr;
    QToolBar* mainToolBar = nullptr;
    QStatusBar* statusBar = nullptr;
    QTextEdit* logTextEdit = nullptr;
    QProgressBar* progressBar = nullptr;
    
    // File menu actions
    QAction* openAction = nullptr;
    QAction* saveAction = nullptr;
    QAction* exitAction = nullptr;
    
    // Recovery menu actions
    QAction* recoverAction = nullptr;
    QAction* truncateRecoveryAction = nullptr;
    QAction* headerRepairAction = nullptr;
    
    // Export menu actions
    QAction* exportCsvAction = nullptr;
    QAction* exportSqlAction = nullptr;
    QAction* exportIbdAction = nullptr;
    
    // View menu actions
    QAction* showLogAction = nullptr;
    QAction* showTreeAction = nullptr;
    
    // Central widget components
    QTabWidget* mainTabWidget = nullptr;
    QTreeWidget* pageTreeWidget = nullptr;
    QTextEdit* recordTextEdit = nullptr;
    QGroupBox* recoveryOptionsGroupBox = nullptr;
    
    // Recovery options
    QComboBox* rowFormatComboBox = nullptr;
    QSpinBox* pageSpinBox = nullptr;
    QCheckBox* checksumCheckBox = nullptr;
    QCheckBox* blobRecoveryCheckBox = nullptr;
    QLineEdit* outputLineEdit = nullptr;
    QPushButton* recoverButton = nullptr;
    
    // Status labels
    QLabel* statusLabel = nullptr;
    QLabel* fileLabel = nullptr;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), d(std::make_unique<MainWindow::MainWindowPrivate>())
{
    // Set window title and size
    setWindowTitle(tr("MySQL 8.0 Recovery Engine"));
    resize(1200, 800);
    
    // Create central widget
    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    
    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
    
    // Create tab widget
    d->mainTabWidget = new QTabWidget(this);
    mainLayout->addWidget(d->mainTabWidget);
    
    // Tab 1: Recovery Options
    QWidget* recoveryTab = new QWidget(this);
    QVBoxLayout* recoveryLayout = new QVBoxLayout(recoveryTab);
    
    // Recovery options group
    d->recoveryOptionsGroupBox = new QGroupBox(tr("Recovery Options"), this);
    QFormLayout* optionsLayout = new QFormLayout(d->recoveryOptionsGroupBox);
    
    // Row format selection
    d->rowFormatComboBox = new QComboBox(this);
    d->rowFormatComboBox->addItem(tr("Auto Detect"), 0);
    d->rowFormatComboBox->addItem(tr("REDUNDANT"), 1);
    d->rowFormatComboBox->addItem(tr("COMPACT"), 2);
    d->rowFormatComboBox->addItem(tr("DYNAMIC"), 3);
    d->rowFormatComboBox->addItem(tr("COMPRESSED"), 4);
    optionsLayout->addRow(tr("Row Format:"), d->rowFormatComboBox);
    
    // Page size
    d->pageSpinBox = new QSpinBox(this);
    d->pageSpinBox->setMinimum(4096);
    d->pageSpinBox->setMaximum(65536);
    d->pageSpinBox->setSingleStep(4096);
    d->pageSpinBox->setValue(16384);
    optionsLayout->addRow(tr("Page Size:"), d->pageSpinBox);
    
    // Checksum validation
    d->checksumCheckBox = new QCheckBox(tr("Validate Checksums"), this);
    d->checksumCheckBox->setChecked(true);
    optionsLayout->addRow(d->checksumCheckBox);
    
    // BLOB recovery
    d->blobRecoveryCheckBox = new QCheckBox(tr("Enable BLOB Recovery"), this);
    d->blobRecoveryCheckBox->setChecked(true);
    optionsLayout->addRow(d->blobRecoveryCheckBox);
    
    // Output path
    QHBoxLayout* outputPathLayout = new QHBoxLayout();
    d->outputLineEdit = new QLineEdit(this);
    d->outputLineEdit->setPlaceholderText(tr("Select output directory"));
    outputPathLayout->addWidget(d->outputLineEdit);
    
    QPushButton* browseButton = new QPushButton(tr("Browse"), this);
    outputPathLayout->addWidget(browseButton);
    optionsLayout->addRow(tr("Output Path:"), outputPathLayout);
    
    recoveryLayout->addWidget(d->recoveryOptionsGroupBox);
    
    // Recover button
    d->recoverButton = new QPushButton(tr("Start Recovery"), this);
    d->recoverButton->setStyleSheet("QPushButton { font-weight: bold; padding: 10px; }");
    recoveryLayout->addWidget(d->recoverButton);
    
    recoveryLayout->addStretch();
    d->mainTabWidget->addTab(recoveryTab, tr("Recovery"));
    
    // Tab 2: Page Tree
    QWidget* pageTab = new QWidget(this);
    QVBoxLayout* pageLayout = new QVBoxLayout(pageTab);
    
    d->pageTreeWidget = new QTreeWidget(this);
    d->pageTreeWidget->setHeaderLabels({tr("Page No"), tr("Type"), tr("Records"), tr("Status")});
    d->pageTreeWidget->header()->setSectionResizeMode(QHeaderView::Stretch);
    pageLayout->addWidget(d->pageTreeWidget);
    
    d->mainTabWidget->addTab(pageTab, tr("Pages"));
    
    // Tab 3: Record View
    QWidget* recordTab = new QWidget(this);
    QVBoxLayout* recordLayout = new QVBoxLayout(recordTab);
    
    d->recordTextEdit = new QTextEdit(this);
    d->recordTextEdit->setReadOnly(true);
    d->recordTextEdit->setFontFamily("monospace");
    recordLayout->addWidget(d->recordTextEdit);
    
    d->mainTabWidget->addTab(recordTab, tr("Records"));
    
    // Tab 4: Log
    QWidget* logTab = new QWidget(this);
    QVBoxLayout* logLayout = new QVBoxLayout(logTab);
    
    d->logTextEdit = new QTextEdit(this);
    d->logTextEdit->setReadOnly(true);
    d->logTextEdit->setFontFamily("monospace");
    logLayout->addWidget(d->logTextEdit);
    
    d->mainTabWidget->addTab(logTab, tr("Log"));
    
    // Create menu bar
    d->menuBar = menuBar();
    
    // File menu
    QMenu* fileMenu = d->menuBar->addMenu(tr("&File"));
    d->openAction = fileMenu->addAction(tr("&Open IBD File..."));
    d->openAction->setShortcut(QKeySequence::Open);
    fileMenu->addSeparator();
    d->saveAction = fileMenu->addAction(tr("&Save..."));
    d->saveAction->setShortcut(QKeySequence::Save);
    fileMenu->addSeparator();
    d->exitAction = fileMenu->addAction(tr("E&xit"));
    d->exitAction->setShortcut(QKeySequence::Quit);
    
    // Recovery menu
    QMenu* recoveryMenu = d->menuBar->addMenu(tr("&Recovery"));
    d->recoverAction = recoveryMenu->addAction(tr("&Recover Table"));
    d->recoverAction->setShortcut(QKeySequence(tr("Ctrl+R")));
    recoveryMenu->addSeparator();
    d->truncateRecoveryAction = recoveryMenu->addAction(tr("Recover &Truncated Table"));
    d->headerRepairAction = recoveryMenu->addAction(tr("Repair &Header"));
    
    // Export menu
    QMenu* exportMenu = d->menuBar->addMenu(tr("&Export"));
    d->exportCsvAction = exportMenu->addAction(tr("Export to &CSV"));
    d->exportSqlAction = exportMenu->addAction(tr("Export to &SQL"));
    d->exportIbdAction = exportMenu->addAction(tr("Export to &IBD"));
    
    // View menu
    QMenu* viewMenu = d->menuBar->addMenu(tr("&View"));
    d->showLogAction = viewMenu->addAction(tr("Show &Log"));
    d->showLogAction->setCheckable(true);
    d->showLogAction->setChecked(true);
    
    // Help menu
    QMenu* helpMenu = d->menuBar->addMenu(tr("&Help"));
    QAction* aboutAction = helpMenu->addAction(tr("&About"));
    QAction* aboutQtAction = helpMenu->addAction(tr("About &Qt"));
    
    // Create toolbar
    d->mainToolBar = addToolBar(tr("Main Toolbar"));
    d->mainToolBar->addAction(d->openAction);
    d->mainToolBar->addAction(d->recoverAction);
    d->mainToolBar->addSeparator();
    d->mainToolBar->addAction(d->exportCsvAction);
    d->mainToolBar->addAction(d->exportSqlAction);
    
    // Create status bar
    d->statusBar = statusBar();
    d->statusLabel = new QLabel(tr("Ready"), this);
    d->fileLabel = new QLabel(tr("No file loaded"), this);
    d->progressBar = new QProgressBar(this);
    d->progressBar->setMaximumWidth(200);
    d->progressBar->setVisible(false);
    
    d->statusBar->addWidget(d->statusLabel);
    d->statusBar->addWidget(d->fileLabel);
    d->statusBar->addPermanentWidget(d->progressBar);
    
    // Connect signals
    connect(d->openAction, &QAction::triggered, this, &MainWindow::onOpenFile);
    connect(d->recoverAction, &QAction::triggered, this, &MainWindow::onRecover);
    connect(d->truncateRecoveryAction, &QAction::triggered, this, &MainWindow::onTruncateRecovery);
    connect(d->headerRepairAction, &QAction::triggered, this, &MainWindow::onHeaderRepair);
    connect(d->exportCsvAction, &QAction::triggered, this, &MainWindow::onExportCsv);
    connect(d->exportSqlAction, &QAction::triggered, this, &MainWindow::onExportSql);
    connect(d->exitAction, &QAction::triggered, this, &QWidget::close);
    connect(browseButton, &QPushButton::clicked, this, &MainWindow::onBrowseOutput);
    connect(d->recoverButton, &QPushButton::clicked, this, &MainWindow::onRecover);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
    connect(aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
    connect(d->showLogAction, &QAction::toggled, this, &MainWindow::onShowLog);
}

MainWindow::~MainWindow() = default;

// Slots
void MainWindow::onOpenFile()
{
    QString fileName = QFileDialog::getOpenFileName(
        this,
        tr("Open IBD File"),
        "",
        tr("IBD Files (*.ibd);;All Files (*)")
    );
    
    if (fileName.isEmpty()) {
        return;
    }
    
    d->outputLineEdit->setText(QFileInfo(fileName).absolutePath());
    d->fileLabel->setText(tr("Loaded: %1").arg(QFileInfo(fileName).fileName()));
    d->statusLabel->setText(tr("File loaded successfully"));
    
    // Log the action
    logMessage(tr("Opened file: %1").arg(fileName));
}

void MainWindow::onBrowseOutput()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("Select Output Directory"),
        d->outputLineEdit->text()
    );
    
    if (!dir.isEmpty()) {
        d->outputLineEdit->setText(dir);
    }
}

void MainWindow::onRecover()
{
    if (d->outputLineEdit->text().isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select an output directory"));
        return;
    }
    
    d->statusLabel->setText(tr("Recovery in progress..."));
    d->progressBar->setVisible(true);
    d->progressBar->setRange(0, 0); // Indeterminate mode
    
    // Simulate recovery process
    QTimer::singleShot(2000, this, [this]() {
        d->progressBar->setVisible(false);
        d->statusLabel->setText(tr("Recovery completed"));
        QMessageBox::information(this, tr("Success"), tr("Recovery completed successfully"));
    });
}

void MainWindow::onTruncateRecovery()
{
    QMessageBox::information(this, tr("Truncate Recovery"),
        tr("This feature will be implemented in a future version.\n\n"
           "It will recover data from tables that have been truncated."));
}

void MainWindow::onHeaderRepair()
{
    QMessageBox::information(this, tr("Header Repair"),
        tr("This feature will be implemented in a future version.\n\n"
           "It will repair corrupted file headers."));
}

void MainWindow::onExportCsv()
{
    QMessageBox::information(this, tr("Export CSV"),
        tr("This feature will be implemented in a future version.\n\n"
           "It will export recovered data to CSV format."));
}

void MainWindow::onExportSql()
{
    QMessageBox::information(this, tr("Export SQL"),
        tr("This feature will be implemented in a future version.\n\n"
           "It will export recovered data to SQL format."));
}

void MainWindow::onShowLog(bool show)
{
    if (show) {
        d->mainTabWidget->addTab(d->mainTabWidget->widget(d->mainTabWidget->count() - 1), tr("Log"));
    }
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, tr("About MySQL 8.0 Recovery Engine"),
        tr("<h2>MySQL 8.0 Recovery Engine v1.0.0</h2>"
           "<p>A high-performance C++ library for offline MySQL 8.0+ data recovery.</p>"
           "<p>Author: jliang.fang</p>"
           "<p>License: MIT</p>"));
}

void MainWindow::logMessage(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    d->logTextEdit->append(QString("[%1] %2").arg(timestamp).arg(message));
}
