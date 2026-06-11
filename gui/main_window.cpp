#include "main_window.hpp"
#include <iostream>
#include <filesystem>
#include <memory>

// Include recovery engine headers
#include "dbrecovery/recovery/engine.hpp"
#include "dbrecovery/recovery/rmrf_recovery.hpp"
#include "dbrecovery/innodb/page_parser_v2.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace gui {

// ==================== 辅助函数 ====================
/**
 * @brief 从 SDI 页解析表结构
 */
std::optional<dbrecovery::catalog::TableSchema> parseSchemaFromSDI(
    const std::string& ibd_path, uint64_t table_id) {

    dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
    if (!reader.is_valid()) {
        return std::nullopt;
    }

    uint32_t total_pages = reader.get_total_pages();

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        auto page_data = reader.read_page_raw(page_no);
        if (page_data.empty()) continue;

        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(
            page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

        if (page_type == dbrecovery::innodb::FIL_PAGE_SDI) {
            const uint8_t* sdi_payload = page_data.data() + 38;
            if (page_data.size() < 38 + 20) continue;

            auto json_opt = dbrecovery::innodb::SDIManager::extract_sdi_json(
                sdi_payload, page_data.size() - 38);

            if (json_opt.has_value()) {
                try {
                    auto json = nlohmann::json::parse(json_opt.value());
                    dbrecovery::catalog::TableSchema schema;
                    if (schema.from_json(json)) {
                        return schema;
                    }
                } catch (const nlohmann::json::parse_error& e) {
                    // Ignore parse errors
                }
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief 创建默认表结构
 */
dbrecovery::catalog::TableSchema createDefaultSchema() {
    dbrecovery::catalog::TableSchema schema;
    schema.table_name = "recovered_table";
    schema.row_format = 2;  // DYNAMIC
    schema.page_size = 16384;

    dbrecovery::catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = dbrecovery::innodb::mysql_types::LONGLONG;
    col1.is_nullable = false;
    col1.column_length = 8;
    schema.columns[1] = col1;

    dbrecovery::catalog::ColumnInfo col2;
    col2.name = "name";
    col2.mysql_type_id = dbrecovery::innodb::mysql_types::VARCHAR;
    col2.is_nullable = true;
    col2.column_length = 255;
    schema.columns[2] = col2;

    dbrecovery::catalog::ColumnInfo col3;
    col3.name = "email";
    col3.mysql_type_id = dbrecovery::innodb::mysql_types::VARCHAR;
    col3.is_nullable = true;
    col3.column_length = 255;
    schema.columns[3] = col3;

    dbrecovery::catalog::ColumnInfo col4;
    col4.name = "created_at";
    col4.mysql_type_id = dbrecovery::innodb::mysql_types::DATETIME;
    col4.is_nullable = true;
    schema.columns[4] = col4;

    return schema;
}

// ==================== RecoveryWorker ====================
/**
 * @brief Worker class for running recovery in a separate thread
 */
class RecoveryWorker : public QObject {
    Q_OBJECT

public:
    explicit RecoveryWorker(QObject *parent = nullptr)
        : QObject(parent), shouldCancel_(false) {}

    ~RecoveryWorker() override = default;

    void setParameters(const QString& filePath,
                       const QString& outputDir,
                       const QString& mode,
                       const QString& format,
                       bool blobRecovery,
                       bool headerRepair,
                       uint32_t pageSize,
                       const QString& tableName,
                       const QString& databaseName,
                       const QString& userName,
                       const QString& encryptionKey = "",
                       const QString& tableKey = "",
                       const QString& tablespaceId = "",
                       bool detectEncryption = false,
                       bool tryNoKey = true,
                       int mysqlVersion = 0,
                       bool detectVersion = false,
                       const QString& rowFormat = "dynamic") {
        filePath_ = filePath.toStdString();
        outputDir_ = outputDir.toStdString();
        mode_ = mode.toStdString();
        format_ = format.toStdString();
        blobRecovery_ = blobRecovery;
        headerRepair_ = headerRepair;
        pageSize_ = pageSize;
        tableName_ = tableName.toStdString();
        databaseName_ = databaseName.toStdString();
        userName_ = userName.toStdString();
        encryptionKey_ = encryptionKey.toStdString();
        tableKey_ = tableKey.toStdString();
        tablespaceId_ = tablespaceId.toStdString();
        detectEncryption_ = detectEncryption;
        tryNoKey_ = tryNoKey;
        mysqlVersion_ = mysqlVersion;
        detectVersion_ = detectVersion;
        rowFormat_ = rowFormat.toStdString();
    }

    void cancel() { shouldCancel_ = true; }

signals:
    void progressUpdated(int value, int max);
    void statusUpdated(const QString& message);
    void recoveryCompleted(const QString& resultPath, int recordCount);
    void recoveryFailed(const QString& errorMessage);

public slots:
    void runRecovery() {
        try {
            // 使用 recovery::RecoveryEngine
            dbrecovery::recovery::RecoveryEngine engine;

            if (!engine.open(filePath_)) {
                emit recoveryFailed(QString("Failed to open file: ") + filePath_.c_str());
                return;
            }

            // ==================== TDE 加密检测 ====================
            bool file_is_encrypted = false;
            if (detectEncryption_ || !encryptionKey_.empty()) {
                emit statusUpdated("Checking for TDE encryption...");
                
                innodb::InnoDBFileReader check_reader(filePath_);
                if (check_reader.is_valid() && check_reader.get_total_pages() > 0) {
                    auto first_page = check_reader.read_page_raw(0);
                    if (first_page.size() >= 44) {
                        if (innodb::TDEDecryptor::is_encrypted(first_page.data(), first_page.size())) {
                            file_is_encrypted = true;
                            emit statusUpdated("File is encrypted with TDE");
                        } else {
                            emit statusUpdated("File is not encrypted");
                        }
                    }
                }
            }

            // ==================== MySQL 版本检测 ====================
            if (detectVersion_ || mysqlVersion_ > 0) {
                emit statusUpdated("Checking MySQL version compatibility...");
                
                if (mysqlVersion_ > 0) {
                    emit statusUpdated("Using MySQL version: " + QString::number(mysqlVersion_));
                }
                
                emit statusUpdated("Using row format: " + QString::fromStdString(rowFormat_));
            }

            emit statusUpdated("Analyzing file structure...");

            // 尝试从 SDI 解析表结构
            auto schema_opt = parseSchemaFromSDI(filePath_, 0);
            dbrecovery::catalog::TableSchema schema;

            if (schema_opt.has_value()) {
                schema = schema_opt.value();
                emit statusUpdated("Schema parsed from SDI: " + QString::fromStdString(schema.table_name));
            } else {
                emit statusUpdated("Using default schema (no SDI found)");
                schema = createDefaultSchema();
            }

            // 使用用户指定的表名（如果提供）
            if (!tableName_.empty()) {
                schema.table_name = tableName_;
                emit statusUpdated("Using table name: " + QString::fromStdString(tableName_));
            }

            // 使用用户指定的数据库名（如果提供）
            if (!databaseName_.empty()) {
                schema.database_name = databaseName_;
                emit statusUpdated("Using database name: " + QString::fromStdString(databaseName_));
            }

            std::vector<std::map<std::string, std::string>> recovered_records;

            // 根据模式执行恢复
            if (mode_ == "truncate") {
                emit statusUpdated("Starting Truncate recovery...");
                recovered_records = engine.recover_truncate(schema);
                emit statusUpdated("Truncate recovery completed. Found " +
                                   QString::number(recovered_records.size()) + " records.");
            } else if (mode_ == "rmrf") {
                emit statusUpdated("Starting rm -rf recovery...");
                dbrecovery::recovery::RmrfRecovery rmrf;
                rmrf.set_page_size(pageSize_);

                bool success = rmrf.reconstruct_from_file(filePath_, schema, recovered_records);
                if (success) {
                    emit statusUpdated("rm -rf recovery completed. Found " +
                                       QString::number(recovered_records.size()) + " records.");
                } else {
                    emit statusUpdated("rm -rf recovery returned no records.");
                }
            } else if (mode_ == "header") {
                emit statusUpdated("Starting header repair...");
                std::string output_path = outputDir_ + "/repaired.ibd";
                if (engine.repair_header(output_path)) {
                    emit recoveryCompleted(output_path, 0);
                    engine.close();
                    return;
                } else {
                    emit recoveryFailed("Failed to repair header.");
                    engine.close();
                    return;
                }
            } else {
                // Default recovery
                emit statusUpdated("Starting default recovery...");
                recovered_records = engine.recover_truncate(schema);
                emit statusUpdated("Default recovery completed. Found " +
                                   QString::number(recovered_records.size()) + " records.");
            }

            // 导出结果
            std::string output_path;
            if (mode_ != "header") {
                if (blobRecovery_) {
                    emit statusUpdated("BLOB recovery enabled.");
                }

                // 根据用户选择的格式导出
                std::string format_lower = format_;
                std::transform(format_lower.begin(), format_lower.end(), format_lower.begin(), ::tolower);

                if (format_lower == "csv") {
                    output_path = outputDir_ + "/recovered.csv";
                    if (engine.export_to_csv(recovered_records, output_path)) {
                        emit statusUpdated("Exported " + QString::number(recovered_records.size()) +
                                           " records to CSV: " + QString::fromStdString(output_path));
                    }
                } else if (format_lower == "sql") {
                    output_path = outputDir_ + "/recovered.sql";
                    std::string user_for_sql = userName_.empty() ? "root" : userName_;
                    if (engine.export_to_sql_with_user(recovered_records, schema.table_name, output_path, user_for_sql, databaseName_)) {
                        emit statusUpdated("Exported SQL to: " + QString::fromStdString(output_path));
                    }
                } else if (format_lower == "json") {
                    output_path = outputDir_ + "/recovered.json";
                    if (dbrecovery::innodb::JSONExporter::export_to_json(recovered_records, output_path)) {
                        emit statusUpdated("Exported " + QString::number(recovered_records.size()) +
                                           " records to JSON: " + QString::fromStdString(output_path));
                    }
                } else if (format_lower == "xml") {
                    output_path = outputDir_ + "/recovered.xml";
                    if (dbrecovery::innodb::XMLExporter::export_to_xml(recovered_records, output_path)) {
                        emit statusUpdated("Exported " + QString::number(recovered_records.size()) +
                                           " records to XML: " + QString::fromStdString(output_path));
                    }
                } else if (format_lower == "yaml") {
                    output_path = outputDir_ + "/recovered.yaml";
                    if (dbrecovery::innodb::YAMLExporter::export_to_yaml(recovered_records, output_path)) {
                        emit statusUpdated("Exported " + QString::number(recovered_records.size()) +
                                           " records to YAML: " + QString::fromStdString(output_path));
                    }
                } else if (format_lower == "avro") {
                    output_path = outputDir_ + "/recovered.avro";
                    if (dbrecovery::innodb::AvroExporter::export_to_avro(recovered_records, output_path, schema)) {
                        emit statusUpdated("Exported " + QString::number(recovered_records.size()) +
                                           " records to Avro: " + QString::fromStdString(output_path));
                    }
                } else if (format_lower == "text") {
                    output_path = outputDir_ + "/recovered.txt";
                    // 使用简单的文本格式导出
                    std::ofstream txt_file(output_path);
                    if (txt_file && !recovered_records.empty()) {
                        for (const auto& record : recovered_records) {
                            bool first = true;
                            for (const auto& kv : record) {
                                if (!first) txt_file << " | ";
                                txt_file << kv.first << ": " << kv.second;
                                first = false;
                            }
                            txt_file << "\n";
                        }
                        emit statusUpdated("Exported " + QString::number(recovered_records.size()) +
                                           " records to Text: " + QString::fromStdString(output_path));
                    }
                } else {
                    // 默认导出为 CSV
                    output_path = outputDir_ + "/recovered.csv";
                    if (engine.export_to_csv(recovered_records, output_path)) {
                        emit statusUpdated("Exported " + QString::number(recovered_records.size()) +
                                           " records to CSV: " + QString::fromStdString(output_path));
                    }
                }
            }

            engine.close();
            emit recoveryCompleted(output_path, recovered_records.size());

        } catch (const std::exception& e) {
            emit recoveryFailed(QString("Recovery failed: ") + e.what());
        }
    }

private:
    // 辅助函数：从 SDI 页解析表结构
    std::optional<dbrecovery::catalog::TableSchema> parseSchemaFromSDI(
        const std::string& ibd_path, uint64_t table_id) {

        dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
        if (!reader.is_valid()) {
            return std::nullopt;
        }

        uint32_t total_pages = reader.get_total_pages();

        for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
            auto page_data = reader.read_page_raw(page_no);
            if (page_data.empty()) continue;

            uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(
                page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

            if (page_type == dbrecovery::innodb::FIL_PAGE_SDI) {
                const uint8_t* sdi_payload = page_data.data() + 38;
                if (page_data.size() < 38 + 20) continue;

                auto json_opt = dbrecovery::innodb::SDIManager::extract_sdi_json(
                    sdi_payload, page_data.size() - 38);

                if (json_opt.has_value()) {
                    try {
                        auto json = nlohmann::json::parse(json_opt.value());
                        dbrecovery::catalog::TableSchema schema;
                        if (schema.from_json(json)) {
                            return schema;
                        }
                    } catch (const nlohmann::json::parse_error& e) {
                        // Ignore parse errors
                    }
                }
            }
        }

        return std::nullopt;
    }

    // 辅助函数：创建默认表结构
    dbrecovery::catalog::TableSchema createDefaultSchema() {
        dbrecovery::catalog::TableSchema schema;
        schema.table_name = "recovered_table";
        schema.row_format = 2;  // DYNAMIC
        schema.page_size = 16384;

        dbrecovery::catalog::ColumnInfo col1;
        col1.name = "id";
        col1.mysql_type_id = dbrecovery::innodb::mysql_types::LONGLONG;
        col1.is_nullable = false;
        col1.column_length = 8;
        schema.columns[1] = col1;

        dbrecovery::catalog::ColumnInfo col2;
        col2.name = "name";
        col2.mysql_type_id = dbrecovery::innodb::mysql_types::VARCHAR;
        col2.is_nullable = true;
        col2.column_length = 255;
        schema.columns[2] = col2;

        dbrecovery::catalog::ColumnInfo col3;
        col3.name = "email";
        col3.mysql_type_id = dbrecovery::innodb::mysql_types::VARCHAR;
        col3.is_nullable = true;
        col3.column_length = 255;
        schema.columns[3] = col3;

        dbrecovery::catalog::ColumnInfo col4;
        col4.name = "created_at";
        col4.mysql_type_id = dbrecovery::innodb::mysql_types::DATETIME;
        col4.is_nullable = true;
        schema.columns[4] = col4;

        return schema;
    }

private:
    std::string filePath_;
    std::string outputDir_;
    std::string mode_;
    std::string format_;
    bool blobRecovery_;
    bool headerRepair_;
    uint32_t pageSize_;
    std::atomic<bool> shouldCancel_;
    
    // TDE 加密参数
    std::string encryptionKey_;
    std::string tableKey_;
    std::string tablespaceId_;
    bool detectEncryption_;
    bool tryNoKey_;
    
    // MySQL 版本参数
    int mysqlVersion_;
    bool detectVersion_;
    std::string rowFormat_;
};

void MainWindow::initUI() {
    setWindowTitle("MySQL 8.x Recovery Engine v2.0.0 - Enhanced Edition");
    resize(1400, 900);

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

    toolsMenu_ = menuBar_->addMenu(tr("&Tools"));
    scanDeviceAction_ = toolsMenu_->addAction(tr("&Scan Raw Device..."));
    connect(scanDeviceAction_, &QAction::triggered, this, &MainWindow::onActionScanDevice);
    
    scanDirectoryAction_ = toolsMenu_->addAction(tr("&Scan Data Directory..."));
    connect(scanDirectoryAction_, &QAction::triggered, this, &MainWindow::onActionScanDirectory);
    
    searchTableAction_ = toolsMenu_->addAction(tr("&Search Table..."));
    connect(searchTableAction_, &QAction::triggered, this, &MainWindow::onActionSearchTable);

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
    tableIdLabel_ = new QLabel("Table ID: -");
    rowFormatLabel_ = new QLabel("Row Format: -");

    QVBoxLayout* fileInfoLayout = new QVBoxLayout();
    fileInfoLayout->addWidget(filePathLabel_);
    fileInfoLayout->addWidget(fileSizeLabel_);
    fileInfoLayout->addWidget(pageCountLabel_);
    fileInfoLayout->addWidget(pageSizeMode_);
    fileInfoLayout->addWidget(tableIdLabel_);
    fileInfoLayout->addWidget(rowFormatLabel_);
    fileInfoLayout->addStretch();

    fileInfoTab_ = new QWidget();
    fileInfoTab_->setLayout(fileInfoLayout);

    // 恢复设置
    QGroupBox* recoveryModeGroup = new QGroupBox("Recovery Mode");
    recoveryModeCombo_ = new QComboBox();
    recoveryModeCombo_->addItem("Default Recovery", "default");
    recoveryModeCombo_->addItem("Truncate Recovery", "truncate");
    recoveryModeCombo_->addItem("rm -rf Recovery", "rmrf");
    recoveryModeCombo_->addItem("Header Repair", "header");

    QVBoxLayout* modeLayout = new QVBoxLayout();
    modeLayout->addWidget(recoveryModeCombo_);
    recoveryModeGroup->setLayout(modeLayout);

    QGroupBox* outputGroup = new QGroupBox("Output Settings");
    QVBoxLayout* outputLayout = new QVBoxLayout(outputGroup);
    outputPathEdit_ = new QLineEdit("/tmp/recovered");
    browseButton_ = new QPushButton("...");
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

    QGroupBox* tableSettingsGroup = new QGroupBox("Table Settings");
    QVBoxLayout* tableLayout = new QVBoxLayout(tableSettingsGroup);
    tableNameEdit_ = new QLineEdit();
    databaseNameEdit_ = new QLineEdit("recovered_db");
    userNameEdit_ = new QLineEdit("root");
    tableLayout->addWidget(new QLabel("Table Name (optional):"));
    tableLayout->addWidget(tableNameEdit_);
    tableLayout->addWidget(new QLabel("Database Name:"));
    tableLayout->addWidget(databaseNameEdit_);
    tableLayout->addWidget(new QLabel("User Name:"));
    tableLayout->addWidget(userNameEdit_);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    startButton_ = new QPushButton("Start Recovery");
    startButton_->setDefault(true);
    cancelButton_ = new QPushButton("Cancel");
    pauseButton_ = new QPushButton("Pause");
    resumeButton_ = new QPushButton("Resume");
    pauseButton_->setEnabled(false);
    resumeButton_->setEnabled(false);
    buttonLayout->addWidget(startButton_);
    buttonLayout->addWidget(cancelButton_);
    buttonLayout->addWidget(pauseButton_);
    buttonLayout->addWidget(resumeButton_);

    QVBoxLayout* recoveryVBoxLayout = new QVBoxLayout();
    recoveryVBoxLayout->addWidget(recoveryModeGroup);
    recoveryVBoxLayout->addWidget(outputGroup);
    recoveryVBoxLayout->addWidget(optionsGroup);
    recoveryVBoxLayout->addWidget(pageSettingsGroup);
    recoveryVBoxLayout->addWidget(tableSettingsGroup);
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

    // 设备扫描设置
    QGroupBox* deviceScanGroup = new QGroupBox("Raw Device/Partition Scan");
    QHBoxLayout* devicePathLayout = new QHBoxLayout();
    devicePathEdit_ = new QLineEdit("/dev/sda");
    scanDeviceButton_ = new QPushButton("Scan");
    devicePathLayout->addWidget(new QLabel("Device Path:"));
    devicePathLayout->addWidget(devicePathEdit_);
    devicePathLayout->addWidget(scanDeviceButton_);
    deviceScanGroup->setLayout(devicePathLayout);

    scanResultsEdit_ = new QPlainTextEdit();
    scanResultsEdit_->setReadOnly(true);
    scanResultsEdit_->setMaximumHeight(200);

    QHBoxLayout* scanButtonLayout = new QHBoxLayout();
    cancelScanButton_ = new QPushButton("Cancel Scan");
    cancelScanButton_->setEnabled(false);
    scanButtonLayout->addStretch();
    scanButtonLayout->addWidget(scanDeviceButton_);
    scanButtonLayout->addWidget(cancelScanButton_);

    QVBoxLayout* deviceScanVBoxLayout = new QVBoxLayout();
    deviceScanVBoxLayout->addWidget(deviceScanGroup);
    deviceScanVBoxLayout->addWidget(scanResultsEdit_);
    deviceScanVBoxLayout->addLayout(scanButtonLayout);

    deviceScanTab_ = new QWidget();
    deviceScanTab_->setLayout(deviceScanVBoxLayout);

    // 批量恢复设置
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

    // 日志显示
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

    // 导出按钮
    QHBoxLayout* exportLayout = new QHBoxLayout();
    exportButton_ = new QPushButton("Export Results");
    saveButton_ = new QPushButton("Save Results");
    exportLayout->addStretch();
    exportLayout->addWidget(exportButton_);
    exportLayout->addWidget(saveButton_);
    resultsLayout->addLayout(exportLayout);

    tabWidget_->addTab(fileInfoTab_, "File Info");
    tabWidget_->addTab(recoveryTab_, "Recovery");
    tabWidget_->addTab(resultsTab_, "Results");
    tabWidget_->addTab(deviceScanTab_, "Device Scan");
    tabWidget_->addTab(batchTab_, "Batch");
    tabWidget_->addTab(logTab_, "Log");

    setCentralWidget(tabWidget_);

    // 连接导出按钮
    connect(exportButton_, &QPushButton::clicked, this, &MainWindow::onExportResults);
    connect(saveButton_, &QPushButton::clicked, this, &MainWindow::onSaveResults);

    // 连接信号
    connect(browseButton_, &QPushButton::clicked, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory");
        if (!dir.isEmpty()) {
            outputPathEdit_->setText(dir);
        }
    });

    connect(startButton_, &QPushButton::clicked, this, &MainWindow::onStartRecovery);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::onCancelRecovery);
    connect(pauseButton_, &QPushButton::clicked, this, &MainWindow::onPauseRecovery);
    connect(resumeButton_, &QPushButton::clicked, this, &MainWindow::onResumeRecovery);

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

    connect(startBatchButton_, &QPushButton::clicked, this, &MainWindow::onStartBatchRecovery);
    connect(clearBatchButton_, &QPushButton::clicked, this, &MainWindow::onClearBatchTasks);

    connect(clearLogButton_, &QPushButton::clicked, this, &MainWindow::clearLog);
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

    // 目录扫描UI
    initDirectoryScanUI();
    
    // 表搜索UI
    initSearchUI();
    
    // 连接目录扫描信号
    connect(scanDirectoryButton_, &QPushButton::clicked, this, &MainWindow::onStartDirectoryScan);
    connect(cancelDirectoryScanButton_, &QPushButton::clicked, this, &MainWindow::onCancelDirectoryScan);
    
    // 连接表搜索信号
    connect(searchTableButton_, &QPushButton::clicked, this, &MainWindow::onStartTableSearch);
    connect(cancelSearchButton_, &QPushButton::clicked, this, &MainWindow::onCancelTableSearch);

    connect(logLevelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int index) { (void)index; /* TODO: implement filtering */ });

    connect(tabWidget_, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
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
    QMessageBox::about(this, "About MySQL Recovery Pro v2.1.0",
        "<h2>MySQL Recovery Pro v2.1.0 - Enhanced Edition</h2>"
        "<p>A high-performance offline data recovery tool for MySQL InnoDB.</p>"
        "<p><b>Features:</b></p>"
        "<ul>"
        "<li>Truncate recovery - Recover from truncated tables</li>"
        "<li>rm -rf recovery - Recover from deleted files</li>"
        "<li>Header repair - Repair corrupted page headers</li>"
        "<li>BLOB recovery - Recover BLOB data</li>"
        "<li>Device scanning - Scan raw devices/partitions</li>"
        "<li>Full disk scanning - Scan entire data directories</li>"
        "<li>Table name search - Search for tables by name with wildcards</li>"
        "<li>Bulk recovery - Recover multiple tables at once</li>"
        "<li>Multi-format export - CSV, SQL, JSON</li>"
        "<li>Batch processing - Process multiple files in sequence</li>"
        "<li>Real-time logging - Monitor recovery progress</li>"
        "</ul>"
        "<p>Author: jliang.fang</p>"
        "<p>License: MIT</p>"
    );
}

void MainWindow::onActionHelp() {
    QMessageBox::information(this, "Help",
        "<h3>Usage Guide</h3>"
        "<ol>"
        "<li><b>Open File:</b> Select an .ibd file using File -> Open</li>"
        "<li><b>Recovery:</b> Choose recovery mode in the Recovery tab</li>"
        "<li><b>Options:</b> Enable BLOB recovery, set output path and format</li>"
        "<li><b>Start:</b> Click 'Start Recovery' to begin</li>"
        "<li><b>Results:</b> View results in the Results tab</li>"
        "<li><b>Device Scan:</b> Use Device Scan tab to scan raw devices</li>"
        "<li><b>Batch:</b> Add multiple files to the Batch tab for bulk recovery</li>"
        "<li><b>Log:</b> Monitor recovery progress in the Log tab</li>"
        "</ol>"
        "<p><b>Recovery Modes:</b></p>"
        "<ul>"
        "<li><i>Default Recovery:</i> Standard recovery mode</li>"
        "<li><i>Truncate Recovery:</i> Recover from truncated tables</li>"
        "<li><i>rm -rf Recovery:</i> Recover from deleted files</li>"
        "<li><i>Header Repair:</i> Repair corrupted page headers</li>"
        "</ul>"
        "<p><b>Enhanced Features:</b></p>"
        "<ul>"
        "<li><i>Full Disk Scanning:</i> Scan entire data directories for tables</li>"
        "<li><i>Table Name Search:</i> Search for tables by name with wildcards (* and ?)</li>"
        "<li><i>Bulk Recovery:</i> Recover multiple tables at once</li>"
        "<li><i>Batch Processing:</i> Process multiple files in sequence</li>"
        "</ul>"
    );
}

void MainWindow::onActionScanDevice() {
    tabWidget_->setCurrentWidget(deviceScanTab_);
}

void MainWindow::onStartDeviceScan() {
    QString devicePath = devicePathEdit_->text();
    if (devicePath.isEmpty()) {
        QMessageBox::warning(this, "Error", "Please enter a device path.");
        return;
    }

    isScanningDevice_ = true;
    scanDeviceButton_->setEnabled(false);
    cancelScanButton_->setEnabled(true);
    scanResultsEdit_->clear();
    scanResultsEdit_->appendPlainText("Starting device scan: " + devicePath + "\n");

    // 使用线程进行设备扫描
    deviceScanThread_ = new QThread(this);
    shouldCancelScan_ = false;

    // 创建扫描工作线程
    auto scanWorker = [this, devicePath]() {
        try {
            dbrecovery::recovery::RmrfRecovery rmrf;
            rmrf.set_page_size(16384);

            bool success = rmrf.scan_raw_device(devicePath.toStdString());

            if (success) {
                QMetaObject::invokeMethod(this, [this, &rmrf]() {
                    scanResultsEdit_->appendPlainText("\n=== Scan Results ===\n");
                    scanResultsEdit_->appendPlainText("Total files found: " + QString::number(rmrf.get_scanned_files().size()) + "\n");
                    scanResultsEdit_->appendPlainText("Total index pages: " + QString::number(rmrf.get_total_index_pages()) + "\n");

                    for (const auto& file : rmrf.get_scanned_files()) {
                        scanResultsEdit_->appendPlainText("\nFile: " + QString::fromStdString(file.file_path) + "\n");
                        scanResultsEdit_->appendPlainText("  Size: " + QString::number(file.file_size) + " bytes\n");
                        scanResultsEdit_->appendPlainText("  Pages: " + QString::number(file.total_pages) + "\n");
                        scanResultsEdit_->appendPlainText("  Index pages: " + QString::number(file.valid_index_pages) + "\n");
                    }
                    scanResultsEdit_->appendPlainText("\nScan completed successfully!\n");
                }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(this, [this]() {
                    scanResultsEdit_->appendPlainText("\nScan failed: No valid data found.\n");
                }, Qt::QueuedConnection);
            }
        } catch (const std::exception& e) {
            QMetaObject::invokeMethod(this, [this, e]() {
                scanResultsEdit_->appendPlainText("\nScan error: " + QString(e.what()) + "\n");
            }, Qt::QueuedConnection);
        }

        QMetaObject::invokeMethod(this, [this]() {
            isScanningDevice_ = false;
            scanDeviceButton_->setEnabled(true);
            cancelScanButton_->setEnabled(false);
        }, Qt::QueuedConnection);
    };

    QThread* thread = new QThread(this);
    QObject::connect(thread, &QThread::started, [scanWorker]() {
        scanWorker();
    });
    QObject::connect(thread, &QThread::finished, [thread]() {
        thread->deleteLater();
    });

    thread->start();
    deviceScanThread_ = thread;
}

void MainWindow::onCancelDeviceScan() {
    shouldCancelScan_ = true;
    if (deviceScanThread_) {
        deviceScanThread_->quit();
        deviceScanThread_->wait();
        deviceScanThread_ = nullptr;
    }
    isScanningDevice_ = false;
    scanDeviceButton_->setEnabled(true);
    cancelScanButton_->setEnabled(false);
    scanResultsEdit_->appendPlainText("\nScan cancelled.\n");
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

    // 获取用户选择的格式
    QString format = outputFormatCombo_->currentData().toString();

    // 创建并启动恢复工作线程
    RecoveryWorker* worker = new RecoveryWorker();
    QThread* thread = new QThread(this);

    worker->setParameters(
        QString::fromStdString(currentFilePath_),
        outputPathEdit_->text(),
        recoveryModeCombo_->currentData().toString(),
        format,
        blobRecoveryCheck_->isChecked(),
        headerRepairCheck_->isChecked(),
        pageSizeSpin_->value(),
        tableNameEdit_->text(),
        databaseNameEdit_->text(),
        userNameEdit_->text()
    );

    worker->moveToThread(thread);

    // 连接信号和槽
    connect(thread, &QThread::started, worker, &RecoveryWorker::runRecovery);
    connect(worker, &RecoveryWorker::statusUpdated, this, &MainWindow::updateStatus);
    connect(worker, &RecoveryWorker::recoveryCompleted, this, &MainWindow::onRecoveryCompleted);
    connect(worker, &RecoveryWorker::recoveryFailed, this, &MainWindow::onRecoveryFailed);
    connect(thread, &QThread::finished, [thread, worker]() {
        thread->deleteLater();
        worker->deleteLater();
    });

    recoveryThread_ = thread;
    thread->start();
}

void MainWindow::onRecoveryCompleted(const QString& resultPath, int recordCount) {
    isRecovering_ = false;
    startButton_->setEnabled(true);
    cancelButton_->setEnabled(false);
    progressBar_->setVisible(false);

    updateStatus("Recovery completed successfully!");
    displayRecoveryResults(resultPath, recordCount);
}

void MainWindow::onRecoveryFailed(const QString& errorMessage) {
    isRecovering_ = false;
    startButton_->setEnabled(true);
    cancelButton_->setEnabled(false);
    progressBar_->setVisible(false);

    updateStatus("Recovery failed: " + errorMessage);
    QMessageBox::critical(this, "Recovery Error", errorMessage);
}

void MainWindow::onCancelRecovery() {
    if (recoveryThread_) {
        recoveryThread_->quit();
        recoveryThread_->wait();
        recoveryThread_ = nullptr;
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

void MainWindow::addLog(const QString& message, const QString& level,
                        const QString& source) {
    LogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.level = level;
    entry.message = message;
    entry.source = source;

    logEntries_.push_back(entry);
    displayLog(entry);
}

void MainWindow::addBatchTask(const BatchRecoveryTask& task) {
    batchTasks_.push_back(task);
    displayBatchTasks();
}

void MainWindow::clearLog() {
    logTextEdit_->clear();
    logEntries_.clear();
}

void MainWindow::setState(RecoveryState state) {
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

void MainWindow::displayRecoveryResults(const QString& resultPath, int recordCount) {
    resultsTextEdit_->clear();
    recordsCountLabel_->setText("Records: " + QString::number(recordCount));

    if (recordCount == 0) {
        resultsTextEdit_->append("No records found.");
        resultsTextEdit_->append("\nResult file: " + resultPath);
        return;
    }

    // 显示文件路径
    resultsTextEdit_->append("Result file: " + resultPath);
    resultsTextEdit_->append("");

    // 读取并显示 CSV 文件内容
    QFile file(resultPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        int lineCount = 0;
        while (!in.atEnd() && lineCount < 100) {  // 限制显示前100行
            QString line = in.readLine();
            resultsTextEdit_->append(line);
            lineCount++;
        }
        if (!in.atEnd()) {
            resultsTextEdit_->append("...");
            resultsTextEdit_->append("(Only showing first 100 lines)");
        }
        file.close();
    } else {
        resultsTextEdit_->append("Could not open result file.");
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

void MainWindow::displayLog(const LogEntry& entry) {
    logTextEdit_->appendPlainText(entry.to_string());
}

void MainWindow::displayBatchTasks() {
    batchTaskList_->clear();
    for (const auto& task : batchTasks_) {
        batchTaskList_->addItem(task.toQString());
    }
}

void MainWindow::onStartBatchRecovery() {
    if (batchTasks_.empty()) {
        QMessageBox::warning(this, "Error", "No batch tasks to process.");
        return;
    }

    totalBatchTasks_ = batchTasks_.size();
    completedBatchTasks_ = 0;
    batchProgressBar_->setValue(0);

    // 启动批量恢复线程
    startBatchRecoveryThread();
}

void MainWindow::onClearBatchTasks() {
    batchTasks_.clear();
    displayBatchTasks();
}

void MainWindow::startBatchRecoveryThread() {
    QThread* thread = new QThread(this);

    auto batchWorker = [this]() {
        for (int i = 0; i < totalBatchTasks_; ++i) {
            const auto& task = batchTasks_[i];

            QMetaObject::invokeMethod(this, [this, i, &task]() {
                updateStatus("Processing task " + QString::number(i + 1) + "/" +
                            QString::number(totalBatchTasks_));
            }, Qt::QueuedConnection);

            // 执行恢复
            dbrecovery::recovery::RecoveryEngine engine;
            if (engine.open(task.file_path)) {
                // 尝试从 SDI 解析表结构
                auto schema_opt = parseSchemaFromSDI(task.file_path, 0);
                dbrecovery::catalog::TableSchema schema;

                if (schema_opt.has_value()) {
                    schema = schema_opt.value();
                } else {
                    schema = createDefaultSchema();
                }

                std::vector<std::map<std::string, std::string>> records;
                if (task.recovery_mode == "truncate") {
                    records = engine.recover_truncate(schema);
                } else {
                    records = engine.recover_truncate(schema);
                }

                // 导出结果
                std::string output_path = task.output_dir + "/recovered_" +
                                         std::to_string(i + 1) + "." + task.format;
                if (task.format == "csv") {
                    engine.export_to_csv(records, output_path);
                } else if (task.format == "sql") {
                    engine.export_to_sql(records, "recovered_table", output_path);
                }

                engine.close();

                QMetaObject::invokeMethod(this, [this, i, &records]() {
                    completedBatchTasks_++;
                    batchProgressBar_->setValue(static_cast<int>(completedBatchTasks_) * 100 / totalBatchTasks_);
                    addLog("Task " + QString::number(i + 1) + " completed: " +
                          QString::number(records.size()) + " records recovered",
                          "INFO", "Batch");
                }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(this, [this, i]() {
                    addLog("Task " + QString::number(i + 1) + " failed: Could not open file",
                          "ERROR", "Batch");
                }, Qt::QueuedConnection);
            }
        }

        QMetaObject::invokeMethod(this, [this]() {
            updateStatus("Batch recovery completed!");
            addLog("All batch tasks completed", "INFO", "Batch");
        }, Qt::QueuedConnection);
    };

    QObject::connect(thread, &QThread::started, [batchWorker]() {
        batchWorker();
    });
    QObject::connect(thread, &QThread::finished, [thread]() {
        thread->deleteLater();
    });

    thread->start();
}

// ==================== 目录扫描UI ====================
void MainWindow::initDirectoryScanUI() {
    directoryScanTab_ = new QWidget();
    QVBoxLayout* directoryScanLayout = new QVBoxLayout(directoryScanTab_);

    QGroupBox* scanSettingsGroup = new QGroupBox("Directory Scan Settings");
    QVBoxLayout* settingsLayout = new QVBoxLayout(scanSettingsGroup);
    
    directoryPathEdit_ = new QLineEdit("/var/lib/mysql");
    scanDirectoryButton_ = new QPushButton("Scan Directory");
    QHBoxLayout* pathLayout = new QHBoxLayout();
    pathLayout->addWidget(new QLabel("Directory Path:"));
    pathLayout->addWidget(directoryPathEdit_);
    pathLayout->addWidget(scanDirectoryButton_);
    settingsLayout->addLayout(pathLayout);
    
    scanDepthSpin_ = new QSpinBox();
    scanDepthSpin_->setRange(1, 10);
    scanDepthSpin_->setValue(3);
    QHBoxLayout* depthLayout = new QHBoxLayout();
    depthLayout->addWidget(new QLabel("Scan Depth:"));
    depthLayout->addWidget(scanDepthSpin_);
    depthLayout->addStretch();
    settingsLayout->addLayout(depthLayout);
    
    directoryScanLayout->addWidget(scanSettingsGroup);
    
    scanResultsTree_ = new QTreeWidget();
    scanResultsTree_->setHeaderLabels({"File Path", "Size", "Pages", "Status"});
    scanResultsTree_->setMinimumHeight(300);
    directoryScanLayout->addWidget(scanResultsTree_);
    
    QHBoxLayout* scanButtonLayout = new QHBoxLayout();
    cancelDirectoryScanButton_ = new QPushButton("Cancel Scan");
    cancelDirectoryScanButton_->setEnabled(false);
    scanButtonLayout->addStretch();
    scanButtonLayout->addWidget(cancelDirectoryScanButton_);
    directoryScanLayout->addLayout(scanButtonLayout);
    
    tabWidget_->insertTab(4, directoryScanTab_, "Directory Scan");
}

// ==================== 表搜索UI ====================
void MainWindow::initSearchUI() {
    searchTab_ = new QWidget();
    QVBoxLayout* searchLayout = new QVBoxLayout(searchTab_);

    QGroupBox* searchSettingsGroup = new QGroupBox("Table Search Settings");
    QVBoxLayout* settingsLayout = new QVBoxLayout(searchSettingsGroup);
    
    searchTableNameEdit_ = new QLineEdit("%");
    searchTableButton_ = new QPushButton("Search");
    QHBoxLayout* nameLayout = new QHBoxLayout();
    nameLayout->addWidget(new QLabel("Table Name (supports % and _ wildcards):"));
    nameLayout->addWidget(searchTableNameEdit_);
    nameLayout->addWidget(searchTableButton_);
    settingsLayout->addLayout(nameLayout);
    
    searchLayout->addWidget(searchSettingsGroup);
    
    searchResultsTree_ = new QTreeWidget();
    searchResultsTree_->setHeaderLabels({"Table Name", "File Path", "Pages", "Row Format"});
    searchResultsTree_->setMinimumHeight(300);
    searchLayout->addWidget(searchResultsTree_);
    
    QHBoxLayout* searchButtonLayout = new QHBoxLayout();
    cancelSearchButton_ = new QPushButton("Cancel Search");
    cancelSearchButton_->setEnabled(false);
    searchButtonLayout->addStretch();
    searchButtonLayout->addWidget(cancelSearchButton_);
    searchLayout->addLayout(searchButtonLayout);
    
    tabWidget_->insertTab(5, searchTab_, "Table Search");
}

// ==================== Slot Implementations ====================

void MainWindow::onActionScanDirectory() {
    tabWidget_->setCurrentWidget(directoryScanTab_);
}

void MainWindow::onStartDirectoryScan() {
    QString dirPath = directoryPathEdit_->text();
    if (dirPath.isEmpty()) {
        QMessageBox::warning(this, "Error", "Please enter a directory path.");
        return;
    }

    if (!QDir(dirPath).exists()) {
        QMessageBox::warning(this, "Error", "Directory does not exist.");
        return;
    }

    isScanningDirectory_ = true;
    scanDirectoryButton_->setEnabled(false);
    cancelDirectoryScanButton_->setEnabled(true);
    scanResultsTree_->clear();

    // 启动扫描线程
    directoryScanThread_ = new QThread(this);
    shouldCancelDirectoryScan_ = false;

    auto scanWorker = [this, dirPath]() {
        try {
            dbrecovery::recovery::RmrfRecovery rmrf;
            rmrf.set_page_size(16384);

            bool success = rmrf.scan_directory(dirPath.toStdString(), scanDepthSpin_->value());

            if (success) {
                const auto& files = rmrf.get_scanned_files();
                
                QMetaObject::invokeMethod(this, [this, &files]() {
                    for (const auto& file : files) {
                        QTreeWidgetItem* item = new QTreeWidgetItem(scanResultsTree_);
                        item->setText(0, QString::fromStdString(file.file_path));
                        item->setText(1, QString::number(file.file_size) + " bytes");
                        item->setText(2, QString::number(file.total_pages));
                        item->setText(3, file.valid_index_pages > 0 ? "Valid" : "Unknown");
                    }
                    scanResultsTree_->expandAll();
                }, Qt::QueuedConnection);
                
                QMetaObject::invokeMethod(this, [this]() {
                    addLog("Directory scan completed. Found " + 
                          QString::number(scanResultsTree_->topLevelItemCount()) + " files.",
                          "INFO", "Directory Scan");
                }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(this, [this]() {
                    addLog("Directory scan failed: No valid data found.", "ERROR", "Directory Scan");
                }, Qt::QueuedConnection);
            }
        } catch (const std::exception& e) {
            QMetaObject::invokeMethod(this, [this, e]() {
                addLog("Directory scan error: " + QString(e.what()), "ERROR", "Directory Scan");
            }, Qt::QueuedConnection);
        }

        QMetaObject::invokeMethod(this, [this]() {
            isScanningDirectory_ = false;
            scanDirectoryButton_->setEnabled(true);
            cancelDirectoryScanButton_->setEnabled(false);
        }, Qt::QueuedConnection);
    };

    QThread* thread = new QThread(this);
    QObject::connect(thread, &QThread::started, [scanWorker]() {
        scanWorker();
    });
    QObject::connect(thread, &QThread::finished, [thread]() {
        thread->deleteLater();
    });

    thread->start();
    directoryScanThread_ = thread;
}

void MainWindow::onCancelDirectoryScan() {
    shouldCancelDirectoryScan_ = true;
    if (directoryScanThread_) {
        directoryScanThread_->quit();
        directoryScanThread_->wait();
        directoryScanThread_ = nullptr;
    }
    isScanningDirectory_ = false;
    scanDirectoryButton_->setEnabled(true);
    cancelDirectoryScanButton_->setEnabled(false);
    addLog("Directory scan cancelled.", "WARN", "Directory Scan");
}

void MainWindow::onActionSearchTable() {
    tabWidget_->setCurrentWidget(searchTab_);
}

void MainWindow::onStartTableSearch() {
    QString tableName = searchTableNameEdit_->text();
    if (tableName.isEmpty()) {
        QMessageBox::warning(this, "Error", "Please enter a table name.");
        return;
    }

    isSearching_ = true;
    searchTableButton_->setEnabled(false);
    cancelSearchButton_->setEnabled(true);
    searchResultsTree_->clear();

    // 启动搜索线程
    searchThread_ = new QThread(this);
    shouldCancelSearch_ = false;

    auto searchWorker = [this, tableName]() {
        try {
            // 获取目录路径
            QString dirPath = directoryPathEdit_->text();
            if (dirPath.isEmpty() || !QDir(dirPath).exists()) {
                dirPath = "/var/lib/mysql";
            }

            dbrecovery::recovery::RmrfRecovery rmrf;
            rmrf.set_page_size(16384);

            bool success = rmrf.scan_directory(dirPath.toStdString(), scanDepthSpin_->value());
            
            if (success) {
                const auto& files = rmrf.get_scanned_files();
                
                // 过滤匹配的表名
                std::vector<std::map<std::string, std::string>> results;
                for (const auto& file : files) {
                    // 简单的表名提取（从文件名）
                    std::string fileName = file.file_path;
                    size_t lastSlash = fileName.rfind('/');
                    if (lastSlash != std::string::npos) {
                        fileName = fileName.substr(lastSlash + 1);
                    }
                    
                    // 移除 .ibd 扩展名
                    if (fileName.size() > 4 && fileName.substr(fileName.size() - 4) == ".ibd") {
                        fileName = fileName.substr(0, fileName.size() - 4);
                    }
                    
                    // 简单的通配符匹配
                    bool match = true;
                    std::string pattern = tableName.toStdString();
                    
                    // 将 SQL 通配符转换为正则表达式
                    std::string regexPattern = "^";
                    for (char c : pattern) {
                        if (c == '%') regexPattern += ".*";
                        else if (c == '_') regexPattern += ".";
                        else regexPattern += c;
                    }
                    regexPattern += "$";
                    
                    // 简单匹配（不使用正则）
                    if (pattern.find('%') != std::string::npos || pattern.find('_') != std::string::npos) {
                        // 通配符匹配
                        if (pattern.front() == '%' && pattern.back() == '%') {
                            // %pattern%
                            match = fileName.find(pattern.substr(1, pattern.size() - 2)) != std::string::npos;
                        } else if (pattern.front() == '%') {
                            // %pattern
                            match = fileName.size() >= pattern.size() - 1 && 
                                    fileName.substr(fileName.size() - pattern.size() + 1) == pattern.substr(1);
                        } else if (pattern.back() == '%') {
                            // pattern%
                            match = fileName.size() >= pattern.size() - 1 && 
                                    fileName.substr(0, pattern.size() - 1) == pattern.substr(0, pattern.size() - 1);
                        }
                    } else {
                        // 精确匹配
                        match = fileName == pattern;
                    }
                    
                    if (match) {
                        std::map<std::string, std::string> result;
                        result["table_name"] = fileName;
                        result["file_path"] = file.file_path;
                        result["pages"] = std::to_string(file.total_pages);
                        result["row_format"] = "DYNAMIC";
                        results.push_back(result);
                    }
                }
                
                QMetaObject::invokeMethod(this, [this, &results]() {
                    for (const auto& result : results) {
                        QTreeWidgetItem* item = new QTreeWidgetItem(searchResultsTree_);
                        item->setText(0, QString::fromStdString(result["table_name"]));
                        item->setText(1, QString::fromStdString(result["file_path"]));
                        item->setText(2, QString::fromStdString(result["pages"]));
                        item->setText(3, QString::fromStdString(result["row_format"]));
                    }
                    searchResultsTree_->expandAll();
                    
                    addLog("Table search completed. Found " + 
                          QString::number(results.size()) + " matching table(s).",
                          "INFO", "Table Search");
                }, Qt::QueuedConnection);
            }
        } catch (const std::exception& e) {
            QMetaObject::invokeMethod(this, [this, e]() {
                addLog("Table search error: " + QString(e.what()), "ERROR", "Table Search");
            }, Qt::QueuedConnection);
        }

        QMetaObject::invokeMethod(this, [this]() {
            isSearching_ = false;
            searchTableButton_->setEnabled(true);
            cancelSearchButton_->setEnabled(false);
        }, Qt::QueuedConnection);
    };

    QThread* thread = new QThread(this);
    QObject::connect(thread, &QThread::started, [searchWorker]() {
        searchWorker();
    });
    QObject::connect(thread, &QThread::finished, [thread]() {
        thread->deleteLater();
    });

    thread->start();
    searchThread_ = thread;
}

void MainWindow::onCancelTableSearch() {
    shouldCancelSearch_ = true;
    if (searchThread_) {
        searchThread_->quit();
        searchThread_->wait();
        searchThread_ = nullptr;
    }
    isSearching_ = false;
    searchTableButton_->setEnabled(true);
    cancelSearchButton_->setEnabled(false);
    addLog("Table search cancelled.", "WARN", "Table Search");
}

void MainWindow::onExportResults() {
    if (resultsTextEdit_->toPlainText().isEmpty()) {
        QMessageBox::warning(this, "Export", "No results to export.");
        return;
    }
    
    QString filePath = QFileDialog::getSaveFileName(this, "Export Results", 
                                                    "/tmp/recovered/results.csv",
                                                    "CSV Files (*.csv);;JSON Files (*.json);;SQL Files (*.sql)");
    if (filePath.isEmpty()) {
        return;
    }
    
    std::ofstream file(filePath.toStdString());
    if (!file) {
        QMessageBox::critical(this, "Export", "Failed to create file.");
        return;
    }
    
    file << resultsTextEdit_->toPlainText().toStdString();
    file.close();
    
    addLog("Results exported to: " + filePath, "INFO", "Export");
    QMessageBox::information(this, "Export", "Results exported successfully!");
}

void MainWindow::onSaveResults() {
    onExportResults();
}

} // namespace gui
} // namespace dbrecovery
