#include "dbrecovery/innodb/redo_log_enhanced.hpp"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== RedoLogReplayer 实现 ====================

std::optional<RedoOperation> RedoLogReplayer::parse_operation(const EnhancedRedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.lsn = lsn;
    op.space_id = record.r_space_id;
    op.page_no = record.r_page_no;
    op.trx_id = record.r_trx_id;
    op.data = record.r_data;

    switch (record.r_type) {
        case MLOG_REC_INSERT:
        case MLOG_COMP_REC_INSERT:
            return parse_rec_insert(record, lsn);
        case MLOG_REC_UPDATE:
        case MLOG_COMP_REC_UPDATE:
            return parse_rec_update(record, lsn);
        case MLOG_REC_DELETE:
        case MLOG_COMP_REC_DELETE:
            return parse_rec_delete(record, lsn);
        case MLOG_PAGE_CREATE:
        case MLOG_COMP_PAGE_CREATE:
            return parse_page_create(record, lsn);
        case MLOG_PAGE_REINIT:
        case MLOG_COMP_PAGE_REINIT:
            return parse_page_reinit(record, lsn);
        case MLOG_BLOB_WRITE:
            return parse_blob_write(record, lsn);
        case MLOG_CHECKPOINT:
            return parse_checkpoint(record, lsn);
        default:
            op.type = RedoOperationType::UNKNOWN;
            return op;
    }
}

std::optional<RedoOperation> RedoLogReplayer::parse_rec_insert(const EnhancedRedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.type = RedoOperationType::INSERT;
    op.lsn = lsn;
    op.space_id = record.r_space_id;
    op.page_no = record.r_page_no;
    op.trx_id = record.r_trx_id;
    op.data = record.r_data;
    
    stats_.op_counts[RedoOperationType::INSERT]++;
    return op;
}

std::optional<RedoOperation> RedoLogReplayer::parse_rec_update(const EnhancedRedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.type = RedoOperationType::UPDATE;
    op.lsn = lsn;
    op.space_id = record.r_space_id;
    op.page_no = record.r_page_no;
    op.trx_id = record.r_trx_id;
    op.data = record.r_data;
    
    // 解析字段更新
    if (record.r_data.size() >= 4) {
        uint16_t n_fields = read_le<uint16_t>(record.r_data.data(), 0);
        size_t pos = 2;
        for (uint16_t i = 0; i < n_fields && pos + 2 <= record.r_data.size(); i++) {
            uint16_t field_no = read_le<uint16_t>(record.r_data.data(), pos);
            pos += 2;
            if (pos + 2 <= record.r_data.size()) {
                uint16_t field_len = read_le<uint16_t>(record.r_data.data(), pos);
                pos += 2;
                if (pos + field_len <= record.r_data.size()) {
                    op.field_updates[field_no] = std::vector<uint8_t>(
                        record.r_data.data() + pos, 
                        record.r_data.data() + pos + field_len
                    );
                    pos += field_len;
                }
            }
        }
    }
    
    stats_.op_counts[RedoOperationType::UPDATE]++;
    return op;
}

std::optional<RedoOperation> RedoLogReplayer::parse_rec_delete(const EnhancedRedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.type = RedoOperationType::DELETE;
    op.lsn = lsn;
    op.space_id = record.r_space_id;
    op.page_no = record.r_page_no;
    op.trx_id = record.r_trx_id;
    op.data = record.r_data;
    
    stats_.op_counts[RedoOperationType::DELETE]++;
    return op;
}

std::optional<RedoOperation> RedoLogReplayer::parse_page_create(const EnhancedRedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.type = RedoOperationType::PAGE_CREATE;
    op.lsn = lsn;
    op.space_id = record.r_space_id;
    op.page_no = record.r_page_no;
    
    // 创建新页数据
    std::vector<uint8_t> page_data(16384, 0);
    if (record.r_data.size() >= 38) {
        std::memcpy(page_data.data(), record.r_data.data(), 
                    std::min(record.r_data.size(), page_data.size()));
    }
    pages_[record.r_page_no] = std::move(page_data);
    
    stats_.op_counts[RedoOperationType::PAGE_CREATE]++;
    return op;
}

std::optional<RedoOperation> RedoLogReplayer::parse_page_reinit(const EnhancedRedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.type = RedoOperationType::PAGE_REINIT;
    op.lsn = lsn;
    op.space_id = record.r_space_id;
    op.page_no = record.r_page_no;
    
    // 重初始化页
    auto it = pages_.find(record.r_page_no);
    if (it != pages_.end()) {
        std::memset(it->second.data(), 0, it->second.size());
    }
    
    stats_.op_counts[RedoOperationType::PAGE_REINIT]++;
    return op;
}

std::optional<RedoOperation> RedoLogReplayer::parse_blob_write(const EnhancedRedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.type = RedoOperationType::BLOB_WRITE;
    op.lsn = lsn;
    op.space_id = record.r_space_id;
    op.page_no = record.r_page_no;
    op.data = record.r_data;
    
    stats_.op_counts[RedoOperationType::BLOB_WRITE]++;
    return op;
}

std::optional<RedoOperation> RedoLogReplayer::parse_checkpoint(const EnhancedRedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.type = RedoOperationType::CHECKPOINT;
    op.lsn = lsn;
    
    // 解析检查点信息
    if (record.r_data.size() >= 8) {
        op.data = record.r_data;
    }
    
    stats_.op_counts[RedoOperationType::CHECKPOINT]++;
    return op;
}

bool RedoLogReplayer::replay_by_lsn() {
    // 按 LSN 排序
    std::sort(records_.begin(), records_.end(),
        [](const EnhancedRedoLogRecord& a, const EnhancedRedoLogRecord& b) {
            return a.r_lsn < b.r_lsn;
        });

    for (const auto& record : records_) {
        auto op = parse_operation(record, record.r_lsn);
        if (op.has_value()) {
            operations_.push_back(op.value());
            stats_.applied_records++;
        } else {
            stats_.failed_records++;
        }
    }

    stats_.operations = operations_.size();
    return !operations_.empty();
}

bool RedoLogReplayer::replay_by_page() {
    // 按页号分组
    std::map<uint32_t, std::vector<const EnhancedRedoLogRecord*>> page_records;
    for (const auto& record : records_) {
        page_records[record.r_page_no].push_back(&record);
    }

    // 按页号排序
    std::vector<uint32_t> page_nos;
    for (const auto& [page_no, _] : page_records) {
        page_nos.push_back(page_no);
    }
    std::sort(page_nos.begin(), page_nos.end());

    // 按页号回放
    for (uint32_t page_no : page_nos) {
        const auto& recs = page_records[page_no];
        for (const auto* record : recs) {
            auto op = parse_operation(*record, record->r_lsn);
            if (op.has_value()) {
                operations_.push_back(op.value());
                stats_.applied_records++;
            } else {
                stats_.failed_records++;
            }
        }
    }

    stats_.operations = operations_.size();
    return !operations_.empty();
}

// ==================== RedoLogAnalyzer 实现 ====================

bool RedoLogAnalyzer::analyze_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    // 读取文件
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);

    // 解析记录
    size_t pos = 0;
    while (pos + 9 <= buffer.size()) {
        RedoLogRecord record;
        record.r_type = read_le<uint8_t>(buffer.data(), pos);
        pos += sizeof(uint8_t);

        if (pos + 8 <= buffer.size()) {
            record.r_space_id = read_le<uint32_t>(buffer.data(), pos);
            pos += sizeof(uint32_t);
            record.r_page_no = read_le<uint32_t>(buffer.data(), pos);
            pos += sizeof(uint32_t);
        }

        // 更新分析信息
        stats_.total_records++;
        stats_.record_type_counts[record.r_type]++;
        modified_spaces_.insert(record.r_space_id);
        modified_pages_.insert(record.r_page_no);

        // 解析事务信息（如果存在）
        if (record.r_type == MLOG_REC_INSERT || record.r_type == MLOG_REC_UPDATE) {
            if (pos + 8 <= buffer.size()) {
                uint64_t trx_id = read_le<uint64_t>(buffer.data(), pos);
                update_transaction_info(trx_id, 0, record.r_space_id, record.r_page_no, false);
                pos += sizeof(uint64_t);
            }
        }
    }

    stats_.unique_transactions = transactions_.size();
    stats_.unique_spaces = modified_spaces_.size();
    stats_.unique_pages = modified_pages_.size();

    return true;
}

bool RedoLogAnalyzer::analyze_records(const std::vector<RedoLogRecord>& records) {
    for (const auto& record : records) {
        stats_.total_records++;
        stats_.record_type_counts[record.r_type]++;
        modified_spaces_.insert(record.r_space_id);
        modified_pages_.insert(record.r_page_no);

        // 更新事务信息
        if (record.r_type == MLOG_REC_INSERT || record.r_type == MLOG_REC_UPDATE) {
            update_transaction_info(record.r_trx_id, 0, record.r_space_id, record.r_page_no, false);
        }
    }

    stats_.unique_transactions = transactions_.size();
    stats_.unique_spaces = modified_spaces_.size();
    stats_.unique_pages = modified_pages_.size();

    return true;
}

} // namespace innodb
} // namespace dbrecovery
