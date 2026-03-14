#include <iostream>
#include <fstream>
#include <filesystem>
#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/utils/cli_parser.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::cout << "MySQL 8.0 Recovery Engine v1.0.0 (jliang.fang)\n";
    
    dbrecovery::utils::CliParser parser(argc, argv);
    
    // Parse arguments
    std::string ibd_path = parser.get_option("ibd", "");
    std::string output_dir = parser.get_option("output", "/tmp/recovered");
    uint64_t table_id = 0;
    
    try {
        table_id = std::stoull(parser.get_option("table-id", "0"));
    } catch (...) {
        std::cerr << "[WARN] Invalid table-id, using default\n";
    }
    
    if (ibd_path.empty()) {
        std::cout << "Usage: mysql8_recover --ibd=/path/to/table.ibd [--output=/tmp/recovered] [--table-id=42]\n";
        return 1;
    }
    
    // 检查文件是否存在
    std::ifstream f(ibd_path);
    if (!f.good()) {
        std::cerr << "[ERROR] Cannot open file: " << ibd_path << "\n";
        return 1;
    }
    f.close();
    
    // 创建输出目录
    try {
        fs::create_directories(output_dir);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create output directory: " << output_dir 
                  << ", error: " << e.what() << "\n";
        return 1;
    }
    
    // Run recovery
    dbrecovery::innodb::RecoveryEngine engine;
    if (!engine.recover_table(ibd_path, table_id)) {
        std::cerr << "[ERROR] Table recovery failed\n";
        return 1;
    }
    
    std::cout << "✅ Recovery completed: " << output_dir << "\n";
    return 0;
}
