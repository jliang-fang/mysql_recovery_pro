#include "dbrecovery/innodb/blob_pointer.hpp"
#include <iostream>

namespace dbrecovery {
namespace innodb {

bool BlobPageChain::is_valid(const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader,
                             bool strict_mode) const {
    if (pages.empty()) {
        std::cerr << "[ERROR] BLOB chain is empty" << std::endl;
        return false;
    }
    
    // 检查是否有重复页（在任何模式下都必须检查）
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (const auto& p : pages) {
        if (seen.count(p)) {
            std::cerr << "[ERROR] BLOB chain contains duplicate page: (" 
                      << p.first << ", " << p.second << ")" << std::endl;
            return false;
        }
        seen.insert(p);
    }
    
    // 如果不是严格模式，且没有 page_reader，则返回 true（允许宽松恢复）
    if (!strict_mode && !page_reader) {
        std::cerr << "[INFO] BLOB chain validation skipped (non-strict mode): no page_reader provided" << std::endl;
        return true; // 宽松模式下允许无校验通过
    }
    
    // 如果没有 page_reader 但处于严格模式，返回 false（必须验证）
    if (!page_reader) {
        std::cerr << "[WARN] BLOB chain validation skipped: no page_reader provided" << std::endl;
        return false; // 更安全的默认行为：未知状态视为无效
    }
    
    // 验证链表连续性（需要实际读取 BLOB 页头验证）
    for (size_t i = 1; i < pages.size(); ++i) {
        const auto& prev_page = pages[i-1];
        const auto& curr_page = pages[i];
        
        if (prev_page.first != curr_page.first) {
            // 不同表空间，跳过连续性检查（允许跨表空间BLOB）
            std::cerr << "[WARN] BLOB chain crosses tablespace from (" 
                      << prev_page.first << ", " << prev_page.second << ") to ("
                      << curr_page.first << ", " << curr_page.second << ")" << std::endl;
            continue; // 继续处理后续页对
        }
        
        // 读取前一个页的 BLOB 头以验证 next_bpage 字段
        auto prev_data = page_reader(prev_page.first, prev_page.second);
        if (!prev_data) {
            if (strict_mode) {
                std::cerr << "[ERROR] Cannot read BLOB page header for validation: (" 
                          << prev_page.first << ", " << prev_page.second << ")" << std::endl;
                return false; // 严格模式下无法读取视为失败
            } else {
                std::cerr << "[WARN] Cannot read BLOB page header for validation (non-strict): (" 
                          << prev_page.first << ", " << prev_page.second << ")" << std::endl;
                continue; // 宽松模式下跳过验证
            }
        }
        
        if (prev_data->size() < 16) {
            if (strict_mode) {
                std::cerr << "[ERROR] BLOB page header too small: (" 
                          << prev_page.first << ", " << prev_page.second << ")" << std::endl;
                return false; // 严格模式下数据不足视为失败
            } else {
                std::cerr << "[WARN] BLOB page header too small (non-strict): (" 
                          << prev_page.first << ", " << prev_page.second << ")" << std::endl;
                continue; // 宽松模式下跳过验证
            }
        }
        
        // 读取 next_bpage 字段（偏移 0）
        uint32_t next_bpage = dbrecovery::innodb::read_le<uint32_t>(prev_data->data(), BTR_BPAGE_NEXT);
        
        if (next_bpage == 0xFFFFFFFF) {
            // 链表提前结束，标记为警告但不失败
            std::cerr << "[WARN] BLOB chain ends prematurely at page (" 
                      << prev_page.first << ", " << prev_page.second << ")" << std::endl;
            break;
        }
        
        if (next_bpage != curr_page.second) {
            std::cerr << "[ERROR] BLOB chain断裂 at page (" 
                      << prev_page.first << ", " << prev_page.second 
                      << "): expected next=" << curr_page.second 
                      << ", actual=" << next_bpage << std::endl;
            return false; // next_bpage 不匹配，链表断裂
        }
        
        // ✅ 新增：验证 BLOB 页头校验和（增强数据完整性检查）
        // 注意：BLOB 页也是标准 InnoDB 页，校验和位于 FIL Header 的偏移 24-27 (即页起始位置 + 24)
        if (prev_data->size() >= 62) { // 确保有足够空间读取到校验和字段
            uint32_t stored_checksum = dbrecovery::innodb::read_le<uint32_t>(prev_data->data(), 24);
            
            // 标准 InnoDB 校验模式：NONE (0 或 0xFFFFFFFF) 视为有效
            if (stored_checksum == 0 || stored_checksum == 0xFFFFFFFF) {
                // 校验和未启用，跳过验证
            } else {
                // 创建页副本并清零校验和字段（偏移 24-27）
                std::vector<uint8_t> page_copy = *prev_data;
                for (int j = 0; j < 4; ++j) {
                    page_copy[24 + j] = 0;
                }
                
                uint32_t computed_crc32c = dbrecovery::innodb::PageReader::compute_crc32c(page_copy);
                if (computed_crc32c != stored_checksum) {
                    std::cerr << "[WARN] BLOB page checksum mismatch at (" 
                              << prev_page.first << ", " << prev_page.second 
                              << "), stored=" << stored_checksum 
                              << ", computed=" << computed_crc32c << std::endl;
                }
            }
        } else {
            // 页太小，无法包含完整的 FIL Header
            std::cerr << "[WARN] BLOB page too small for checksum validation at (" 
                      << prev_page.first << ", " << prev_page.second << ")" << std::endl;
        }
    }
    
    return true;
}

} // namespace innodb
} // namespace dbrecovery
