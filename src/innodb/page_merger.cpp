/**
 * @file page_merger.cpp
 * @brief Page merger for recovering data from corrupted/damaged IBD files
 * 
 * 当 IBD 文件损坏时（如 rm -rf 后恢复的备份），需要合并相邻页的数据
 * 本模块提供页级数据恢复和合并功能
 */

#include "dbrecovery/innodb/page_merger.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <sstream>

namespace dbrecovery {
namespace innodb {

// ==================== Page Merger Implementation ====================

std::vector<HeuristicRecord> PageMerger::merge_pages(
    const std::vector<std::vector<uint8_t>>& pages,
    uint32_t page_size) {
    
    std::vector<HeuristicRecord> merged_records;
    std::set<uint16_t> seen_offsets;
    
    HeuristicParser parser;
    
    for (const auto& page : pages) {
        if (page.size() < page_size) {
            continue;
        }
        
        auto records = parser.parse_all_records(
            page.data(), page.size(),
            read_le<uint16_t>(page.data(), 40));
        
        // Filter and deduplicate
        for (const auto& rec : records) {
            if (rec.is_infimum || rec.is_supremum) {
                continue;
            }
            
            // Skip duplicates
            if (seen_offsets.count(rec.offset)) {
                continue;
            }
            seen_offsets.insert(rec.offset);
            
            merged_records.push_back(rec);
        }
    }
    
    // Sort by offset
    std::sort(merged_records.begin(), merged_records.end(),
              [](const auto& a, const auto& b) {
                  return a.offset < b.offset;
              });
    
    return merged_records;
}

std::vector<HeuristicRecord> PageMerger::merge_page_chunks(
    const std::vector<uint8_t>& file_data,
    uint32_t page_size,
    const std::vector<std::pair<uint32_t, uint32_t>>& chunk_ranges) {
    
    std::vector<HeuristicRecord> all_records;
    std::set<uint16_t> seen_offsets;
    
    HeuristicParser parser;
    
    for (const auto& [page_no, offset] : chunk_ranges) {
        size_t start = static_cast<size_t>(page_no) * page_size + offset;
        if (start + page_size > file_data.size()) {
            continue;
        }
        
        auto records = parser.parse_all_records(
            file_data.data() + start, page_size,
            read_le<uint16_t>(file_data.data() + start, 40));
        
        for (const auto& rec : records) {
            if (rec.is_infimum || rec.is_supremum || seen_offsets.count(rec.offset)) {
                continue;
            }
            seen_offsets.insert(rec.offset);
            all_records.push_back(rec);
        }
    }
    
    return all_records;
}

// ==================== Page Recovery Strategy ====================

// RecoveryStrategy is defined in the header file to avoid duplicate definitions
// This file only uses the enum defined in page_merger.hpp

RecoveryStrategy PageMerger::detect_recovery_strategy(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100) {
        return RecoveryStrategy::DEFAULT;
    }
    
    // Check page type
    uint16_t page_type = get_page_type(page_data);
    if (page_type != FIL_PAGE_INDEX) {
        return RecoveryStrategy::HEURISTIC;
    }
    
    // Check for standard infimum/supremum markers
    std::string page_content(reinterpret_cast<const char*>(page_data), 
                            std::min(size_t(1024), page_size));
    
    bool has_infimum = page_content.find("infimum") != std::string::npos;
    bool has_supremum = page_content.find("supremum") != std::string::npos;
    
    if (!has_infimum || !has_supremum) {
        return RecoveryStrategy::HEURISTIC;
    }
    
    // Check page header validity
    uint16_t heap_top = read_le<uint16_t>(page_data, 40);
    uint16_t n_dir_slots = read_le<uint16_t>(page_data, 38);
    
    if (heap_top > page_size || n_dir_slots > 1000) {
        return RecoveryStrategy::HEURISTIC;
    }
    
    return RecoveryStrategy::DEFAULT;
}

std::vector<HeuristicRecord> PageMerger::recover_page_with_strategy(
    const uint8_t* page_data,
    size_t page_size,
    uint32_t page_no,
    RecoveryStrategy strategy) {
    
    switch (strategy) {
        case RecoveryStrategy::DEFAULT:
            return recover_page_standard(page_data, page_size, page_no);
            
        case RecoveryStrategy::HEURISTIC:
            return recover_page_heuristic(page_data, page_size, page_no);
            
        case RecoveryStrategy::PATTERN:
            return recover_page_pattern(page_data, page_size, page_no);
            
        case RecoveryStrategy::MERGE:
            return recover_page_merged(page_data, page_size, page_no);
            
        case RecoveryStrategy::BACKWARD:
            return recover_page_backward(page_data, page_size, page_no);
            
        default:
            return recover_page_standard(page_data, page_size, page_no);
    }
}

// ==================== Individual Recovery Methods ====================

std::vector<HeuristicRecord> PageMerger::recover_page_standard(
    const uint8_t* page_data,
    size_t page_size,
    uint32_t page_no) {
    
    std::vector<HeuristicRecord> records;
    
    if (!page_data || page_size < 100) {
        return records;
    }
    
    uint16_t heap_top = read_le<uint16_t>(page_data, 40);
    
    HeuristicParser parser;
    auto page_records = parser.parse_all_records(page_data, page_size, heap_top);
    
    for (const auto& rec : page_records) {
        if (!rec.is_infimum && !rec.is_supremum && !rec.is_deleted) {
            records.push_back(rec);
        }
    }
    
    return records;
}

std::vector<HeuristicRecord> PageMerger::recover_page_heuristic(
    const uint8_t* page_data,
    size_t page_size,
    uint32_t page_no) {

    // Use SchemaAgnosticScanner for pattern-agnostic recovery
    innodb::SchemaAgnosticScanner::Config sa_config;
    sa_config.recover_deleted = true;
    innodb::SchemaAgnosticScanner sa_scanner(sa_config);

    auto result = sa_scanner.parse_page(page_data, page_size, page_no);

    std::vector<HeuristicRecord> records;
    for (const auto& sa_rec : result.records) {
        if (sa_rec.is_deleted || !sa_rec.is_valid) {
            continue;
        }

        HeuristicRecord rec;
        rec.offset = sa_rec.record_offset;
        rec.is_infimum = false;
        rec.is_supremum = false;
        rec.is_deleted = sa_rec.is_deleted;

        // Convert SchemalessField to HeuristicField
        for (const auto& sa_field : sa_rec.fields) {
            HeuristicField field;
            field.is_null = sa_field.is_null;
            field.is_valid = sa_field.type != innodb::SchemalessField::Type::UNKNOWN;

            // Copy raw bytes
            field.data = sa_field.raw_bytes;

            // Determine type - use LONGLONG for both int and float/double as storage is 8 bytes
            switch (sa_field.type) {
                case innodb::SchemalessField::Type::INT_VAL:
                case innodb::SchemalessField::Type::DOUBLE_VAL:
                case innodb::SchemalessField::Type::FLOAT_VAL:
                    field.type = HeuristicFieldType::LONGLONG;
                    break;
                default:
                    field.type = HeuristicFieldType::VARCHAR_SHORT;
                    break;
            }

            rec.fields.push_back(field);
        }

        records.push_back(rec);
    }

    return records;
}

std::vector<HeuristicRecord> PageMerger::recover_page_pattern(
    const uint8_t* page_data,
    size_t page_size,
    uint32_t page_no) {

    // Delegate to recover_page_heuristic which now uses SchemaAgnosticScanner
    return recover_page_heuristic(page_data, page_size, page_no);
}

std::vector<HeuristicRecord> PageMerger::recover_page_merged(
    const uint8_t* page_data,
    size_t page_size,
    uint32_t page_no) {
    
    // Try to merge with adjacent pages if this page is damaged
    // This is useful when pages are partially overwritten
    
    std::vector<HeuristicRecord> records;
    
    // Parse current page
    auto current_records = recover_page_standard(page_data, page_size, page_no);
    
    // Look for overlapping data patterns
    // If page is partially damaged, try to find records in corrupted areas
    
    // Check for record-like patterns in page tail (often contains fragments)
    for (size_t i = page_size - 1024; i + 20 < page_size; i++) {
        if (page_data[i] == 'u' && page_data[i+1] == 's') {
            // Possible record start
            HeuristicRecord rec;
            rec.offset = static_cast<uint16_t>(i);
            rec.is_infimum = false;
            rec.is_supremum = false;
            rec.is_deleted = false;
            
            // Extract whatever we can find
            size_t end = i;
            while (end < page_size && page_data[end] != 0 && 
                   page_data[end] != 0xFF && end - i < 256) {
                end++;
            }
            
            if (end > i + 10) {
                HeuristicField field;
                field.type = HeuristicFieldType::VARCHAR_SHORT;
                field.data.assign(page_data + i, page_data + end);
                field.is_null = false;
                field.is_valid = true;
                rec.fields.push_back(field);
                
                records.push_back(rec);
            }
        }
    }
    
    // Add current page records
    records.insert(records.end(), current_records.begin(), current_records.end());
    
    return records;
}

std::vector<HeuristicRecord> PageMerger::recover_page_backward(
    const uint8_t* page_data,
    size_t page_size,
    uint32_t page_no) {
    
    // Scan backward from page end to find records
    // This catches records that were partially overwritten at the start
    
    std::vector<HeuristicRecord> records;
    
    //扫描结尾部分（.record 可能残留）
    for (size_t i = page_size - 100; i > 100; i--) {
        if (page_data[i] == 'e' && page_data[i+1] == 'c' && 
            page_data[i+2] == 'o' && page_data[i+3] == 'r') {
            // "record" ending found
            
            // Find start of record
            size_t start = i;
            while (start > 0 && page_data[start - 1] >= 32) {
                start--;
            }
            
            if (i - start > 10) {
                HeuristicRecord rec;
                rec.offset = static_cast<uint16_t>(start);
                rec.is_infimum = false;
                rec.is_supremum = false;
                rec.is_deleted = false;
                
                HeuristicField field;
                field.type = HeuristicFieldType::VARCHAR_SHORT;
                field.data.assign(page_data + start, page_data + i + 4);
                field.is_null = false;
                field.is_valid = true;
                rec.fields.push_back(field);
                
                records.push_back(rec);
                
                i = start;  // Continue scanning
            }
        }
    }
    
    return records;
}

// ==================== Multi-Page Recovery ====================

std::vector<HeuristicRecord> PageMerger::recover_from_multiple_files(
    const std::vector<std::string>& file_paths,
    uint32_t page_size) {
    
    std::vector<HeuristicRecord> all_records;
    std::set<uint16_t> seen_offsets;
    
    for (const auto& file_path : file_paths) {
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            std::cerr << "[WARN] Cannot open file: " << file_path << "\n";
            continue;
        }
        
        // Get file size
        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        uint32_t total_pages = static_cast<uint32_t>((file_size + page_size - 1) / page_size);
        
        std::vector<uint8_t> page_buffer(page_size);
        
        for (uint32_t page_no = 0; page_no < total_pages; page_no++) {
            file.seekg(static_cast<std::streamoff>(page_no) * page_size, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_buffer.data()), page_size);
            
            // Try multiple recovery strategies
            RecoveryStrategy strategy = detect_recovery_strategy(
                page_buffer.data(), page_buffer.size());
            
            auto records = recover_page_with_strategy(
                page_buffer.data(), page_buffer.size(), page_no, strategy);
            
            for (const auto& rec : records) {
                if (!rec.is_infimum && !rec.is_supremum && !seen_offsets.count(rec.offset)) {
                    seen_offsets.insert(rec.offset);
                    all_records.push_back(rec);
                }
            }
        }
    }
    
    return all_records;
}

// ==================== Cross-Page Record Reconstruction ====================

std::optional<HeuristicRecord> PageMerger::reconstruct_record_from_pages(
    const std::vector<std::vector<uint8_t>>& pages,
    uint16_t expected_offset,
    uint32_t page_size) {
    
    // Try to find record with specific offset across multiple pages
    for (const auto& page : pages) {
        if (page.size() < page_size) continue;
        
        HeuristicParser parser;
        auto records = parser.parse_all_records(page.data(), page_size, 
                                               read_le<uint16_t>(page.data(), 40));
        
        for (const auto& rec : records) {
            if (rec.offset == expected_offset && !rec.is_infimum && !rec.is_supremum) {
                return rec;
            }
        }
    }
    
    return std::nullopt;
}

// ==================== damaged page repair ====================

std::vector<uint8_t> PageMerger::repair_damaged_page(
    const std::vector<uint8_t>& page_data,
    uint32_t page_size) {
    
    if (page_data.size() < page_size) {
        return page_data;
    }
    
    std::vector<uint8_t> repaired = page_data;
    
    // Repair page header if damaged
    // Page header starts at offset 38 (FIL_HEADER_SIZE)
    
    // Check if heap_top is valid
    uint16_t heap_top = read_le<uint16_t>(repaired.data(), 40);
    if (heap_top > page_size || heap_top < 100) {
        // Try to estimate heap_top from page content
        // Look for the first 0xFF 0xFF (end of records)
        for (size_t i = 100; i + 1 < page_size; i++) {
            if (repaired[i] == 0xFF && repaired[i+1] == 0xFF) {
                heap_top = static_cast<uint16_t>(i);
                write_le<uint16_t>(repaired.data(), 40, heap_top);
                break;
            }
        }
    }
    
    // Check n_dir_slots
    uint16_t n_dir_slots = read_le<uint16_t>(repaired.data(), 38);
    if (n_dir_slots > 1000 || n_dir_slots == 0) {
        // Estimate from heap_top (approximately 1 slot per 16 records)
        n_dir_slots = static_cast<uint16_t>((heap_top / 16 + 15) / 16);
        write_le<uint16_t>(repaired.data(), 38, n_dir_slots);
    }
    
    return repaired;
}

// ==================== Statistics and Reporting ====================

std::string PageMerger::get_recovery_report(
    const std::vector<HeuristicRecord>& records,
    const std::vector<RecoveryResult>& results) {
    
    std::ostringstream report;
    report << "=== Page Recovery Report ===\n\n";
    
    report << "Total records recovered: " << records.size() << "\n";
    
    // Group by page
    std::map<uint32_t, int> records_per_page;
    for (const auto& rec : records) {
        // Estimate page number from offset
        uint32_t page_no = rec.offset / 16384;
        records_per_page[page_no]++;
    }
    
    report << "\nRecords per page:\n";
    for (const auto& [page_no, count] : records_per_page) {
        if (count > 0) {
            report << "  Page " << page_no << ": " << count << " records\n";
        }
    }
    
    // Recovery strategies used
    report << "\nRecovery results by strategy:\n";
    for (const auto& result : results) {
        report << "  Strategy " << static_cast<int>(result.strategy) 
               << ": " << result.records_found << " records\n";
    }
    
    return report.str();
}

} // namespace innodb
} // namespace dbrecovery
