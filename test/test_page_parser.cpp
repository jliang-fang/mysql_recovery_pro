#include <iostream>
#include <fstream>
#include <cstring>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"

int main() {
    // 生成一个最小合法 InnoDB 页（512字节，无数据）
    std::ofstream ofs("/tmp/test_page.ibd", std::ios::binary);
    if (!ofs) { std::cerr << "Failed to create test page\n"; return 1; }

    char zero[16384] = {};
    // 设置页头关键字段（偏移0-37）
    uint16_t page_level = 0;
    uint16_t n_dir_slots = 2;
    std::memcpy(zero + 36, &page_level, sizeof(page_level));
    std::memcpy(zero + 38, &n_dir_slots, sizeof(n_dir_slots));

    // 设置页尾校验（最后8字节为 PAGE_END_LSN）
    uint64_t lsn = 0;
    std::memcpy(zero + 16376, &lsn, sizeof(lsn));

    ofs.write(zero, 16384);
    ofs.close();

    // 读取并解析
    dbrecovery::BinaryReader reader("/tmp/test_page.ibd");
    if (!reader.open()) { std::cerr << "Failed to open test page\n"; return 1; }

    auto header = dbrecovery::InnodbPageHeader::parse(&reader, 0);
    if (header.page_level != 0) {
        std::cerr << "FAIL: Expected page_level=0, got " << header.page_level << "\n";
        return 1;
    }
    if (header.n_dir_slots != 2) {
        std::cerr << "FAIL: Expected n_dir_slots=2, got " << header.n_dir_slots << "\n";
        return 1;
    }

    std::cout << "PASS: Page parser test\n";
    return 0;
}
