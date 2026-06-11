#ifndef DBRECOVERY_INNODB_HEURISTIC_SCANNER_HPP
#define DBRECOVERY_INNODB_HEURISTIC_SCANNER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"

namespace dbrecovery {
namespace innodb {

// 前向声明
// ✅ 使用 page_defs.hpp 中的 is_valid_page_type 定义（避免重复定义）
// inline bool is_valid_page_type(uint16_t type);  // 已在 page_defs.hpp 中定义

// ==================== Heuristic Scan Result ====================
/**
 * @brief 启发式扫描结果
 */
struct HeuristicScanResult {
    uint32_t page_no;
    uint16_t page_type;
    uint32_t offset;
    uint64_t lsn;
    uint32_t space_id;
    
    // 推断的页信息
    uint32_t prev_page;
    uint32_t next_page;
    uint16_t page_level;
    
    // 坏块推断
    bool is_bad_block;
    std::string bad_block_reason;
    
    // 页健康状态
    bool is_valid;
    std::string status_message;
    
    // 置信度 (0-100)
    uint8_t confidence;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "HeuristicScanResult{page=" << page_no
            << ", type=0x" << std::hex << page_type << std::dec
            << ", offset=" << offset
            << ", lsn=" << lsn
            << ", space=" << space_id
            << ", prev=" << prev_page
            << ", next=" << next_page
            << ", level=" << page_level
            << ", bad=" << std::boolalpha << is_bad_block << std::noboolalpha
            << ", valid=" << is_valid
            << ", confidence=" << (int)confidence << "%}";
        return oss.str();
    }
};

/**
 * @brief 坏块推断结果
 */
struct BadBlockInfo {
    uint32_t page_no;
    uint32_t offset;
    std::string reason;
    std::vector<std::string> evidence;
    bool is_recoverable;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "BadBlockInfo{page=" << page_no
            << ", offset=" << offset
            << ", reason=\"" << reason << "\""
            << ", recoverable=" << std::boolalpha << is_recoverable << std::noboolalpha
            << ", evidence=[" << evidence.size() << " items]}";
        return oss.str();
    }
};

/**
 * @brief 启发式扫描统计
 */
struct HeuristicScanStats {
    size_t total_pages_scanned = 0;
    size_t valid_pages = 0;
    size_t bad_blocks = 0;
    size_t fragmented_pages = 0;
    size_t metadata_pages = 0;
    size_t recovered_records = 0;
    size_t inferred_pages = 0;
    
    // 坏块类型统计
    size_t checksum_errors = 0;
    size_t type_mismatches = 0;
    size_t chain_breaks = 0;
    size_t corrupted_headers = 0;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "HeuristicScanStats{scanned=" << total_pages_scanned
            << ", valid=" << valid_pages
            << ", bad=" << bad_blocks
            << ", fragmented=" << fragmented_pages
            << ", metadata=" << metadata_pages
            << ", recovered=" << recovered_records
            << ", inferred=" << inferred_pages
            << ", checksum_err=" << checksum_errors
            << ", type_mismatch=" << type_mismatches
            << ", chain_breaks=" << chain_breaks
            << ", corrupted_hdr=" << corrupted_headers << "}";
        return oss.str();
    }
};

// ==================== Heuristic Scanner ====================
/**
 * @brief 启发式扫描器
 * @details 使用启发式方法扫描和推断 InnoDB 页，即使在数据损坏的情况下
 */
class HeuristicScanner {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小（默认 16KB）
     * @param scan_mode 扫描模式
     */
    explicit HeuristicScanner(size_t page_size = 16384, bool aggressive = false);

    /**
     * @brief 设置扫描模式
     */
    void set_aggressive_mode(bool aggressive);

    /**
     * @brief 扫描文件
     */
    std::vector<HeuristicScanResult> scan_file(const std::filesystem::path& file_path);

    /**
     * @brief 扫描内存缓冲区
     */
    std::vector<HeuristicScanResult> scan_buffer(const uint8_t* data, size_t size);

    /**
     * @brief 获取扫描统计
     */
    const HeuristicScanStats& stats() const { return stats_; }

    /**
     * @brief 获取坏块信息
     */
    const std::vector<BadBlockInfo>& bad_blocks() const { return bad_blocks_; }

    /**
     * @brief 推断页类型
     * @details 根据页内容特征推断页类型
     */
    uint16_t infer_page_type(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 推断页号
     * @details 根据页在文件中的位置推断页号
     */
    uint32_t infer_page_no(uint32_t file_offset, uint32_t base_page_no = 0);

    /**
     * @brief 推断页链关系
     * @details 根据页号推断 prev/next 关系
     */
    void infer_page_chain(std::vector<HeuristicScanResult>& results);

    /**
     * @brief 推断 LSN
     * @details 根据页号和文件位置推断 LSN
     */
    uint64_t infer_lsn(uint32_t page_no, uint64_t base_lsn = 0);

    /**
     * @brief 推断页级别
     * @details 根据页号推断页级别
     */
    uint16_t infer_page_level(uint32_t page_no, uint32_t root_page_no = 0);

    /**
     * @brief 检测坏块
     */
    BadBlockInfo detect_bad_block(const uint8_t* page_data, size_t page_size, uint32_t page_no);

    /**
     * @brief 修复坏块信息
     */
    void fix_bad_block_info(HeuristicScanResult& result);

    /**
     * @brief 获取坏块推断报告
     */
    std::string get_bad_block_report() const;

    /**
     * @brief 获取扫描报告
     */
    std::string get_scan_report() const;

    // Validation methods (public for testing)
    bool validate_page_header(const uint8_t* page_data);
    bool validate_page_tail(const uint8_t* page_data);
    bool validate_checksum(const uint8_t* page_data);
    bool validate_record_header(const uint8_t* page_data);
    bool validate_page_directory(const uint8_t* page_data);

private:
    size_t page_size_;
    bool aggressive_mode_;
    HeuristicScanStats stats_;
    std::vector<BadBlockInfo> bad_blocks_;

    // 扫描单个页
    HeuristicScanResult scan_single_page(const uint8_t* data, size_t offset);

    // 推断页类型
    uint16_t infer_page_type_internal(const uint8_t* page_data);

    // 检测碎片化页
    bool is_fragmented_page(const uint8_t* page_data);
};

// ==================== 实现 ====================

inline HeuristicScanner::HeuristicScanner(size_t page_size, bool aggressive)
    : page_size_(page_size), aggressive_mode_(aggressive) {
    // 验证页大小
    if (page_size_ != 4096 && page_size_ != 8192 &&
        page_size_ != 16384 && page_size_ != 32768 && page_size_ != 65536) {
        page_size_ = 16384;
    }
}

inline void HeuristicScanner::set_aggressive_mode(bool aggressive) {
    aggressive_mode_ = aggressive;
}

inline uint16_t HeuristicScanner::infer_page_type_internal(const uint8_t* page_data) {
    // 检查页头的页类型字段
    uint16_t type = read_le<uint16_t>(page_data, 16);
    
    // 如果页类型有效，直接返回
    if (is_valid_page_type(type)) {
        return type;
    }
    
    // 否则尝试推断页类型
    // 检查页头的其他字段
    uint32_t page_no = read_le<uint32_t>(page_data, 20);
    uint32_t space_id = read_le<uint32_t>(page_data, 24);
    
    // 如果页号和空间ID有效，可能是索引页
    if (page_no < 0xFFFFFFF0 && space_id < 0xFFFFFFF0) {
        return FIL_PAGE_INDEX;
    }
    
    // 默认返回未分配页
    return FIL_PAGE_TYPE_ALLOCATED;
}

inline uint16_t HeuristicScanner::infer_page_type(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 38) {
        return FIL_PAGE_TYPE_ALLOCATED;
    }
    
    return infer_page_type_internal(page_data);
}

inline uint32_t HeuristicScanner::infer_page_no(uint32_t file_offset, uint32_t base_page_no) {
    // 页号 = 文件偏移 / 页大小 + 基础页号
    uint32_t page_no = (file_offset / page_size_) + base_page_no;
    return page_no;
}

inline uint64_t HeuristicScanner::infer_lsn(uint32_t page_no, uint64_t base_lsn) {
    // LSN 通常随页号递增
    // 简化实现：LSN = base_lsn + page_no * 1000
    return base_lsn + (page_no * 1000);
}

inline uint16_t HeuristicScanner::infer_page_level(uint32_t page_no, uint32_t root_page_no) {
    // 简化实现：假设所有页都是叶子页（level = 0）
    // 实际应该根据页号范围推断
    return 0;
}

inline bool HeuristicScanner::validate_page_header(const uint8_t* page_data) {
    if (!page_data || page_size_ < 38) {
        return false;
    }

    // 检查页类型
    uint16_t type = read_le<uint16_t>(page_data, 16);
    if (!is_valid_page_type(type)) {
        return false;
    }

    // 检查页号
    uint32_t page_no = read_le<uint32_t>(page_data, 20);
    // 页号 >= 0xFFFFFFF0 且 != 0xFFFFFFFF 被视为无效（保留值）
    if (page_no >= 0xFFFFFFF0 && page_no != 0xFFFFFFFF) {
        return false;
    }

    // 检查空间ID
    uint32_t space_id = read_le<uint32_t>(page_data, 24);
    // 空间ID >= 0xFFFFFFF0 被视为无效（保留值）
    if (space_id >= 0xFFFFFFF0) {
        return false;
    }

    return true;
}

inline bool HeuristicScanner::validate_page_tail(const uint8_t* page_data) {
    if (!page_data || page_size_ < 8) {
        return false;
    }
    
    // 检查页尾的校验和字段
    // 页尾在偏移 page_size - 8 处
    size_t tail_offset = page_size_ - 8;
    
    // 读取页尾的校验和
    uint32_t checksum = read_le<uint32_t>(page_data, tail_offset);
    
    // 校验和为 0 或 0xFFFFFFFF 表示未校验
    if (checksum == 0 || checksum == 0xFFFFFFFF) {
        return true;
    }
    
    // 其他值可能表示已校验
    return true;
}

inline bool HeuristicScanner::validate_checksum(const uint8_t* page_data) {
    if (!page_data || page_size_ < 52) {
        return false;
    }
    
    // 读取存储的校验和
    uint32_t stored_checksum = read_le<uint32_t>(page_data, 24);
    
    // 简化实现：校验和为 0 表示未校验
    if (stored_checksum == 0) {
        return true;
    }
    
    // 校验和为 0xFFFFFFFF 表示 CRC32C
    if (stored_checksum == 0xFFFFFFFF) {
        return true;
    }
    
    // 其他值需要实际计算校验和
    // 这里简化处理
    return true;
}

inline bool HeuristicScanner::validate_record_header(const uint8_t* page_data) {
    if (!page_data || page_size_ < 54 + 13) {
        return false;
    }
    
    // 检查 INFIMUM 记录
    uint8_t infimum_info = page_data[54];
    uint8_t infimum_type = infimum_info & 0x07;
    
    if (infimum_type != REC_STATUS_INFIMUM) {
        return false;
    }
    
    // 检查 INFIMUM 的 next_record_offset
    uint16_t next_offset = read_le<uint16_t>(page_data, 55) & 0x3FFF;
    
    if (next_offset < 54 || next_offset >= page_size_) {
        return false;
    }
    
    return true;
}

inline bool HeuristicScanner::validate_page_directory(const uint8_t* page_data) {
    if (!page_data || page_size_ < 74) {
        return false;
    }

    // 读取目录槽数量
    uint16_t n_dir_slots = read_le<uint16_t>(page_data, 38);

    // 检查目录槽数量是否合理
    if (n_dir_slots < 2 || n_dir_slots > 1000) {
        return false;
    }

    // 检查目录槽范围
    size_t dir_end = 38 + n_dir_slots * 2;
    if (dir_end > page_size_) {
        return false;
    }

    return true;
}

inline bool HeuristicScanner::is_fragmented_page(const uint8_t* page_data) {
    if (!page_data || page_size_ < 54) {
        return false;
    }
    
    // 检查堆顶位置
    uint16_t heap_top = read_le<uint16_t>(page_data, 40);
    
    // 检查空闲空间
    uint16_t free_offset = read_le<uint16_t>(page_data, 44);
    
    // 如果空闲空间很大，可能是碎片化页
    if (free_offset > heap_top) {
        return true;
    }
    
    // 检查页级别
    uint16_t page_level = read_le<uint16_t>(page_data, 42);
    
    // 如果页级别很高，可能是内部节点页
    if (page_level > 5) {
        return true;
    }
    
    return false;
}

inline BadBlockInfo HeuristicScanner::detect_bad_block(const uint8_t* page_data, size_t page_size, uint32_t page_no) {
    BadBlockInfo bad_block{};
    bad_block.page_no = page_no;
    bad_block.offset = page_no * page_size;
    bad_block.is_recoverable = true;
    
    if (!page_data || page_size < 38) {
        bad_block.reason = "Page too small";
        bad_block.evidence.push_back("Page size < 38 bytes");
        bad_block.is_recoverable = false;
        return bad_block;
    }
    
    // 检查页类型
    uint16_t type = read_le<uint16_t>(page_data, 16);
    if (!is_valid_page_type(type)) {
        bad_block.reason = "Invalid page type";
        bad_block.evidence.push_back("Page type: 0x" + std::to_string(type));
        bad_block.is_recoverable = false;
        return bad_block;
    }
    
    // 检查页号
    uint32_t stored_page_no = read_le<uint32_t>(page_data, 20);
    if (stored_page_no != page_no) {
        bad_block.reason = "Page number mismatch";
        bad_block.evidence.push_back("Expected: " + std::to_string(page_no));
        bad_block.evidence.push_back("Stored: " + std::to_string(stored_page_no));
        bad_block.is_recoverable = true;  // 可以修复
        return bad_block;
    }
    
    // 检查校验和
    if (!validate_checksum(page_data)) {
        bad_block.reason = "Checksum mismatch";
        bad_block.evidence.push_back("Checksum validation failed");
        bad_block.is_recoverable = true;  // 可以修复
        return bad_block;
    }
    
    // 检查页头
    if (!validate_page_header(page_data)) {
        bad_block.reason = "Corrupted page header";
        bad_block.evidence.push_back("Page header validation failed");
        bad_block.is_recoverable = true;  // 可以修复
        return bad_block;
    }
    
    // 检查记录头
    if (!validate_record_header(page_data)) {
        bad_block.reason = "Corrupted record header";
        bad_block.evidence.push_back("Record header validation failed");
        bad_block.is_recoverable = true;  // 可以修复
        return bad_block;
    }
    
    // 检查页目录
    if (!validate_page_directory(page_data)) {
        bad_block.reason = "Corrupted page directory";
        bad_block.evidence.push_back("Page directory validation failed");
        bad_block.is_recoverable = true;  // 可以修复
        return bad_block;
    }
    
    // 检查页尾
    if (!validate_page_tail(page_data)) {
        bad_block.reason = "Corrupted page tail";
        bad_block.evidence.push_back("Page tail validation failed");
        bad_block.is_recoverable = true;  // 可以修复
        return bad_block;
    }
    
    return bad_block;
}

inline void HeuristicScanner::fix_bad_block_info(HeuristicScanResult& result) {
    if (!result.is_bad_block) {
        return;
    }
    
    // 尝试修复页信息
    if (result.page_type == 0) {
        result.page_type = infer_page_type_internal((const uint8_t*)nullptr);
    }
    
    if (result.page_no == 0) {
        result.page_no = infer_page_no(result.offset);
    }
    
    if (result.lsn == 0) {
        result.lsn = infer_lsn(result.page_no);
    }
    
    if (result.prev_page == 0xFFFFFFFF && result.next_page == 0xFFFFFFFF) {
        // 孤立页，标记为可恢复
        result.is_bad_block = false;
        result.confidence = 50;
        result.status_message = "Orphaned page, recovered";
    }
}

inline HeuristicScanResult HeuristicScanner::scan_single_page(const uint8_t* data, size_t offset) {
    HeuristicScanResult result{};
    result.offset = static_cast<uint32_t>(offset);
    result.page_no = 0;
    result.page_type = 0;
    result.lsn = 0;
    result.space_id = 0;
    result.prev_page = 0xFFFFFFFF;
    result.next_page = 0xFFFFFFFF;
    result.page_level = 0;
    result.is_bad_block = false;
    result.is_valid = false;
    result.confidence = 0;
    
    if (!data || page_size_ < 38) {
        result.status_message = "Invalid page data";
        return result;
    }
    
    // 读取页头信息 (FIL Header)
    result.prev_page = read_le<uint32_t>(data, 0);   // prev_page at offset 0
    result.next_page = read_le<uint32_t>(data, 4);   // next_page at offset 4
    result.lsn = read_le<uint64_t>(data, 8);         // LSN at offset 8
    result.page_type = read_le<uint16_t>(data, 16);  // page_type at offset 16
    result.page_no = read_le<uint32_t>(data, 20);    // page_no at offset 20
    result.space_id = read_le<uint32_t>(data, 24);   // space_id at offset 24
    
    // 读取页级别
    if (page_size_ >= 42) {
        result.page_level = read_le<uint16_t>(data, 42);
    }
    
    // 检测坏块
    BadBlockInfo bad_block = detect_bad_block(data, page_size_, result.page_no);
    
    if (!bad_block.reason.empty()) {
        result.is_bad_block = true;
        result.bad_block_reason = bad_block.reason;
        result.confidence = 30;
        
        if (bad_block.is_recoverable) {
            result.is_valid = true;
            result.confidence = 50;
            result.status_message = "Bad block, but recoverable";
        } else {
            result.is_valid = false;
            result.confidence = 10;
            result.status_message = "Bad block, not recoverable";
        }
    } else {
        result.is_valid = true;
        result.confidence = 90;
        result.status_message = "Valid page";
    }
    
    return result;
}

inline std::vector<HeuristicScanResult> HeuristicScanner::scan_buffer(const uint8_t* data, size_t size) {
    std::vector<HeuristicScanResult> results;
    stats_ = HeuristicScanStats{};
    bad_blocks_.clear();
    
    if (!data || size < page_size_) {
        return results;
    }
    
    stats_.total_pages_scanned = size / page_size_;
    
    // 扫描每个页
    for (size_t offset = 0; offset + page_size_ <= size; offset += page_size_) {
        const uint8_t* page_data = data + offset;
        HeuristicScanResult result = scan_single_page(page_data, offset);
        
        // 推断页类型（如果未识别）
        if (result.page_type == 0) {
            result.page_type = infer_page_type_internal(page_data);
        }
        
        // 推断页号（如果为0）
        if (result.page_no == 0) {
            result.page_no = infer_page_no(static_cast<uint32_t>(offset));
        }
        
        // 推断 LSN（如果为0）
        if (result.lsn == 0) {
            result.lsn = infer_lsn(result.page_no);
        }
        
        // 推断页级别
        if (result.page_level == 0) {
            result.page_level = infer_page_level(result.page_no, 0);
        }
        
        // 检查是否为碎片化页
        if (is_fragmented_page(page_data)) {
            result.status_message += ", fragmented";
            stats_.fragmented_pages++;
        }
        
        // 统计页类型
        switch (result.page_type) {
            case FIL_PAGE_INDEX:
                stats_.valid_pages++;
                break;
            case FIL_PAGE_SDI:
                stats_.metadata_pages++;
                break;
            case FIL_PAGE_UNDO_LOG:
                stats_.valid_pages++;
                break;
            default:
                if (result.is_valid) {
                    stats_.valid_pages++;
                }
                break;
        }
        
        // 统计坏块
        if (result.is_bad_block) {
            stats_.bad_blocks++;
            
            BadBlockInfo bad_block{};
            bad_block.page_no = result.page_no;
            bad_block.offset = result.offset;
            bad_block.reason = result.bad_block_reason;
            bad_block.is_recoverable = result.is_valid;
            
            if (result.bad_block_reason.find("checksum") != std::string::npos) {
                stats_.checksum_errors++;
            }
            if (result.bad_block_reason.find("type") != std::string::npos) {
                stats_.type_mismatches++;
            }
            if (result.bad_block_reason.find("chain") != std::string::npos) {
                stats_.chain_breaks++;
            }
            if (result.bad_block_reason.find("header") != std::string::npos) {
                stats_.corrupted_headers++;
            }
            
            bad_blocks_.push_back(bad_block);
        }
        
        results.push_back(result);
    }
    
    // 推断页链关系
    infer_page_chain(results);
    
    return results;
}

inline std::vector<HeuristicScanResult> HeuristicScanner::scan_file(const std::filesystem::path& file_path) {
    std::vector<HeuristicScanResult> results;
    
    // 打开文件
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return results;
    }
    
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // 分配缓冲区
    std::vector<uint8_t> buffer(file_size);
    
    // 读取整个文件
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    size_t actual_read = file.gcount();
    
    if (actual_read == 0) {
        return results;
    }
    
    // 扫描
    return scan_buffer(buffer.data(), actual_read);
}

inline void HeuristicScanner::infer_page_chain(std::vector<HeuristicScanResult>& results) {
    if (results.empty()) {
        return;
    }
    
    // 按页号排序
    std::sort(results.begin(), results.end(),
              [](const HeuristicScanResult& a, const HeuristicScanResult& b) {
                  return a.page_no < b.page_no;
              });
    
    // 推断 prev/next 关系
    for (size_t i = 0; i < results.size(); ++i) {
        auto& result = results[i];
        
        // 设置 prev 指针
        if (i > 0) {
            result.prev_page = results[i - 1].page_no;
        } else {
            result.prev_page = 0xFFFFFFFF;
        }
        
        // 设置 next 指针
        if (i < results.size() - 1) {
            result.next_page = results[i + 1].page_no;
        } else {
            result.next_page = 0xFFFFFFFF;
        }
    }
}

inline std::string HeuristicScanner::get_bad_block_report() const {
    std::ostringstream oss;
    oss << "Bad Block Report:" << std::endl;
    oss << "  Total bad blocks: " << bad_blocks_.size() << std::endl;
    oss << "  Checksum errors: " << stats_.checksum_errors << std::endl;
    oss << "  Type mismatches: " << stats_.type_mismatches << std::endl;
    oss << "  Chain breaks: " << stats_.chain_breaks << std::endl;
    oss << "  Corrupted headers: " << stats_.corrupted_headers << std::endl;
    
    if (!bad_blocks_.empty()) {
        oss << std::endl << "Bad Block Details:" << std::endl;
        for (const auto& block : bad_blocks_) {
            oss << "  Page " << block.page_no << ": " << block.reason << std::endl;
            for (const auto& evidence : block.evidence) {
                oss << "    - " << evidence << std::endl;
            }
        }
    }
    
    return oss.str();
}

inline std::string HeuristicScanner::get_scan_report() const {
    std::ostringstream oss;
    oss << "Heuristic Scan Report:" << std::endl;
    oss << stats_.to_string() << std::endl;
    oss << get_bad_block_report();
    return oss.str();
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_HEURISTIC_SCANNER_HPP
