#include <iostream>
#include <fstream>
#include <filesystem>
#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/utils/cli_parser.hpp"
#include "dbrecovery/utils/math_helper.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::cout << "MySQL 8.0 Recovery Engine v1.0.0 (jliang.fang)\n";
    
    // ✅ 统一使用 C++17 兼容的预计算表（项目要求 C++17 标准）
    uint32_t n = 5;
    constexpr std::array<uint64_t, 21> factorial_table = []() noexcept {
        std::array<uint64_t, 21> table{};
        table[0] = 1;
        for (uint32_t i = 1; i <= 20; ++i) {
            table[i] = table[i-1] * i;
        }
        return table;
    }();
    std::cout << "Factorial of " << n << " is: " << factorial_table[n] << std::endl;

    dbrecovery::utils::CliParser parser(argc, argv);
    
    // Parse arguments
    std::string ibd_path = parser.get_option("ibd", "");
    std::string output_dir = parser.get_option("output", "/tmp/recovered");
    uint64_t table_id = 0;
    
    if (ibd_path.empty()) {
        std::cerr << "Usage: " << argv[0] << " --ibd=<path_to_ibd_file> [--output=<dir>] [--table-id=<id>]" << std::endl;
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
