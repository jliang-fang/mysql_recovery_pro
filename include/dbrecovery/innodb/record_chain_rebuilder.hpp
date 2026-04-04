#ifndef DBRECOVERY_INNODB_RECORD_CHAIN_REBUILDER_HPP
#define DBRECOVERY_INNODB_RECORD_CHAIN_REBUILDER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/btree_node_parser.hpp"

namespace dbrecovery {
namespace innodb {

// 记录链重连统计（在类之前声明）
struct RecordChainStats {
    size_t total_records = 0;
    size_t fixed_chains = 0;
    size_t orphan_records = 0;
    size_t circular_references = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "RecordChainStats{total=" << total_records
            << ", fixed=" << fixed_chains
            << ", orphan=" << orphan_records
            << ", circular=" << circular_references << "}";
        return oss.str();
    }
};

/**
 * @brief 记录链重连器
 * @details 修复页内记录链断裂，重建记录的顺序关系
 */
class RecordChainRebuilder {
public:
    RecordChainRebuilder();

    /**
     * @brief 重连记录链
     * @param records 原始记录列表
     * @param heap_top 堆顶位置
     * @return true 重连成功
     */
    bool rebuild_chain(const std::vector<RecordInfo>& records, uint16_t heap_top);

    /**
     * @brief 获取重连后的记录
     */
    const std::vector<RecordInfo>& get_rebuilt_records() const { return rebuilt_records_; }

    /**
     * @brief 获取重连统计
     */
    const RecordChainStats& stats() const { return chain_stats_; }

    /**
     * @brief 按 heap_no 排序记录
     */
    void sort_by_heap_no();

    /**
     * @brief 按记录偏移排序
     */
    void sort_by_offset();

    /**
     * @brief 修复记录的 next 指针
     */
    void fix_next_pointers();

    /**
     * @brief 检测记录链中的循环引用
     */
    bool has_circular_reference() const;

    /**
     * @brief 获取孤立记录（不在链中）
     */
    std::vector<size_t> get_orphan_records() const;

    /**
     * @brief 构建 heap_no 到索引的映射
     */
    std::map<uint16_t, size_t> build_heap_no_map() const;

    /**
     * @brief 解析记录的字段数据
     */
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> parse_record_fields(
        const RecordInfo& record, uint16_t n_fields) const;

private:
    std::vector<RecordInfo> rebuilt_records_;
    RecordChainStats chain_stats_;

    // 检测循环引用
    bool detect_circular_reference() const;

    // 标记孤立记录
    void mark_orphan_records();
};

// ==================== 实现 ====================

inline RecordChainRebuilder::RecordChainRebuilder() {
    chain_stats_.total_records = 0;
    chain_stats_.fixed_chains = 0;
    chain_stats_.orphan_records = 0;
    chain_stats_.circular_references = 0;
}

inline bool RecordChainRebuilder::rebuild_chain(const std::vector<RecordInfo>& records, uint16_t heap_top) {
    if (records.empty()) {
        chain_stats_.total_records = 0;
        return false;
    }

    // 初始化
    rebuilt_records_ = records;
    chain_stats_.total_records = records.size();
    chain_stats_.fixed_chains = 0;
    chain_stats_.orphan_records = 0;
    chain_stats_.circular_references = 0;

    // 按 heap_no 排序
    sort_by_heap_no();

    // 修复 next 指针
    fix_next_pointers();

    // 检测循环引用
    if (detect_circular_reference()) {
        chain_stats_.circular_references++;
    }

    // 标记孤立记录
    mark_orphan_records();

    return true;
}

inline void RecordChainRebuilder::sort_by_heap_no() {
    std::sort(rebuilt_records_.begin(), rebuilt_records_.end(),
              [](const RecordInfo& a, const RecordInfo& b) {
                  return a.heap_no < b.heap_no;
              });
}

inline void RecordChainRebuilder::sort_by_offset() {
    std::sort(rebuilt_records_.begin(), rebuilt_records_.end(),
              [](const RecordInfo& a, const RecordInfo& b) {
                  return a.offset < b.offset;
              });
}

inline void RecordChainRebuilder::fix_next_pointers() {
    if (rebuilt_records_.empty()) {
        return;
    }

    // 按 heap_no 排序后，修复 next 指针
    for (size_t i = 0; i < rebuilt_records_.size(); ++i) {
        auto& record = rebuilt_records_[i];

        // 设置 next 指针为下一条记录的偏移
        if (i < rebuilt_records_.size() - 1) {
            record.next_offset = rebuilt_records_[i + 1].offset;
            chain_stats_.fixed_chains++;
        } else {
            // 最后一条记录的 next 应该指向 SUPREMUM（使用默认值）
            record.next_offset = 0xFFFD;  // SUPREMUM 通常在 heap_top - 5
            chain_stats_.fixed_chains++;
        }
    }
}

inline bool RecordChainRebuilder::detect_circular_reference() const {
    if (rebuilt_records_.empty()) {
        return false;
    }

    // 使用快慢指针检测循环
    for (size_t start = 0; start < rebuilt_records_.size(); ++start) {
        size_t slow = start;
        size_t fast = start;

        while (true) {
            // 慢指针走一步
            uint16_t next_offset = rebuilt_records_[slow].next_offset;
            if (next_offset == 0xFFFF || next_offset <= rebuilt_records_[slow].offset) {
                break;
            }

            // 查找下一个记录
            bool found = false;
            for (size_t i = 0; i < rebuilt_records_.size(); ++i) {
                if (rebuilt_records_[i].offset == next_offset) {
                    slow = i;
                    found = true;
                    break;
                }
            }
            if (!found) break;

            // 快指针走两步
            for (int step = 0; step < 2; ++step) {
                next_offset = rebuilt_records_[fast].next_offset;
                if (next_offset == 0xFFFF || next_offset <= rebuilt_records_[fast].offset) {
                    break;
                }

                found = false;
                for (size_t i = 0; i < rebuilt_records_.size(); ++i) {
                    if (rebuilt_records_[i].offset == next_offset) {
                        fast = i;
                        found = true;
                        break;
                    }
                }
                if (!found) break;
            }
            if (fast >= rebuilt_records_.size()) break;

            // 如果快慢指针相遇，说明有循环
            if (slow == fast) {
                return true;
            }
        }
    }

    return false;
}

inline bool RecordChainRebuilder::has_circular_reference() const {
    return detect_circular_reference();
}

inline std::vector<size_t> RecordChainRebuilder::get_orphan_records() const {
    std::vector<size_t> orphan_indices;

    for (size_t i = 0; i < rebuilt_records_.size(); ++i) {
        const auto& record = rebuilt_records_[i];

        // 孤立记录的判断：
        // 1. next 指针无效（0xFFFF 或指向自己）
        // 2. 或者 next 指针指向不存在的记录
        bool next_valid = (record.next_offset == 0xFFFF) ||
                          (record.next_offset > record.offset);

        if (!next_valid) {
            orphan_indices.push_back(i);
        }
    }

    return orphan_indices;
}

inline void RecordChainRebuilder::mark_orphan_records() {
    auto orphan_indices = get_orphan_records();
    chain_stats_.orphan_records = orphan_indices.size();
}

inline std::map<uint16_t, size_t> RecordChainRebuilder::build_heap_no_map() const {
    std::map<uint16_t, size_t> map;
    for (size_t i = 0; i < rebuilt_records_.size(); ++i) {
        map[rebuilt_records_[i].heap_no] = i;
    }
    return map;
}

inline std::vector<std::pair<uint16_t, std::vector<uint8_t>>> RecordChainRebuilder::parse_record_fields(
    const RecordInfo& record, uint16_t n_fields) const {
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> fields;

    if (n_fields == 0 || record.raw_data.size() < 6) {
        return fields;
    }

    // COMPACT 格式：第6字节开始是可变字段长度数组
    size_t pos = 6;

    for (uint16_t i = 0; i < n_fields && pos < record.raw_data.size(); i++) {
        uint16_t field_no = i;
        std::vector<uint8_t> field_data;

        // 读取字段长度（1或2字节）
        uint16_t field_len = 0;
        if (pos < record.raw_data.size()) {
            uint8_t len_byte = record.raw_data[pos];
            if (len_byte & 0x80) {
                // 2字节长度
                if (pos + 1 < record.raw_data.size()) {
                    field_len = static_cast<uint16_t>(((len_byte & 0x7F) << 8) | record.raw_data[pos + 1]);
                    pos += 2;
                }
            } else {
                // 1字节长度
                field_len = len_byte;
                pos++;
            }
        }

        // 读取字段数据
        if (pos + field_len <= record.raw_data.size()) {
            field_data.assign(record.raw_data.data() + pos, record.raw_data.data() + pos + field_len);
            pos += field_len;
        }

        fields.emplace_back(field_no, field_data);
    }

    return fields;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_RECORD_CHAIN_REBUILDER_HPP
