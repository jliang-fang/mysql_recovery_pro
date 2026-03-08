#include <iostream>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"

// 测试 NULL 位图边界条件
bool test_null_bitmap() {
    // 创建一个包含 NULL 位图的页（假设 3 列可为空）
    char page[16384] = {};
    
    // 设置 NULL 位图：第0列和第2列为NULL（位模式：101 -> 字节值 0x05）
    page[38] = 0x05; // 第38字节开始是页头，这里模拟NULL位图位置
    
    try {
        dbrecovery::RowDecoder decoder(reinterpret_cast<uint8_t*>(page), 1); // 1字节可表示8列
        
        if (!decoder.is_null(0)) { std::cerr << "FAIL: Column 0 should be NULL\n"; return false; }
        if (decoder.is_null(1)) { std::cerr << "FAIL: Column 1 should NOT be NULL\n"; return false; }
        if (!decoder.is_null(2)) { std::cerr << "FAIL: Column 2 should be NULL\n"; return false; }
        
        // 测试越界访问
        if (decoder.is_null(8)) { std::cerr << "FAIL: Column 8 should be out of range\n"; return false; }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: Exception during null bitmap test: " << e.what() << "\n";
        return false;
    }
    
    return true;
}

// 测试 BLOB 校验和验证
bool test_blob_checksum() {
    // 创建一个模拟的BLOB页（16KB）
    char blob_page[16384] = {};
    std::memset(blob_page, 0xFF, sizeof(blob_page));
    
    dbrecovery::BlobPageChain chain;
    chain.add_page(0, 100);
    
    // 验证空数据应失败
    if (chain.verify_page_checksum(nullptr, 16384)) {
        std::cerr << "FAIL: Null pointer should fail checksum\n";
        return false;
    }
    
    // 验证小尺寸数据应失败
    if (chain.verify_page_checksum(reinterpret_cast<uint8_t*>(blob_page), 1024)) {
        std::cerr << "FAIL: Small page size should fail checksum\n";
        return false;
    }
    
    return true;
}

int main() {
    // 原始页解析测试
    {
        std::ofstream ofs("/tmp/test_page.ibd", std::ios::binary);
        if (!ofs) { std::cerr << "Failed to create test page\n"; return 1; }

        char zero[16384] = {};
        uint16_t page_level = 0;
        uint16_t n_dir_slots = 2;
        std::memcpy(zero + 36, &page_level, sizeof(page_level));
        std::memcpy(zero + 38, &n_dir_slots, sizeof(n_dir_slots));

        uint64_t lsn = 0;
        std::memcpy(zero + 16376, &lsn, sizeof(lsn));

        ofs.write(zero, 16384);
        ofs.close();

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
    }

    // 新增测试
    if (!test_null_bitmap()) { return 1; }
    std::cout << "PASS: NULL bitmap boundary test\n";

    if (!test_blob_checksum()) { return 1; }
    std::cout << "PASS: BLOB checksum validation test\n";

    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
