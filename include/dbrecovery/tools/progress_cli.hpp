#ifndef DBRECOVERY_TOOLS_PROGRESS_CLI_HPP
#define DBRECOVERY_TOOLS_PROGRESS_CLI_HPP

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <optional>
#include <sstream>
#include <atomic>
#include <mutex>

#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/page_chain_rebuilder.hpp"
#include "dbrecovery/innodb/sdi_extractor.hpp"
#include "dbrecovery/innodb/btree_node_parser.hpp"
#include "dbrecovery/innodb/record_chain_rebuilder.hpp"
#include "dbrecovery/innodb/blob_chain_rebuilder.hpp"
#include "dbrecovery/utils/cli_parser.hpp"
#include "dbrecovery/recovery/engine.hpp"
#include "dbrecovery/recovery/parallel_engine.hpp"
#include "dbrecovery/recovery/rmrf_recovery.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/repair/header_repairer.hpp"

namespace fs = std::filesystem;

namespace dbrecovery {
namespace tools {

// 进度条类
class ProgressBar {
public:
    explicit ProgressBar(size_t width = 50) : width_(width), current_(0), total_(0), started_(false) {}

    void start(size_t total) {
        std::lock_guard<std::mutex> lock(mutex_);
        total_ = total;
        current_ = 0;
        started_ = true;
        update(0);
    }

    void update(size_t current) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = current;
        draw();
    }

    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            // Fill remaining
            size_t filled = width_;
            std::cout << "\r[";
            for (size_t i = 0; i < filled; ++i) {
                std::cout << "█";
            }
            for (size_t i = filled; i < width_; ++i) {
                std::cout << " ";
            }
            std::cout << "] 100% Complete\n";
            started_ = false;
        }
    }

    void set_total(size_t total) {
        std::lock_guard<std::mutex> lock(mutex_);
        total_ = total;
    }

private:
    void draw() {
        if (!started_ || total_ == 0) return;

        size_t filled = static_cast<size_t>((static_cast<double>(current_) / total_) * width_);
        
        std::cout << "\r[";
        for (size_t i = 0; i < filled; ++i) {
            std::cout << "█";
        }
        for (size_t i = filled; i < width_; ++i) {
            std::cout << "░";
        }
        double percent = static_cast<double>(current_) / total_ * 100.0;
        std::cout << "] " << std::fixed << std::setw(5) << std::setprecision(1) << percent << "%";
        std::cout.flush();
    }

    size_t width_;
    size_t current_;
    size_t total_;
    bool started_;
    std::mutex mutex_;
};

// 增强版 CLI - 支持实时进度显示
class ProgressRecoveryCLI {
public:
    explicit ProgressRecoveryCLI(int argc, char** argv);

    int run();

    void show_help(const char* program_name);
    void show_version();

private:
    // 恢复统计
    struct RecoveryStats {
        uint64_t total_pages_scanned = 0;
        uint64_t valid_pages = 0;
        uint64_t index_pages = 0;
        uint64_t sdi_pages = 0;
        uint64_t undo_pages = 0;
        uint64_t blob_pages = 0;
        uint64_t records_recovered = 0;
        uint64_t chains_fixed = 0;
        uint64_t orphan_pages = 0;
        uint64_t sdi_extracted = 0;
        uint64_t blob_data_bytes = 0;
        double elapsed_seconds = 0.0;
    };

    // 参数解析
    bool parse_arguments();
    
    // 恢复操作
    bool run_truncate_recovery();
    bool run_parallel_recovery();
    bool run_rmrf_recovery();
    bool run_header_repair();
    bool run_scan();
    
    // 导出操作
    bool export_to_csv(const std::string& path, 
                      const std::vector<std::map<std::string, std::string>>& records);
    bool export_to_sql(const std::string& path,
                      const std::vector<std::map<std::string, std::string>>& records,
                      const std::string& table_name);
    bool export_to_json(const std::string& path,
                       const std::vector<std::map<std::string, std::string>>& records);

    // 辅助函数
    std::optional<catalog::TableSchema> parse_schema_from_sdi(const std::string& path, uint64_t table_id);
    catalog::TableSchema create_default_schema();
    catalog::TableSchema create_mysql56_test_schema();

private:
    int argc_;
    char** argv_;
    utils::CliParser parser_;

    // 命令行参数
    std::string ibd_path_;
    std::string output_dir_;
    std::string format_;
    uint32_t page_size_;
    int mysql_version_;
    std::string row_format_;
    std::string page_type_;
    
    // TDE 支持
    std::string key_hex_;
    std::string table_key_hex_;
    std::string keyring_path_;
    uint32_t tablespace_id_;
    bool detect_encryption_;
    bool try_no_key_;
    
    // MySQL 版本检测
    bool detect_version_;
    
    // 恢复模式
    bool truncate_recovery_;
    bool rmrf_recovery_;
    bool blob_recovery_;
    bool header_repair_;
    bool scan_only_;
    
    // 并行选项
    bool parallel_;
    size_t thread_count_;
    
    // 进度条
    ProgressBar progress_bar_;
    
    // 统计
    RecoveryStats stats_;
};

} // namespace tools
} // namespace dbrecovery

#endif // DBRECOVERY_TOOLS_PROGRESS_CLI_HPP
