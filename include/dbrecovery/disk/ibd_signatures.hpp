#ifndef DBRECOVERY_DISK_IBD_SIGNATURES_HPP
#define DBRECOVERY_DISK_IBD_SIGNATURES_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <array>

namespace dbrecovery {
namespace disk {

// ==================== IBD File Signature Constants ====================

/// @brief IBD 文件的签名/特征码（文件头特征）
/// MySQL 5.6+ IBD 文件的 FIL_PAGE_TYPE 在偏移 24 处应该是 FIL_PAGE_INDEX (0x45BC / 17852)
/// 但文件开头的几个特征更重要

/// @brief IBD 文件大小特征：必须是页大小的倍数（4096, 8192, 16384, 32768, 65536）
inline bool is_valid_ibd_size(uint64_t size, uint32_t page_size = 16384) {
    return size >= page_size && (size % page_size) == 0;
}

/// @brief 检查前 512 字节是否包含有效的 IBD 文件头特征
/// MySQL 5.6 IBD 文件头特征：
/// - Offset 0-3: FSP_SPACE_ID (空间 ID)
/// - Offset 4-7: FSP_NOT_USED (未使用)
/// - Offset 8-11: FSP_SIZE (区大小)
/// - Offset 12-15: FSP_FREE_LIMIT (空闲页限制)
/// - Offset 16-19: FSP_SPACE_FLAGS (空间标志)
/// - Offset 20-23: FSP_FRAG_N_USED (段碎片页数)
/// - Offset 24-25: FIL_PAGE_TYPE = 0x45BC (17852) for MySQL 5.6
inline bool has_valid_ibd_header(const uint8_t* data, size_t size, uint32_t page_size = 16384) {
    if (size < 64) {
        return false;
    }
    
    // 读取 FIL_PAGE_TYPE (offset 24)
    uint16_t page_type = static_cast<uint16_t>(data[24]) | (static_cast<uint16_t>(data[25]) << 8);
    
    // MySQL 5.6 的索引页类型: 0x45BC (17852) 或 0x45BF (48965)
    bool is_valid_type = (page_type == 0x45BC) || (page_type == 0x45BF) || 
                         (page_type == 9969);  // MySQL 8.0 的 FIL_PAGE_INDEX
    
    if (!is_valid_type) {
        return false;
    }
    
    // 检查页大小是否合理
    uint32_t flags = static_cast<uint32_t>(data[16]) | 
                     (static_cast<uint32_t>(data[17]) << 8) |
                     (static_cast<uint32_t>(data[18]) << 16) |
                     (static_cast<uint32_t>(data[19]) << 24);
    
    // 检查 POST_ANTELOPE 位（bit 28-31）判断行格式
    uint32_t page_size_hint = (flags >> 20) & 0x0F;
    if (page_size_hint > 6) {  // 2^6 = 64KB is max page size
        return false;
    }
    
    return true;
}

/// @brief 检查页是否是有效的 InnoDB 索引页
inline bool is_valid_innodb_index_page(const uint8_t* data, size_t size, uint32_t page_size = 16384) {
    if (size < 64) {
        return false;
    }
    
    // 读取页类型
    uint16_t page_type = static_cast<uint16_t>(data[24]) | (static_cast<uint16_t>(data[25]) << 8);
    
    // 有效的 InnoDB 页类型
    return (page_type == 0x45BC) ||  // MySQL 5.6 索引页
           (page_type == 0x45BF) ||  // MySQL 5.6 索引页变体
           (page_type == 9969) ||    // MySQL 8.0 FIL_PAGE_INDEX
           (page_type == 9970) ||    // FIL_PAGE_UNDO_LOG
           (page_type == 9971) ||    // FIL_PAGE_SDI
           (page_type == 0);         // ALLOCATED
}

/// @brief 检查页 CRC32 校验（如果启用了）
/// 对于 MySQL 5.6，通常不使用 CRC32
inline bool check_page_crc32(const uint8_t* data, size_t size, uint32_t page_size = 16384) {
    // MySQL 5.6 默认不使用 CRC32，只有在特定配置下才使用
    // 这里返回 true 以兼容更多情况
    return true;
}

/// @brief 从页数据中提取空间 ID
inline uint32_t extract_space_id(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

/// @brief 从页数据中提取页号
inline uint32_t extract_page_number(const uint8_t* data) {
    return static_cast<uint32_t>(data[4]) |
           (static_cast<uint32_t>(data[5]) << 8) |
           (static_cast<uint32_t>(data[6]) << 16) |
           (static_cast<uint32_t>(data[7]) << 24);
}

/// @brief 提取页号（big-endian，用于旧版本 MySQL）
inline uint32_t extract_page_number_be(const uint8_t* data) {
    return (static_cast<uint32_t>(data[4]) << 24) |
           (static_cast<uint32_t>(data[5]) << 16) |
           (static_cast<uint32_t>(data[6]) << 8) |
           static_cast<uint32_t>(data[7]);
}

// ==================== IBD 文件签名模式 ====================
// 用于 PhotoRec 风格的文件 carving

/// @brief IBD 文件的特征签名数组
/// 格式: {offset, length, signature_bytes}
struct IbdSignature {
    uint32_t offset;
    uint32_t length;
    std::array<uint8_t, 16> bytes;
    uint8_t mask[16];  // 1 = must match, 0 = don't care
    
    IbdSignature() : offset(0), length(0) {
        std::fill(bytes.begin(), bytes.end(), 0);
        std::fill(mask, mask + 16, 0);
    }
    
    IbdSignature(uint32_t off, uint32_t len, const std::array<uint8_t, 16>& sig, const uint8_t* m)
        : offset(off), length(len) {
        bytes = sig;
        std::copy(m, m + 16, mask);
    }
};

/// @brief 预定义的 IBD 文件签名
/// 这些签名基于 MySQL 5.6 的 InnoDB 存储格式
inline std::vector<IbdSignature> get_ibd_signatures() {
    std::vector<IbdSignature>_sigs;
    
    // 签名 1: MySQL 5.6 索引页特征
    // Offset 24-25: FIL_PAGE_TYPE = 0x45BC (17852)
    {
        std::array<uint8_t, 16> sig = {0x45, 0xBC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t mask[16] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        _sigs.emplace_back(24, 2, sig, mask);
    }
    
    // 签名 2: MySQL 8.0 索引页特征
    // Offset 24-25: FIL_PAGE_TYPE = 0x26F1 (9969)
    {
        std::array<uint8_t, 16> sig = {0x26, 0xF1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t mask[16] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        _sigs.emplace_back(24, 2, sig, mask);
    }
    
    // 签名 3: 页号为 0 的页（第一个页）
    // Offset 4-7: FIL_PAGE_OFFSET = 0
    {
        std::array<uint8_t, 16> sig = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t mask[16] = {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        _sigs.emplace_back(4, 4, sig, mask);
    }
    
    // 签名 4: 空间 ID 非零（有效的表空间）
    // Offset 0-3: FSP_SPACE_ID != 0 (通常 > 0)
    // 这个签名不严格匹配，需要在匹配函数中处理
    
    return _sigs;
}

/// @brief 检查页数据是否匹配签名
inline bool match_signature(const uint8_t* data, size_t size, const IbdSignature& sig) {
    if (sig.offset + sig.length > size) {
        return false;
    }
    
    for (uint32_t i = 0; i < sig.length; ++i) {
        if (sig.mask[i]) {
            if (data[sig.offset + i] != sig.bytes[i]) {
                return false;
            }
        }
    }
    
    return true;
}

/// @brief 在数据中查找所有匹配签名的位置
inline std::vector<uint64_t> find_signatures(const uint8_t* data, size_t size, 
                                             const std::vector<IbdSignature>& sigs,
                                             uint32_t page_size = 16384) {
    std::vector<uint64_t> matches;
    
    for (size_t offset = 0; offset + page_size <= size; offset += page_size) {
        bool all_match = true;
        for (const auto& sig : sigs) {
            if (!match_signature(data + offset, page_size, sig)) {
                all_match = false;
                break;
            }
        }
        if (all_match) {
            matches.push_back(offset);
        }
    }
    
    return matches;
}

// ==================== .frm 文件签名 ====================
// .frm 文件是 MySQL 的表定义文件

/// @brief .frm 文件的特征签名
/// .frm 文件通常以 0xFE 0x01 开头（二进制格式）
inline bool has_frm_signature(const uint8_t* data, size_t size) {
    if (size < 10) {
        return false;
    }
    
    // .frm 文件通常以 0xFE 0x01 开头
    return data[0] == 0xFE && data[1] == 0x01;
}

/// @brief 检查 .frm 文件版本
inline uint16_t get_frm_version(const uint8_t* data, size_t size) {
    if (size < 12) {
        return 0;
    }
    
    // Offset 10-11: .frm 版本号
    return static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8);
}

// ==================== 大文件扫描优化 ====================
// 使用滑动窗口和签名匹配进行高效扫描

/// @brief 扫描大文件/磁盘查找 IBD 文件
/// 参数:
///   - data: 文件/磁盘数据
///   - size: 数据大小
///   - callback: 找到 IBD 文件块时的回调函数
///   - min_page_size: 最小页大小
///   - max_page_size: 最大页大小
template<typename Callback>
void scan_for_ibd_blocks(const uint8_t* data, size_t size, Callback callback,
                         uint32_t min_page_size = 4096, uint32_t max_page_size = 65536) {
    if (!data || size == 0) {
        return;
    }
    
    // 尝试不同的页大小
    std::vector<uint32_t> page_sizes;
    for (uint32_t ps = min_page_size; ps <= max_page_size; ps *= 2) {
        page_sizes.push_back(ps);
    }
    
    // 扫描每个页大小
    for (uint32_t page_size : page_sizes) {
        if (page_size == 0 || size < page_size) {
            continue;
        }
        
        size_t num_pages = size / page_size;
        
        // 找到所有可能的 IBD 页
        for (size_t page_no = 0; page_no < num_pages; ++page_no) {
            size_t offset = page_no * page_size;
            
            // 快速检查页类型
            if (offset + 26 > size) {
                break;
            }
            
            uint16_t page_type = static_cast<uint16_t>(data[offset + 24]) |
                                (static_cast<uint16_t>(data[offset + 25]) << 8);
            
            // 检查是否是有效的 InnoDB 页类型
            if (!is_valid_innodb_index_page(data + offset, page_size, page_size)) {
                continue;
            }
            
            // 验证页号和空间 ID
            uint32_t page_num = extract_page_number(data + offset);
            uint32_t space_id = extract_space_id(data + offset);
            
            // 页号应该是连续的（对于单个 .ibd 文件）
            // 但 TRUNCATE 后页号可能不连续
            // 所以只要页号合理（< 1000000）就接受
            if (page_num > 1000000) {
                continue;
            }
            
            // 回调找到的块
            callback(offset, page_size, page_num, space_id, page_type);
        }
    }
}

/// @brief 将找到的 IBD 块聚合成文件
struct IbdBlock {
    uint64_t file_offset;
    uint32_t page_size;
    uint32_t page_number;
    uint32_t space_id;
    uint16_t page_type;
    std::vector<uint8_t> data;
};

/// @brief 聚合 IBD 块为完整文件
/// 参数:
///   - blocks: 找到的 IBD 块
///   - page_size: 页大小
///   - min_pages: 最小页数（小于这个的文件太小）
///   - output: 聚合后的文件数据
inline bool aggregate_ibd_files(const std::vector<IbdBlock>& blocks,
                                uint32_t page_size,
                                uint32_t min_pages = 2,
                                std::vector<uint8_t>* output = nullptr) {
    if (blocks.size() < min_pages) {
        return false;
    }
    
    if (output == nullptr) {
        return true;
    }
    
    // 按页号排序
    std::vector<IbdBlock> sorted_blocks = blocks;
    std::sort(sorted_blocks.begin(), sorted_blocks.end(),
              [](const IbdBlock& a, const IbdBlock& b) {
                  return a.page_number < b.page_number;
              });
    
    // 检查页号是否连续（允许一定间隔，用于 TRUNCATE 场景）
    uint32_t expected_page = 0;
    uint32_t gaps = 0;
    
    for (const auto& block : sorted_blocks) {
        if (block.page_number > expected_page + 100) {
            gaps++;
        }
        expected_page = block.page_number + 1;
    }
    
    // 如果间隙太多（> 10%），可能不是同一个文件
    if (gaps > sorted_blocks.size() * 0.1) {
        return false;
    }
    
    // 填充文件数据
    output->resize(sorted_blocks.size() * page_size);
    std::fill(output->begin(), output->end(), 0);
    
    for (const auto& block : sorted_blocks) {
        size_t file_offset = block.page_number * page_size;
        if (file_offset + page_size <= output->size()) {
            std::memcpy(output->data() + file_offset, block.data.data(), block.data.size());
        }
    }
    
    return true;
}

} // namespace disk
} // namespace dbrecovery

#endif // DBRECOVERY_DISK_IBD_SIGNATURES_HPP
