#ifndef DBRECOVERY_TESTS_TEST_UTILS_HPP
#define DBRECOVERY_TESTS_TEST_UTILS_HPP

#include <string>
#include <vector>
#include <cstdint>

namespace dbrecovery {
namespace tests {

// 从 .ibd 文件加载指定页的数据
std::vector<uint8_t> load_page_from_ibd(const std::string& ibd_path, uint32_t page_no);

// 创建一个最小化的测试 .ibd 文件（包含有效页头）
std::string create_test_ibd_file();

// 辅助断言：检查校验和计算结果
void assert_checksum_equal(uint32_t expected, uint32_t actual, const std::string& msg = "");

} // namespace tests
} // namespace dbrecovery

#endif // DBRECOVERY_TESTS_TEST_UTILS_HPP
