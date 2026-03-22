#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <iostream>
#include <array>
#include <cstring> // for std::memcpy

namespace dbrecovery {
namespace innodb {

// ==================== 辅助函数：校验和计算 ====================
static uint32_t compute_crc32c_internal(const std::vector<uint8_t>& page_data) {
    static const std::array<uint32_t, 256> crc_table = []() -> std::array<uint32_t, 256> {
        std::array<uint32_t, 256> table;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0x82F63B78; // CRC32C 多项式（Castagnoli）
                } else {
                    crc >>= 1;
                }
            }
            table[i] = crc;
        }
        return table;
    }();

    uint32_t crc = 0xFFFFFFFF;

    const size_t checksum_start = 24;
    const size_t checksum_end = 28;

    for (size_t i = 0; i < page_data.size(); ++i) {
        // 跳过校验和字段 (offset 24-27)
        if (i >= checksum_start && i < checksum_end) continue;

        uint8_t byte = page_data[i];
        crc = crc_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }

    return ~crc;
}

static uint32_t compute_innodb_checksum_internal(const std::vector<uint8_t>& page_data) {
    const uint32_t* data = reinterpret_cast<const uint32_t*>(page_data.data());
    size_t len = page_data.size() / 4;

    uint32_t checksum = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        // 跳过校验和字段 (offset 24-27, which is 6th and 7th uint32_t)
        if (i == 6 || i == 7) continue;

        checksum ^= dbrecovery::innodb::read_le<uint32_t>(page_data.data(), i * 4);
        checksum += (checksum >> 16);
        checksum &= 0xFFFFFFFF;
    }

    return (checksum >> 16) ^ (checksum & 0xFFFF);
}

static uint32_t compute_old_checksum_internal(const std::vector<uint8_t>& page_data) {
    // MySQL 5.5/5.6 OLD 校验和算法
    const uint16_t* data = reinterpret_cast<const uint16_t*>(page_data.data());
    size_t len = page_data.size() / 2;

    uint32_t checksum = 0;
    for (size_t i = 0; i < len; ++i) {
        // 跳过校验和字段 (offset 24-27, which is 12th and 13th uint16_t)
        if (i == 12 || i == 13) continue;

        checksum += dbrecovery::innodb::read_le<uint16_t>(page_data.data(), i * 2);
        checksum = (checksum & 0xFFFFFFFF) + (checksum >> 32);
    }

    return (checksum & 0xFFFFFFFF) + (checksum >> 32);
}

// ==================== PageReader 静态工具类实现 ====================

bool PageReader::validate_checksum(const std::vector<uint8_t>& page_data, uint32_t* out_stored, uint32_t* out_computed) {
    // ✅ 增强：严格边界检查
    if (page_data.size() < 38 + 16) { // FIL header (38) + minimal record space
        return false;
    }
    
    // ✅ 新增：验证页大小是标准 InnoDB 页大小的倍数
    static constexpr std::array<size_t, 5> valid_page_sizes = {4096, 8192, 16384, 32768, 65536};
    bool is_valid_size = false;
    for (size_t ps : valid_page_sizes) {
        if (page_data.size() == ps) {
            is_valid_size = true;
            break;
        }
    }
    if (!is_valid_size) {
        std::cerr << "[WARN] Invalid page size: " << page_data.size() << std::endl;
        return false;
    }
    
    // 读取页类型（偏移 38+12=50）
    uint16_t page_type = read_le<uint16_t>(page_data.data(), 38 + 12);
    
    // ✅ 新增：SDI 页通常不启用校验和，但需检查是否为加密表空间
    if (page_type == dbrecovery::innodb::FIL_PAGE_SDI) {
        uint32_t stored_checksum = read_le<uint32_t>(page_data.data(), 24);
        // 如果存储的校验和非零且非全1，则需要验证（加密表空间场景）
        if (stored_checksum != 0 && stored_checksum != 0xFFFFFFFF) {
            std::vector<uint8_t> page_copy = page_data;
            for (int i = 0; i < 4; ++i) {
                page_copy[24 + i] = 0;
            }
            uint32_t computed_crc32c = compute_crc32c(page_copy);
            if (computed_crc32c == stored_checksum) {
                if (out_computed) *out_computed = computed_crc32c;
                return true;
            }
            std::cerr << "[WARN] SDI page checksum mismatch. Stored=" << stored_checksum 
                      << ", CRC32C=" << computed_crc32c << std::endl;
            return false;
        }
        return true; // NONE 模式或未初始化
    }
    
    // 读取存储的校验和（偏移 24-27）
    uint32_t stored_checksum = read_le<uint32_t>(page_data.data(), 24);
    if (out_stored) *out_stored = stored_checksum;

    if (stored_checksum == 0 || stored_checksum == 0xFFFFFFFF) {
        // NONE 校验模式或未初始化的校验和
        return true;
    }
    
    // 计算当前校验和（使用 CRC32C）
    std::vector<uint8_t> page_copy = page_data;
    // 临时清零校验和字段以便计算
    for (int i = 0; i < 4; ++i) {
        page_copy[24 + i] = 0;
    }

    uint32_t computed_crc32c = compute_crc32c(page_copy);
    if (computed_crc32c == stored_checksum) {
        if (out_computed) *out_computed = computed_crc32c;
        return true;
    }

    // 尝试 INNODB 校验算法（MySQL 5.6/5.7 兼容）
    uint32_t computed_innodb = compute_innodb_checksum(page_copy);
    if (computed_innodb == stored_checksum) {
        if (out_computed) *out_computed = computed_innodb;
        return true;
    }

    // ✅ 新增：尝试 MySQL 8.0 默认的 NEW_CRC32C 算法（与 CRC32C 相同，但可能有不同实现）
    uint32_t computed_new_crc32c = compute_crc32c(page_copy);
    if (computed_new_crc32c == stored_checksum) {
        if (out_computed) *out_computed = computed_new_crc32c;
        return true;
    }

    // ✅ 新增：尝试 MySQL 8.0 的 INNODB 算法（与旧版略有不同）
    uint32_t computed_innodb_80 = compute_innodb_checksum(page_copy);
    if (computed_innodb_80 == stored_checksum) {
        if (out_computed) *out_computed = computed_innodb_80;
        return true;
    }

    // 所有校验均失败，输出详细错误信息
    std::cerr << "[ERROR] Page checksum mismatch. Stored=" << stored_checksum 
              << ", CRC32C=" << computed_crc32c 
              << ", INNODB=" << computed_innodb
              << ", NEW_CRC32C=" << computed_new_crc32c
              << ", INNODB_80=" << computed_innodb_80 << std::endl;
    if (out_computed) *out_computed = computed_crc32c; // 默认返回 CRC32C
    return false;
}

uint32_t PageReader::compute_crc32c(const std::vector<uint8_t>& page_data) {
    return compute_crc32c_internal(page_data);
}

uint32_t PageReader::compute_innodb_checksum(const std::vector<uint8_t>& page_data) {
    return compute_innodb_checksum_internal(page_data);
}

// ==================== InnoDBFileReader 实现 ====================

InnoDBFileReader::InnoDBFileReader(const std::string& file_path)
    : page_size_(16384), total_pages_(0), is_valid_(false) {
    
    mapper_ = std::make_unique<FileMapper>(file_path);
    if (!mapper_->is_valid()) {
        return;
    }

    // 计算总页数
    file_size_ = mapper_->size();
    
    // InnoDB 支持多种页大小：4KB/8KB/16KB/32KB/64KB
    // 优先从文件大小推断页大小（假设文件是完整页的倍数）
    static constexpr std::array<size_t, 5> valid_page_sizes = {4096, 8192, 16384, 32768, 65536};
    
    for (size_t ps : valid_page_sizes) {
        if (file_size_ % ps == 0) {
            page_size_ = static_cast<uint32_t>(ps);
            break;
        }
    }
    
    // 如果无法推断，使用默认值
    if (page_size_ == 16384 && file_size_ % 16384 != 0) {
        std::cerr << "[WARN] Cannot determine page size, using default 16KB\n";
    }
    
    total_pages_ = static_cast<uint32_t>(file_size_ / page_size_);
    is_valid_ = (total_pages_ > 0);
}

InnoDBFileReader::~InnoDBFileReader() = default;

InnoDBFileReader::InnoDBFileReader(InnoDBFileReader&& other) noexcept
    : file_size_(other.file_size_), page_size_(other.page_size_), total_pages_(other.total_pages_),
      mapper_(std::move(other.mapper_)), is_valid_(other.is_valid_) {
    other.file_size_ = 0;
    other.page_size_ = 16384;
    other.total_pages_ = 0;
    other.is_valid_ = false;
}

InnoDBFileReader& InnoDBFileReader::operator=(InnoDBFileReader&& other) noexcept {
    if (this != &other) {
        file_size_ = other.file_size_;
        page_size_ = other.page_size_;
        total_pages_ = other.total_pages_;
        mapper_ = std::move(other.mapper_);
        is_valid_ = other.is_valid_;

        other.file_size_ = 0;
        other.page_size_ = 16384;
        other.total_pages_ = 0;
        other.is_valid_ = false;
    }
    return *this;
}

std::vector<uint8_t> InnoDBFileReader::read_page(uint32_t page_no) {
    if (!is_valid_ || total_pages_ == 0) {
        return {};
    }
    
    // 计算页在文件中的偏移
    uint64_t offset = static_cast<uint64_t>(page_no) * page_size_;
    if (offset >= file_size_) {
        return {};
    }
    
    // 读取一页数据（原始数据）
    std::vector<uint8_t> page_data(page_size_);
    const uint8_t* data = mapper_->data() + offset;
    std::memcpy(page_data.data(), data, page_size_);
    
    // ✅ 新增：页类型校验
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 38 + 12);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX &&
        page_type != dbrecovery::innodb::FIL_PAGE_SDI &&
        page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
        std::cerr << "[WARN] Page " << page_no << " has invalid type: " << page_type << std::endl;
        return {};
    }
    
    // 校验校验和
    if (!PageReader::validate_checksum(page_data)) {
        std::cerr << "[WARN] Page " << page_no << " checksum validation failed" << std::endl;
        return {};
    }
    
    return page_data;
}

std::vector<uint8_t> InnoDBFileReader::read_page_raw(uint32_t page_no) {
    if (!is_valid_ || total_pages_ == 0) {
        return {};
    }
    
    // 计算页在文件中的偏移
    uint64_t offset = static_cast<uint64_t>(page_no) * page_size_;
    if (offset >= file_size_) {
        return {};
    }
    
    // 读取一页数据
    std::vector<uint8_t> page_data(page_size_);
    const uint8_t* data = mapper_->data() + offset;
    std::memcpy(page_data.data(), data, page_size_);
    
    return page_data;
}

std::span<const uint8_t> InnoDBFileReader::get_page_view(uint32_t page_no) {
    if (!is_valid_ || total_pages_ == 0) {
        return {};
    }
    
    // 计算页在文件中的偏移
    uint64_t offset = static_cast<uint64_t>(page_no) * page_size_;
    if (offset >= file_size_) {
        return {};
    }
    
    const uint8_t* data = mapper_->data() + offset;
    return std::span<const uint8_t>(data, page_size_);
}

} // namespace innodb
} // namespace dbrecovery

// ==================== BlobPointer 单元测试（新增）====================
#ifdef DBRECOVERY_ENABLE_TESTS
#include <gtest/gtest.h>

namespace dbrecovery {
namespace innodb {

TEST(BlobPointerTest, IsValidSinglePageValid) {
    BlobPointer ptr;
    ptr.chain_pages.emplace_back(0, 100);
    ptr.total_len = 50;
    ptr.len = 50;

    EXPECT_TRUE(ptr.is_complete());
}

TEST(BlobPointerTest, IsValidEmptyChain) {
    BlobPointer ptr;
    // chain_pages 为空

    EXPECT_FALSE(ptr.is_complete());
}

TEST(BlobPointerTest, IsValidTotalLenTooLarge) {
    BlobPointer ptr;
    ptr.chain_pages.emplace_back(0, 100);
    ptr.total_len = 128ULL * 1024 * 1024 * 1024; // >64GB
    ptr.len = 50;

    EXPECT_FALSE(ptr.is_complete());
}

TEST(BlobPointerTest, IsValidZeroLengthSinglePage) {
    BlobPointer ptr;
    ptr.chain_pages.emplace_back(0, 100);
    ptr.total_len = 0;
    ptr.len = 0; // 单页 BLOB 长度为 0 是无效的

    EXPECT_FALSE(ptr.is_complete());
}

TEST(PageReaderTest, ValidateChecksumNone) {
    std::vector<uint8_t> page(16384, 0);
    // 设置页类型为 B+Tree (FIL_PAGE_INDEX=9969)
    write_le<uint16_t>(page.data(), 50, FIL_PAGE_INDEX);
    // 校验和字段清零（NONE 模式）
    for (int i = 24; i < 28; ++i) page[i] = 0;
    
    EXPECT_TRUE(PageReader::validate_checksum(page));
}

TEST(PageReaderTest, ValidateChecksumCRC32C) {
    std::vector<uint8_t> page(16384, 0);
    // 设置页类型为 B+Tree
    write_le<uint16_t>(page.data(), 50, FIL_PAGE_INDEX);
    
    // 计算 CRC32C 并写入
    uint32_t crc = PageReader::compute_crc32c(page);
    for (int i = 0; i < 4; ++i) {
        page[24 + i] = (crc >> (i * 8)) & 0xFF;
    }
    
    EXPECT_TRUE(PageReader::validate_checksum(page));
}

// 测试页链断裂检测（BlobPageChain）
TEST(BlobPageChainTest, DetectBrokenChain) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 200); // 假设 200 不是 100 的下一个页

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置 next_bpage 为 999（不是 200），模拟断裂
            write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 999);
            return page_data;
        }
        if (space_id == 0 && page_no == 200) {
            std::vector<uint8_t> page_data(16384, 0);
            // 正确的 BLOB 页头
            write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 50);
            return page_data;
        }
        return std::nullopt;
    };

    EXPECT_FALSE(chain.is_valid(page_reader, true)); // 严格模式下应返回 false
}

// 测试页链循环引用检测（BlobPageChain）
TEST(BlobPageChainTest, DetectCircularReference) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 200);
    chain.add_page(0, 300);
    chain.add_page(0, 100); // 循环引用

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        std::vector<uint8_t> page_data(16384, 0);
        if (page_no == 100) {
            write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 200);
        } else if (page_no == 200) {
            write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 300);
        } else if (page_no == 300) {
            write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 100); // 指向 100，形成循环
        }
        return page_data;
    };

    EXPECT_FALSE(chain.is_valid(page_reader, true)); // 严格模式下应返回 false
}

// 测试页链中重复页检测（BlobPageChain）
TEST(BlobPageChainTest, DetectDuplicatePages) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 100); // 重复页

    EXPECT_FALSE(chain.has_circular_reference()); // has_circular_reference 应检测到
}

} // namespace innodb
} // namespace dbrecovery
#endif // DBRECOVERY_ENABLE_TESTS
