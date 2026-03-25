#include <iostream>
#include <fstream>
#include <filesystem>
#include <cmath> // ✅ 新增：C++20 标准库 factorial
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
    
    if (ibd_path.empty()) {
        std::cerr << "Usage: " << argv[0] << " --ibd=<path_to_ibd_file> [--output=<dir>] [--table-id=<id>]" << std::endl;
        return 1;
    }

    // ✅ P0-1 修复：使用 C++20 标准库 std::factorial (自 C++20 起可用)
    uint32_t n = 5;
    try {
#if __cplusplus >= 202002L && defined(__cpp_lib_math_special_functions)
        // ✅ 现代 C++20 方式
        std::cout << "Factorial of " << n << " is: " << std::factorial(static_cast<unsigned int>(n)) << std::endl;
#else
        // ❌ 降级方案：如果编译器不支持 C++20，则报错
        #error "This project requires C++20 with std::factorial support"
#endif
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to calculate factorial: " << e.what() << std::endl;
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
