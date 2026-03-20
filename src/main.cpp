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
    // 已在 utils/math_helper.hpp 中实现，此处直接调用 utils::factorial()
    dbrecovery::utils::CliParser parser(argc, argv);
    
    // Parse arguments
    std::string ibd_path = parser.get_option("ibd", "");
    std::string output_dir = parser.get_option("output", "/tmp/recovered");
    uint64_t table_id = 0;
    
    if (ibd_path.empty()) {
        std::cerr << "Usage: " << argv[0] << " --ibd=<path_to_ibd_file> [--output=<dir>] [--table-id=<id>]" << std::endl;
        return 1;
    }

    // ✅ 新增：测试阶乘函数功能
    uint32_t n = 5;
    std::cout << "Factorial of " << n << " is: " << dbrecovery::utils::factorial(n) << std::endl;
    
    // 验证预计算结果（用于 CI/CD 测试）
    if (dbrecovery::utils::factorial(5) != 120 || dbrecovery::utils::factorial(10) != 3628800) {
        std::cerr << "[ERROR] Factorial function verification failed" << std::endl;
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
