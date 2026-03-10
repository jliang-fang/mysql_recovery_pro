#include "dbrecovery/innodb/v2/page_parser.hpp"
#include "dbrecovery/innodb/v2/record_parser.hpp"
#include <fstream>
#include <iostream>

int main() {
    std::ifstream ifs("/app/mysql8_recovery/test_data/sample.ibd", std::ios::binary);
    if (!ifs) {
        std::cerr << "[WARN] 未找到测试样本 /app/mysql8_recovery/test_data/sample.ibd\n";
        std::cerr << "[INFO] 使用 tools/ibd_generator.py 生成测试数据: python3 tools/ibd_generator.py test_table\n";
        return 0;
    }

    char buffer[16384];
    ifs.read(buffer, sizeof(buffer));
    
    if (ifs.gcount() < 16384) {
        std::cerr << "[WARN] 文件大小不足 16KB，可能不是有效的 InnoDB 页\n";
        return 0;
    }

    DBRecovery::InnoDB::v2::PageParser parser(
        reinterpret_cast<const uint8_t*>(buffer), 
        sizeof(buffer)
    );

    if (!parser.parse()) {
        std::cerr << "[ERROR] 页解析失败\n";
        return 1;
    }

    std::cout << "[INFO] 页类型: " 
              << (parser.type() == DBRecovery::InnoDB::v2::PageType::FIL_PAGE_INDEX ? "B+Tree Index" : 
                  parser.type() == DBRecovery::InnoDB::v2::PageType::FIL_PAGE_SDI ? "SDI Metadata" :
                  parser.type() == DBRecovery::InnoDB::v2::PageType::FIL_PAGE_TYPE_BLOB ? "BLOB Data" :
                  "Other")
              << "\n";

    // 解析记录
    DBRecovery::InnoDB::v2::RecordParser rec_parser(parser);
    auto records = rec_parser.parse();
    
    std::cout << "[INFO] 记录数量: " << records.size() << "\n";
    for (size_t i = 0; i < records.size(); ++i) {
        std::cout << "   Record[" << i << "] offset=" << records[i].offset 
                  << ", is_deleted=" << records[i].is_deleted << "\n";
    }

    return 0;
}
