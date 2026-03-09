#include <iostream>
#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/utils/cli_parser.hpp"

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
    FILE* f = fopen(ibd_path.c_str(), "rb");
    if (!f) {
        std::cerr << "[ERROR] Cannot open file: " << ibd_path << "\n";
        return 1;
    }
    fclose(f);
    
    // Run recovery
    dbrecovery::innodb::RecoveryEngine engine;
    if (!engine.recover_table(ibd_path, table_id)) {
        std::cerr << "[ERROR] Table recovery failed\n";
        return 1;
    }
    
    std::cout << "✅ Recovery completed: " << output_dir << "\n";
    return 0;
}
