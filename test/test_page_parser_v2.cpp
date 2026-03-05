#include "dbrecovery/innodb/v2/page_parser.hpp"
#include "dbrecovery/innodb/v2/record_parser.hpp"
#include <fstream>
#include <iostream>

int main() {
    std::ifstream ifs("/app/mysql8_recovery/test_data/sample.ibd", std::ios::binary);
    if (!ifs) {
        std::cerr << "⚠️ 未找到测试样本\n";
        return 0;
    }

    char buffer[16384];
    ifs.read(buffer, sizeof(buffer));

    DBRecovery::InnoDB::v2::PageParser parser(
        reinterpret_cast<const uint8_t*>(buffer), 
        sizeof(buffer)
    );

    if (!parser.parse()) {
        std::cerr << "❌ 页解析失败\n";
        return 1;
    }

    std::cout << "✅ 页类型: " 
              << (parser.type() == DBRecovery::InnoDB::v2::PageType::FIL_PAGE_INDEX ? "B+Tree Index" : "Other")
              << "\n";

    // 解析记录
    DBRecovery::InnoDB::v2::RecordParser rec_parser(parser);
    auto records = rec_parser.parse();
    
    std::cout << "✅ 记录数量: " << records.size() << "\n";
    for (size_t i = 0; i < records.size(); ++i) {
        std::cout << "   Record[" << i << "] offset=" << records[i].offset << "\n";
    }

    return 0;
}
