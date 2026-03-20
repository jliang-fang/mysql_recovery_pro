#include "dbrecovery/tests/test_utils.hpp"
#include "dbrecovery/utils/file_mapper.hpp"
#include <fstream>
#include <random>
#include <cstring>

namespace dbrecovery {
namespace tests {

// ✅ 新增：测试 C++20 std::factorial 与预计算表的一致性
TEST(UtilsTest, FactorialConsistency) {
#if __cplusplus >= 202100L
    for (uint32_t i = 0; i <= 20; ++i) {
        EXPECT_EQ(static_cast<uint64_t>(std::factorial(i)), 
                  []() noexcept { std::array<uint64_t, 21> t{}; t[0]=1; for(uint32_t j=1;j<=20;++j)t[j]=t[j-1]*j; return t; }()[i]);
    }
#else
    constexpr std::array<uint64_t, 21> table = []() noexcept {
        std::array<uint64_t, 21> t{};
        t[0] = 1;
        for (uint32_t i = 1; i <= 20; ++i) t[i] = t[i-1] * i;
        return t;
    }();
    EXPECT_EQ(table[5], 120);
    EXPECT_EQ(table[10], 3628800);
#endif
}

// ✅ 注意：get_factorial() 已废弃，不应在此处使用。如需测试阶乘，请直接内联计算。

std::vector<uint8_t> load_page_from_ibd(const std::string& ibd_path, uint32_t page_no) {
    utils::FileMapper mapper(ibd_path);
    if (!mapper.is_valid()) {
        return {};
    }

    constexpr size_t PAGE_SIZE = 16384;
    uint64_t offset = static_cast<uint64_t>(page_no) * PAGE_SIZE;

    if (offset + PAGE_SIZE > mapper.size()) {
        return {};
    }

    std::vector<uint8_t> page(PAGE_SIZE);
    std::memcpy(page.data(), mapper.data() + offset, PAGE_SIZE);
    return page;
}

std::string create_test_ibd_file() {
    // 创建一个包含 2 个页的最小 .ibd 文件
    constexpr size_t PAGE_SIZE = 16384;
    std::vector<uint8_t> ibd_data(PAGE_SIZE * 2, 0);

    // Page 0: FIL Header (38 bytes)
    // FIL_PAGE_TYPE (offset 0) = FIL_PAGE_INDEX (9969)
    ibd_data[0] = 0x11; ibd_data[1] = 0x27; // little-endian 9969
    // FIL_PAGE_OFFSET (offset 4) = 0
    // ... 其他字段留零（测试用）

    // Page 1: 简单的索引页头
    ibd_data[PAGE_SIZE + 38] = 0x00; // PAGE_LEVEL = 0 (leaf)
    ibd_data[PAGE_SIZE + 42] = 0x04; // PAGE_N_HEAP = 4

    std::string filename = "/tmp/test_recovery.ibd";
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) {
        return "";
    }
    ofs.write(reinterpret_cast<const char*>(ibd_data.data()), ibd_data.size());
    
    return filename;
}

void assert_checksum_equal(uint32_t expected, uint32_t actual, const std::string& msg) {
    if (expected != actual) {
        fprintf(stderr, "Checksum mismatch: %s\nExpected: 0x%08X, Actual: 0x%08X\n", 
                msg.c_str(), expected, actual);
        FAIL() << msg;
    }
}

} // namespace tests
} // namespace dbrecovery
