#include "dbrecovery/recovery/rmrf_recovery.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include "dbrecovery/disk/enhanced_scanner.hpp"
#include "dbrecovery/engine/signature_index.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <set>
#include <filesystem>
#include <algorithm>

namespace dbrecovery {
namespace recovery {

// ==================== RmrfRecovery 实现 ====================

RmrfRecovery::RmrfRecovery() : page_size_(16384) {}

RmrfRecovery::~RmrfRecovery() = default;

void RmrfRecovery::set_page_size(uint32_t page_size) {
    if (page_size == 4096 || page_size == 8192 || page_size == 16384 ||
        page_size == 32768 || page_size == 65536) {
        page_size_ = page_size;
    }
}

// ✅ P1-3 实现：目录扫描功能
bool RmrfRecovery::scan_directory(const std::string& dir_path) {
    std::cout << "[INFO] RmrfRecovery::scan_directory: Scanning directory: " << dir_path << std::endl;

    // ✅ Step 1: 验证目录是否存在
    std::filesystem::path dir(dir_path);
    if (!std::filesystem::exists(dir)) {
        std::cerr << "[ERROR] RmrfRecovery: Directory does not exist: " << dir_path << std::endl;
        return false;
    }

    if (!std::filesystem::is_directory(dir)) {
        std::cerr << "[ERROR] RmrfRecovery: Path is not a directory: " << dir_path << std::endl;
        return false;
    }

    // ✅ Step 2: 递归扫描目录，查找所有 .ibd 文件
    std::vector<std::string> ibd_files;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                // 检查是否为 .ibd 文件
                if (filename.size() > 4 && 
                    filename.substr(filename.size() - 4) == ".ibd") {
                    ibd_files.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[ERROR] RmrfRecovery: Failed to scan directory: " << e.what() << std::endl;
        return false;
    }

    if (ibd_files.empty()) {
        std::cout << "[WARN] RmrfRecovery: No .ibd files found in directory: " << dir_path << std::endl;
        return false;
    }

    std::cout << "[INFO] RmrfRecovery: Found " << ibd_files.size() << " .ibd files" << std::endl;

    // ✅ Step 3: 对每个 .ibd 文件进行扫描和分析
    for (const auto& file_path : ibd_files) {
        std::cout << "[INFO] Scanning file: " << file_path << std::endl;

        // 读取文件
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            std::cerr << "[WARN] Failed to open file: " << file_path << std::endl;
            continue;
        }

        // 获取文件大小
        file.seekg(0, std::ios::end);
        uint64_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        // 计算总页数
        uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);

        if (total_pages == 0) {
            std::cout << "[WARN] File too small: " << file_path << std::endl;
            continue;
        }

        // 统计页类型分布
        std::map<uint16_t, uint32_t> page_type_count;
        uint32_t valid_index_pages = 0;

        std::vector<uint8_t> page_data(page_size_);

        for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
            file.seekg(page_no * page_size_, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

            // 检查页类型
            uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
            page_type_count[page_type]++;

            if (page_type == innodb::FIL_PAGE_INDEX) {
                valid_index_pages++;
            }
        }

        std::cout << "[INFO] File: " << file_path << std::endl;
        std::cout << "  - Total pages: " << total_pages << std::endl;
        std::cout << "  - Valid index pages: " << valid_index_pages << std::endl;
        std::cout << "  - Page type distribution:" << std::endl;

        for (const auto& [type, count] : page_type_count) {
            std::cout << "    - " << innodb::page_type_to_string(type) << ": " << count << std::endl;
        }

        // 保存文件信息
        scanned_files_.push_back({
            file_path,
            file_size,
            total_pages,
            valid_index_pages,
            page_type_count
        });
    }

    std::cout << "[INFO] RmrfRecovery: Directory scan completed. Found " 
              << scanned_files_.size() << " files with " 
              << get_total_index_pages() << " index pages" << std::endl;

    return !scanned_files_.empty();
}

// ✅ P1-3 实现：原始设备扫描功能
bool RmrfRecovery::scan_raw_device(const std::string& device_path) {
    std::cout << "[INFO] RmrfRecovery::scan_raw_device: Scanning device: " << device_path << std::endl;

    // 以二进制模式打开设备
    std::ifstream device_file(device_path, std::ios::binary);
    if (!device_file) {
        std::cerr << "[ERROR] RmrfRecovery: Failed to open device: " << device_path << std::endl;
        return false;
    }

    // 获取设备大小
    device_file.seekg(0, std::ios::end);
    uint64_t device_size = device_file.tellg();
    device_file.seekg(0, std::ios::beg);

    if (device_size < page_size_) {
        std::cerr << "[ERROR] RmrfRecovery: Device too small: " << device_size << " bytes" << std::endl;
        return false;
    }

    uint32_t total_pages = static_cast<uint32_t>(device_size / page_size_);
    std::cout << "[INFO] Device size: " << device_size << " bytes (" << total_pages << " pages)" << std::endl;

    // 逐页读取并验证
    std::vector<uint8_t> page_data(page_size_);
    uint32_t valid_pages = 0;
    uint32_t index_pages = 0;
    uint32_t sdi_pages = 0;
    uint32_t undo_pages = 0;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        device_file.seekg(page_no * page_size_, std::ios::beg);
        device_file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        // 检查页类型
        uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

        if (page_type == innodb::FIL_PAGE_INDEX) {
            index_pages++;
            valid_pages++;
        } else if (page_type == innodb::FIL_PAGE_SDI) {
            sdi_pages++;
            valid_pages++;
        } else if (page_type == innodb::FIL_PAGE_UNDO_LOG) {
            undo_pages++;
            valid_pages++;
        } else if (page_type == innodb::FIL_PAGE_TYPE_ALLOCATED) {
            valid_pages++;
        }

        // 进度输出（每 10000 页）
        if (page_no % 10000 == 0) {
            std::cout << "[INFO] Scanned " << page_no << "/" << total_pages << " pages" << std::endl;
        }
    }

    std::cout << "[INFO] Device scan completed:" << std::endl;
    std::cout << "  - Total pages: " << total_pages << std::endl;
    std::cout << "  - Valid pages: " << valid_pages << std::endl;
    std::cout << "  - Index pages: " << index_pages << std::endl;
    std::cout << "  - SDI pages: " << sdi_pages << std::endl;
    std::cout << "  - Undo pages: " << undo_pages << std::endl;

    // 保存设备信息
    ScannedDevice dev_info;
    dev_info.device_path = device_path;
    dev_info.device_size = device_size;
    dev_info.total_pages = total_pages;
    dev_info.valid_pages = valid_pages;
    dev_info.page_type_count[innodb::FIL_PAGE_INDEX] = index_pages;
    dev_info.page_type_count[innodb::FIL_PAGE_SDI] = sdi_pages;
    dev_info.page_type_count[innodb::FIL_PAGE_UNDO_LOG] = undo_pages;
    scanned_devices_.push_back(dev_info);

    return valid_pages > 0;
}

// ✅ P1-3 新增：获取总索引页数
uint32_t RmrfRecovery::get_total_index_pages() const {
    uint32_t total = 0;
    for (const auto& file : scanned_files_) {
        total += file.valid_index_pages;
    }
    return total;
}

// ✅ P1-3 新增：获取扫描的文件列表
const std::vector<ScannedFile>& RmrfRecovery::get_scanned_files() const {
    return scanned_files_;
}

// ✅ P1-3 新增：获取扫描的设备列表
const std::vector<ScannedDevice>& RmrfRecovery::get_scanned_devices() const {
    return scanned_devices_;
}

std::vector<std::map<std::string, std::string>>
RmrfRecovery::recover_records(const std::vector<uint8_t>& page_data,
                               const catalog::TableSchema& schema) {
    std::vector<std::map<std::string, std::string>> records;

    // 检查页类型
    uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
    if (page_type != innodb::FIL_PAGE_INDEX) {
        std::cerr << "[WARN] RmrfRecovery: Page is not an index page" << std::endl;
        return records;
    }

    // 解析 B+Tree 页
    innodb::BPlusTreeParser parser(page_data);
    if (!parser.parse()) {
        std::cerr << "[WARN] RmrfRecovery: Failed to parse page" << std::endl;
        return records;
    }

    // 解析记录
    records = parser.parse_records(schema);

    return records;
}

bool RmrfRecovery::reconstruct_from_pages(
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages,
    const catalog::TableSchema& schema,
    std::vector<std::map<std::string, std::string>>& recovered_records) {
    
    if (pages.empty()) {
        std::cerr << "[WARN] RmrfRecovery: No pages to reconstruct" << std::endl;
        return false;
    }

    // 按页号排序
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> sorted_pages = pages;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // 遍历所有页并提取记录
    std::set<uint32_t> visited_pages;
    for (const auto& [page_no, page_data] : sorted_pages) {
        // 跳过已访问的页（防止循环）
        if (visited_pages.find(page_no) != visited_pages.end()) {
            continue;
        }
        visited_pages.insert(page_no);

        // 检查页类型
        uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
        if (page_type != innodb::FIL_PAGE_INDEX) {
            continue;
        }

        // 解析 B+Tree 页
        innodb::BPlusTreeParser parser(page_data);
        if (!parser.parse()) {
            std::cerr << "[WARN] RmrfRecovery: Failed to parse page " << page_no << std::endl;
            continue;
        }

        // 解析记录
        auto page_records = parser.parse_records(schema);
        recovered_records.insert(recovered_records.end(),
                                 page_records.begin(), page_records.end());
    }

    std::cout << "[INFO] RmrfRecovery: Recovered " << recovered_records.size() 
              << " records from " << visited_pages.size() << " pages" << std::endl;

    return !recovered_records.empty();
}

// ✅ P1-3 新增：基于启发式扫描的记录提取（当 B+Tree 解析失败时）
std::vector<std::map<std::string, std::string>>
RmrfRecovery::extract_records_heuristic(const std::vector<uint8_t>& page_data,
                                         const catalog::TableSchema& schema) {
    std::vector<std::map<std::string, std::string>> records;
    std::set<std::string> seen_keys;

    // 使用 SchemaAgnosticScanner 进行无模式匹配的启发式扫描
    innodb::SchemaAgnosticScanner::Config sa_config;
    sa_config.recover_deleted = true;
    innodb::SchemaAgnosticScanner sa_scanner(sa_config);

    // Parse the page
    auto result = sa_scanner.parse_page(page_data.data(), page_data.size(), 0);

    // 将 SchemalessRecord 转换为 std::map<std::string, std::string>
    for (const auto& sa_rec : result.records) {
        if (sa_rec.is_deleted || !sa_rec.is_valid) {
            continue;
        }

        if (sa_rec.fields.empty()) {
            continue;
        }

        // 构建记录
        std::map<std::string, std::string> rec;

        // Use record_id as ID if available
        if (sa_rec.record_id > 0) {
            rec["id"] = std::to_string(sa_rec.record_id);
        }

        // Build record from schemaless fields
        // Use schema column names if available, otherwise use generic names
        for (size_t i = 0; i < sa_rec.fields.size(); i++) {
            std::string key;

            // Try to get column name from schema (1-indexed in schema.columns)
            if (schema.columns.size() > i) {
                auto it = schema.columns.find(static_cast<int>(i + 1));
                if (it != schema.columns.end()) {
                    key = it->second.name;
                }
            }

            // Fallback to generic field name if no schema column found
            if (key.empty()) {
                char keybuf[16];
                snprintf(keybuf, sizeof(keybuf), "field_%04zu", i);
                key = keybuf;
            }

            std::string value = sa_rec.fields[i].to_string();

            // Skip garbage values
            if (value.find("???") != std::string::npos ||
                value.find("supremum") != std::string::npos ||
                value.find("infimum") != std::string::npos) {
                continue;
            }

            rec[key] = value;
        }

        // Dedup by record_id
        std::string dedup_key = std::to_string(sa_rec.record_id) + "_" + std::to_string(sa_rec.page_no);
        if (seen_keys.find(dedup_key) == seen_keys.end()) {
            seen_keys.insert(dedup_key);
            records.push_back(rec);
        }
    }

    return records;
}

bool RmrfRecovery::reconstruct_from_file(const std::string& file_path,
                                          const catalog::TableSchema& schema,
                                          std::vector<std::map<std::string, std::string>>& recovered_records) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] RmrfRecovery: Failed to open file: " << file_path << std::endl;
        return false;
    }

    // 获取文件大小
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 计算总页数
    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);

    if (total_pages == 0) {
        std::cerr << "[WARN] RmrfRecovery: File is too small" << std::endl;
        return false;
    }

    // 优化的页类型检测和扫描模式选择
    // 目标：自动检测最佳扫描策略
    std::vector<uint8_t> page_data(page_size_);
    uint32_t index_pages = 0;
    uint32_t sdi_pages = 0;
    uint32_t undo_pages = 0;
    uint32_t allocated_pages = 0;
    uint32_t corrupted_pages = 0;
    
    // 采样前100页和最后100页来检测页类型分布
    uint32_t sample_pages = std::min(total_pages, 200u);
    uint32_t sample_step = (total_pages + sample_pages - 1) / sample_pages;
    
    std::cout << "[INFO] RmrfRecovery: Analyzing page type distribution (sample " 
              << sample_pages << " pages) ..." << std::endl;
    
    // 采样扫描
    for (uint32_t i = 0; i < sample_pages; ++i) {
        uint32_t page_no = (i * sample_step) % total_pages;
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
        
        switch (page_type) {
            case innodb::FIL_PAGE_INDEX:
                index_pages++;
                break;
            case innodb::FIL_PAGE_SDI:
                sdi_pages++;
                break;
            case innodb::FIL_PAGE_UNDO_LOG:
                undo_pages++;
                break;
            case innodb::FIL_PAGE_TYPE_ALLOCATED:
                allocated_pages++;
                break;
            default:
                // Check if it looks like a corrupted index page (invalid type but looks like data)
                if (page_type == innodb::FIL_PAGE_TYPE_BLOB || page_type == 0) {
                    index_pages++;  // Treat as potentially valid index page
                } else {
                    corrupted_pages++;
                }
                break;
        }
    }
    
    // 计算总体统计
    double index_ratio = static_cast<double>(index_pages) / sample_pages;
    double corrupt_ratio = static_cast<double>(corrupted_pages) / sample_pages;
    
    std::cout << "[INFO] RmrfRecovery: Page analysis results:" << std::endl;
    std::cout << "  - Index pages: " << index_pages << " (" 
              << std::fixed << std::setprecision(1) << (index_ratio * 100) << "%)" << std::endl;
    std::cout << "  - SDI pages: " << sdi_pages << std::endl;
    std::cout << "  - Undo pages: " << undo_pages << std::endl;
    std::cout << "  - Allocated pages: " << allocated_pages << std::endl;
    std::cout << "  - Corrupted/Invalid: " << corrupted_pages << " (" 
              << (corrupt_ratio * 100) << "%)" << std::endl;

    // 智能模式选择逻辑
    bool use_heuristic = false;
    std::string recovery_mode;
    
    if (corrupt_ratio > 0.3) {
        // 严重损坏 - 强制使用启发式扫描
        use_heuristic = true;
        recovery_mode = "HEURISTIC_FORCE ( severe corruption detected )";
    } else if (index_ratio < 0.5) {
        // 大量页类型异常 - 启发式扫描
        use_heuristic = true;
        recovery_mode = "HEURISTIC_AUTO (low index page ratio)";
    } else {
        // 主要是索引页 - 尝试 B+Tree 解析
        use_heuristic = false;
        recovery_mode = "BTREE_STANDARD (healthy index pages)";
    }
    
    // 如果文件曾被 TRUNCATE，B+Tree 链表可能损坏，强制使用启发式
    // 检测：检查前几页是否有完整的记录链表
    if (!use_heuristic && total_pages > 0) {
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);
        
        // 检查页头中的记录数量
        uint16_t n_dir_slots = innodb::read_le<uint16_t>(page_data.data(), 38);
        uint16_t heap_top = innodb::read_le<uint16_t>(page_data.data(), 40);
        
        // 如果 heap_top 太小（少于100字节），可能数据已被清空
        if (heap_top < 100) {
            use_heuristic = true;
            recovery_mode = "HEURISTIC_TRUNCATE (small heap_top indicates TRUNCATE)";
        }
    }
    
    std::cout << "[INFO] RmrfRecovery: Selected recovery mode: " << recovery_mode << std::endl;

    // 重置文件指针
    file.seekg(0, std::ios::beg);

    uint32_t recovered_pages = 0;
    uint32_t heuristic_scanned = 0;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        // 如果使用 heuristic 模式，直接扫描所有页
        if (use_heuristic) {
            auto page_records = extract_records_heuristic(page_data, schema);
            recovered_records.insert(recovered_records.end(),
                                     page_records.begin(), page_records.end());
            heuristic_scanned++;
        } else {
            // 否则使用标准 B+Tree 解析
            uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
            if (page_type != innodb::FIL_PAGE_INDEX && page_type != innodb::FIL_PAGE_TYPE_BLOB) {
                continue;
            }

            // 尝试 B+Tree 解析
            innodb::BPlusTreeParser parser(page_data);
            if (!parser.parse()) {
                // B+Tree 解析失败，尝试启发式补充
                auto page_records = extract_records_heuristic(page_data, schema);
                recovered_records.insert(recovered_records.end(),
                                         page_records.begin(), page_records.end());
                recovered_pages++;
                continue;
            }

            // 解析记录
            auto page_records = parser.parse_records(schema);
            if (page_records.empty()) {
                // B+Tree 解析成功但没有记录，尝试启发式补充
                auto page_records_heur = extract_records_heuristic(page_data, schema);
                recovered_records.insert(recovered_records.end(),
                                         page_records_heur.begin(), page_records_heur.end());
            } else {
                recovered_records.insert(recovered_records.end(),
                                         page_records.begin(), page_records.end());
            }
            recovered_pages++;
        }
    }

    // 输出恢复统计信息
    std::cout << "[INFO] RmrfRecovery: Recovery completed." << std::endl;
    std::cout << "  - Pages scanned: " << (use_heuristic ? heuristic_scanned : recovered_pages) << std::endl;
    std::cout << "  - Records recovered: " << recovered_records.size() << std::endl;
    std::cout << "  - Mode: " << recovery_mode << std::endl;

    if (use_heuristic) {
        std::cout << "[INFO] RmrfRecovery: Heuristic scan completed. Scanned "
                  << heuristic_scanned << " pages, recovered "
                  << recovered_records.size() << " records" << std::endl;
    } else {
        std::cout << "[INFO] RmrfRecovery: Recovered " << recovered_records.size()
                  << " records from " << recovered_pages << " pages" << std::endl;
    }

    return !recovered_records.empty();
}

// ==================== 全分区扫描恢复 ====================

std::vector<std::map<std::string, std::string>>
RmrfRecovery::full_partition_recovery(const std::string& file_path) {
    std::cout << "[INFO] RmrfRecovery::full_partition_recovery: Starting full partition scan" << std::endl;
    std::cout << "[INFO] File: " << file_path << std::endl;

    std::vector<std::map<std::string, std::string>> recovered_records;
    std::set<std::string> seen_records;

    // Open and read entire file
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[ERROR] RmrfRecovery: Failed to open file: " << file_path << std::endl;
        return recovered_records;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        std::cerr << "[ERROR] RmrfRecovery: File too small: " << file_size << " bytes" << std::endl;
        return recovered_records;
    }

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
    std::cout << "[INFO] Total pages: " << total_pages << std::endl;

    // Read entire file into memory
    std::vector<uint8_t> file_data(file_size);
    file.read(reinterpret_cast<char*>(file_data.data()), file_size);
    file.close();

    // Scan each page
    uint32_t index_pages = 0;
    uint32_t sdi_pages = 0;
    uint32_t undo_pages = 0;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        uint32_t offset = page_no * page_size_;

        // Extract page header - use innodb::read_le
        if (offset + 46 > file_size) {
            break;
        }

        uint16_t page_type = innodb::read_be<uint16_t>(file_data.data() + offset, innodb::FIL_PAGE_TYPE_ABS);
        uint16_t n_dir_slots = innodb::read_le<uint16_t>(file_data.data() + offset, 38);
        uint32_t heap_top = innodb::read_le<uint32_t>(file_data.data() + offset, 40);

        // Count page types
        switch (page_type) {
            case innodb::FIL_PAGE_INDEX:  // 17855
                index_pages++;
                break;
            case innodb::FIL_PAGE_SDI:  // 17854
                sdi_pages++;
                break;
            case innodb::FIL_PAGE_UNDO_LOG:  // 17842
                undo_pages++;
                break;
        }

        // Skip non-index pages for B+Tree parsing
        if (page_type != innodb::FIL_PAGE_INDEX) {
            continue;
        }

        // Use table schema with minimal columns for parsing
        catalog::TableSchema minimal_schema;
        minimal_schema.table_name = "recovered_table";
        // Create ColumnInfo directly and add to map
        {
            catalog::ColumnInfo ci;
            ci.name = "col0";
            ci.mysql_type_id = 0x0C;  // VARCHAR
            ci.is_nullable = true;
            ci.column_length = 255;
            ci.charset_id = 255;
            ci.collation_id = 255;
            minimal_schema.columns[0] = ci;
        }
        {
            catalog::ColumnInfo ci;
            ci.name = "col1";
            ci.mysql_type_id = 0x0C;  // VARCHAR
            ci.is_nullable = true;
            ci.column_length = 255;
            ci.charset_id = 255;
            ci.collation_id = 255;
            minimal_schema.columns[1] = ci;
        }
        {
            catalog::ColumnInfo ci;
            ci.name = "col2";
            ci.mysql_type_id = 0x0C;  // VARCHAR
            ci.is_nullable = true;
            ci.column_length = 255;
            ci.charset_id = 255;
            ci.collation_id = 255;
            minimal_schema.columns[2] = ci;
        }
        {
            catalog::ColumnInfo ci;
            ci.name = "col3";
            ci.mysql_type_id = 0x13;  // DATETIME
            ci.is_nullable = true;
            ci.column_length = 19;
            ci.charset_id = 255;
            ci.collation_id = 255;
            minimal_schema.columns[3] = ci;
        }

        // Try B+Tree parsing
        std::vector<uint8_t> page_data(page_size_);
        std::memcpy(page_data.data(), file_data.data() + offset, page_size_);

        try {
            innodb::BPlusTreeParser parser(page_data);
            if (parser.parse()) {
                auto page_records = parser.parse_records(minimal_schema);

                for (const auto& rec : page_records) {
                    // Create fingerprint for deduplication
                    std::string fingerprint;
                    for (const auto& [key, value] : rec) {
                        fingerprint += key + "=" + value + "|";
                    }

                    if (seen_records.find(fingerprint) == seen_records.end()) {
                        seen_records.insert(fingerprint);
                        recovered_records.push_back(rec);
                    }
                }
            }
        } catch (...) {
            // B+Tree parsing failed for this page, continue
        }
    }

    std::cout << "[INFO] Full partition scan completed:" << std::endl;
    std::cout << "  - Index pages: " << index_pages << std::endl;
    std::cout << "  - SDI pages: " << sdi_pages << std::endl;
    std::cout << "  - Undo pages: " << undo_pages << std::endl;
    std::cout << "  - Records recovered: " << recovered_records.size() << std::endl;

    return recovered_records;
}

bool RmrfRecovery::full_partition_recovery_with_schema(
    const std::string& file_path,
    const catalog::TableSchema& schema,
    std::vector<std::map<std::string, std::string>>& recovered_records) {

    std::cout << "[INFO] RmrfRecovery::full_partition_recovery_with_schema" << std::endl;
    std::cout << "[INFO] File: " << file_path << std::endl;
    std::cout << "[INFO] Schema: " << schema.table_name << std::endl;
    std::cout << "  - Columns: " << schema.columns.size() << std::endl;
    for (const auto& [col_idx, col] : schema.columns) {
        std::cout << "    - " << col_idx << ": " << col.name << " (type=" << col.mysql_type_id << ")" << std::endl;
    }

    recovered_records.clear();
    std::set<std::string> seen_records;

    // Open file
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[ERROR] RmrfRecovery: Failed to open file: " << file_path << std::endl;
        return false;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
    std::cout << "[INFO] Total pages: " << total_pages << std::endl;

    std::vector<uint8_t> file_data(file_size);
    file.read(reinterpret_cast<char*>(file_data.data()), file_size);
    file.close();

    uint32_t index_pages = 0;
    uint32_t records_found = 0;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        uint32_t offset = page_no * page_size_;

        if (offset + 46 > file_size) {
            break;
        }

        uint16_t page_type = innodb::read_be<uint16_t>(file_data.data() + offset, innodb::FIL_PAGE_TYPE_ABS);

        if (page_type != innodb::FIL_PAGE_INDEX) {
            continue;
        }

        index_pages++;

        std::vector<uint8_t> page_data(page_size_);
        std::memcpy(page_data.data(), file_data.data() + offset, page_size_);

        try {
            innodb::BPlusTreeParser parser(page_data);
            if (parser.parse()) {
                auto page_records = parser.parse_records(schema);

                for (const auto& rec : page_records) {
                    // Create fingerprint
                    std::string fingerprint;
                    for (const auto& [key, value] : rec) {
                        fingerprint += key + "=" + value + "|";
                    }

                    if (seen_records.find(fingerprint) == seen_records.end()) {
                        seen_records.insert(fingerprint);
                        recovered_records.push_back(rec);
                        records_found++;
                    }
                }
            }
        } catch (...) {
            // B+Tree parsing failed, continue
        }

        // Progress output
        if (page_no % 1000 == 0) {
            std::cout << "[INFO] Scanned " << page_no << "/" << total_pages
                      << " pages, found " << records_found << " records" << std::endl;
        }
    }

    std::cout << "[INFO] Recovery completed:" << std::endl;
    std::cout << "  - Index pages scanned: " << index_pages << std::endl;
    std::cout << "  - Records recovered: " << records_found << std::endl;

    return !recovered_records.empty();
}


// ==================== 新增：TRUNCATE 恢复实现 ====================

bool RmrfRecovery::recover_truncated_table_from_disk(
    const std::string& disk_path,
    const catalog::TableSchema& schema,
    std::vector<std::map<std::string, std::string>>& recovered_records,
    uint32_t page_size,
    uint32_t min_pages) {
    std::cout << "[INFO] RmrfRecovery::recover_truncated_table_from_disk" << std::endl;
    std::cout << "[INFO] Disk path: " << disk_path << std::endl;
    std::cout << "[INFO] Table: " << schema.table_name << std::endl;
    std::cout << "[INFO] Page size: " << page_size << std::endl;

    recovered_records.clear();
    
    // 扫描磁盘寻找 IBD 块
    disk::EnhancedScanner scanner;
    scanner.set_page_size(page_size);
    
    if (!scanner.scan(disk_path)) {
        std::cerr << "[ERROR] Failed to open disk: " << disk_path << std::endl;
        return false;
    }
    
    // 使用表结构作为模板扫描
    std::vector<std::pair<std::string, uint8_t>> columns;
    for (const auto& [col_idx, col] : schema.columns) {
        columns.emplace_back(col.name, col.mysql_type_id);
    }
    
    auto blocks = scanner.scan_with_table_template(disk_path, columns, min_pages);
    
    if (blocks.empty()) {
        std::cout << "[WARN] No IBD blocks found on disk" << std::endl;
        return false;
    }
    
    std::cout << "[INFO] Found " << blocks.size() << " IBD blocks" << std::endl;
    
    // 聚合为完整文件
    auto files = scanner.aggregate_blocks(blocks, min_pages);
    
    if (files.empty()) {
        std::cout << "[WARN] No valid IBD files found" << std::endl;
        return false;
    }
    
    // 从找到的文件中恢复记录
    ScannedDevice dev_info;
    dev_info.device_path = disk_path;
    dev_info.device_size = 0;  // 未知
    dev_info.total_pages = static_cast<uint32_t>(blocks.size());
    dev_info.valid_pages = static_cast<uint32_t>(blocks.size());
    scanned_devices_.push_back(dev_info);
    
    for (const auto& file : files) {
        std::cout << "[INFO] Processing file: " << file.num_pages << " pages, " 
                  << file.index_pages << " index pages" << std::endl;
        
        // 解析每个页
        for (const auto& block : blocks) {
            if (block.page_type != innodb::FIL_PAGE_INDEX && block.page_type != innodb::FIL_PAGE_TYPE_BLOB && block.page_type != innodb::FIL_PAGE_SDI) {
                continue;
            }
            
            // 尝试 B+Tree 解析
            innodb::BPlusTreeParser parser(block.data);
            if (parser.parse()) {
                auto page_records = parser.parse_records(schema);
                recovered_records.insert(recovered_records.end(),
                                        page_records.begin(), page_records.end());
            } else {
                // 回退到启发式恢复
                auto heuristic_records = extract_records_heuristic(block.data, schema);
                recovered_records.insert(recovered_records.end(),
                                        heuristic_records.begin(), heuristic_records.end());
            }
        }
    }
    
    // 去重
    std::set<std::string> seen;
    std::vector<std::map<std::string, std::string>> unique_records;
    for (const auto& rec : recovered_records) {
        std::string fingerprint;
        for (const auto& [key, value] : rec) {
            fingerprint += key + "=" + value + "|";
        }
        if (seen.find(fingerprint) == seen.end()) {
            seen.insert(fingerprint);
            unique_records.push_back(rec);
        }
    }
    recovered_records = std::move(unique_records);
    
    std::cout << "[INFO] Recovered " << recovered_records.size() << " records" << std::endl;
    
    return !recovered_records.empty();
}

// ==================== 新增：rm -f 删除文件恢复实现 ====================

bool RmrfRecovery::recover_deleted_ibd_files(
    const std::string& disk_path,
    const std::string& output_dir,
    std::vector<std::string>& found_files,
    uint32_t min_pages) {
    std::cout << "[INFO] RmrfRecovery::recover_deleted_ibd_files" << std::endl;
    std::cout << "[INFO] Disk path: " << disk_path << std::endl;
    std::cout << "[INFO] Output dir: " << output_dir << std::endl;
    
    found_files.clear();
    
    // 确保输出目录存在
    std::filesystem::create_directories(output_dir);
    
    // 使用 enhanced scanner 扫描
    disk::EnhancedScanner scanner;
    
    if (!scanner.scan(disk_path)) {
        std::cerr << "[ERROR] Failed to open disk: " << disk_path << std::endl;
        return false;
    }
    
    // PhotoRec 风格的文件雕刻
    auto files = scanner.carve_ibd_files(min_pages);
    
    if (files.empty()) {
        std::cout << "[WARN] No IBD files found on disk" << std::endl;
        return false;
    }
    
    // 保存找到的文件
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        std::string filename = "recovered_table_" + std::to_string(i) + ".ibd";
        std::string filepath = output_dir + "/" + filename;
        
        std::ofstream out(filepath, std::ios::binary);
        if (!out) {
            std::cerr << "[WARN] Failed to create file: " << filepath << std::endl;
            continue;
        }
        
        out.write(reinterpret_cast<const char*>(file.data.data()), file.data.size());
        out.close();
        
        found_files.push_back(filepath);
        std::cout << "[INFO] Recovered: " << filename << " (" 
                  << file.num_pages << " pages, " << file.file_size << " bytes)" << std::endl;
    }
    
    std::cout << "[INFO] Found " << found_files.size() << " IBD files" << std::endl;
    
    return !found_files.empty();
}

// ==================== 新增：支持多模式的恢复 ====================

bool RmrfRecovery::reconstruct_from_pages_with_mode(
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages,
    const catalog::TableSchema& schema,
    int mode,
    std::vector<std::map<std::string, std::string>>& recovered_records) {
    recovered_records.clear();
    std::set<std::string> seen;
    
    if (pages.empty()) {
        std::cerr << "[WARN] No pages to reconstruct" << std::endl;
        return false;
    }
    
    std::cout << "[INFO] RmrfRecovery::reconstruct_from_pages_with_mode" << std::endl;
    std::cout << "[INFO] Mode: " << mode << " (0=auto, 1=btree, 2=heuristic)" << std::endl;
    
    // 按页号排序
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> sorted_pages = pages;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    
    int btree_parsed = 0;
    int heuristic_parsed = 0;
    int skipped = 0;
    
    for (const auto& [page_no, page_data] : sorted_pages) {
        // 检查页类型
        uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
        if (!innodb::is_valid_index_page_type(page_type)) {
            skipped++;
            continue;
        }
        
        bool success = false;
        
        if (mode == 2) {
            // 仅启发式
            auto records = extract_records_heuristic(page_data, schema);
            recovered_records.insert(recovered_records.end(),
                                    records.begin(), records.end());
            heuristic_parsed++;
            success = !records.empty();
        } else {
            // 模式 0（自动）或模式 1（仅 B+Tree）
            innodb::BPlusTreeParser parser(page_data);
            if (parser.parse()) {
                auto records = parser.parse_records(schema);
                
                // 去重
                for (const auto& rec : records) {
                    std::string fingerprint;
                    for (const auto& [key, value] : rec) {
                        fingerprint += key + "=" + value + "|";
                    }
                    if (seen.find(fingerprint) == seen.end()) {
                        seen.insert(fingerprint);
                        recovered_records.push_back(rec);
                    }
                }
                
                btree_parsed++;
                success = !records.empty();
                
                // 模式 1 下，如果 B+Tree 解析成功，不使用启发式
                if (mode == 1) {
                    continue;
                }
            }
            
            // 模式 0 下，B+Tree 失败时回退到启发式
            if (!success && mode == 0) {
                auto records = extract_records_heuristic(page_data, schema);
                recovered_records.insert(recovered_records.end(),
                                        records.begin(), records.end());
                heuristic_parsed++;
            }
        }
    }
    
    std::cout << "[INFO] Recovery completed:" << std::endl;
    std::cout << "  - B+Tree parsed: " << btree_parsed << " pages" << std::endl;
    std::cout << "  - Heuristic parsed: " << heuristic_parsed << " pages" << std::endl;
    std::cout << "  - Skipped: " << skipped << " pages" << std::endl;
    std::cout << "  - Records recovered: " << recovered_records.size() << std::endl;
    
    return !recovered_records.empty();
}

// ==================== 新增：PhotoRec 风格双层签名扫描 ====================

/**
 * @brief 使用 PhotoRec 风格的双层签名索引扫描文件
 *
 * Level 1: 按 offset 索引签名
 * Level 2: 在每个 offset 处按首字节值分桶（256个桶）
 *
 * @param file_path 文件路径
 * @param config 扫描配置
 * @param callback 每找到一个页时调用的回调
 * @return 找到的页数
 */
uint32_t RmrfRecovery::scan_with_two_level_index(
    const std::string& file_path,
    const TwoLevelScanConfig& config,
    std::function<void(const TwoLevelScanResult&)> callback) {

    std::cout << "[INFO] RmrfRecovery::scan_with_two_level_index: " << file_path << std::endl;

    // 构建签名索引
    engine::TwoLevelSignatureIndex sig_index;
    engine::InnoDBSignatureIndex::build_complete_index_into(sig_index);

    // 打开文件
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[ERROR] Failed to open file: " << file_path << std::endl;
        return 0;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 计算扫描范围
    uint64_t start_offset = config.start_offset;
    uint64_t end_offset = config.end_offset > 0 ? config.end_offset : file_size;
    uint32_t page_size = config.page_size > 0 ? config.page_size : page_size_;

    std::cout << "[INFO] Scanning range: " << start_offset << " - " << end_offset
              << " (page_size=" << page_size << ")" << std::endl;

    // 滑动窗口扫描
    constexpr size_t SLIDING_STEP = 512;  // PhotoRec 风格：512 字节步长
    size_t window_size = 16 * 1024 * 1024;  // 16MB 窗口
    std::vector<uint8_t> window(window_size + page_size);

    uint32_t pages_found = 0;
    uint64_t offset = start_offset;

    while (offset < end_offset) {
        size_t to_read = std::min(window_size, static_cast<size_t>(end_offset - offset));
        file.seekg(offset, std::ios::beg);
        file.read(reinterpret_cast<char*>(window.data()), to_read);
        size_t bytes_read = file.gcount();

        if (bytes_read == 0) break;

        // 在窗口内滑动扫描
        for (size_t pos = 0; pos + page_size <= bytes_read; pos += SLIDING_STEP) {
            const uint8_t* page = window.data() + pos;

            // 检查签名
            uint16_t page_type = innodb::read_be<uint16_t>(page, 24);

            // 过滤页类型
            if (!config.allowed_page_types.empty()) {
                bool allowed = false;
                for (uint16_t t : config.allowed_page_types) {
                    if (page_type == t) { allowed = true; break; }
                }
                if (!allowed) continue;
            }

            // 验证页
            if (config.validate_checksum) {
                bool valid = validate_page_checksum(page, page_size, config.strict_checksum);
                if (!valid && config.strict_checksum) continue;
            }

            // 构建结果
            TwoLevelScanResult result;
            result.file_offset = offset + pos;
            result.page_number = innodb::read_be<uint32_t>(page, 4);
            result.space_id = innodb::read_be<uint32_t>(page, 34);
            result.page_type = page_type;
            result.lsn = innodb::read_be<uint64_t>(page, 16);
            result.page_size = page_size;
            result.data.assign(page, page + page_size);

            callback(result);
            pages_found++;
        }

        offset += bytes_read;

        // 进度报告
        if (pages_found % 1000 == 0) {
            std::cout << "[INFO] Scanned " << (offset - start_offset) << "/" << (end_offset - start_offset)
                      << " bytes, found " << pages_found << " pages" << std::endl;
        }
    }

    std::cout << "[INFO] Two-level index scan completed: found " << pages_found << " pages" << std::endl;
    return pages_found;
}

/**
 * @brief FORCE 模式扫描文件
 *
 * FORCE 模式遍历文件的每一页，不依赖页链：
 * 1. 检查 offset 24 的签名
 * 2. 验证 PAGE_INDEX_ID
 * 3. 验证叶子页标识 (data[64:66] == 0)
 * 4. 提取有效数据
 *
 * @param file_path 文件路径
 * @param config FORCE 扫描配置
 * @param callback 每找到一个页时调用的回调
 * @return 找到的页数
 */
uint32_t RmrfRecovery::scan_with_force_mode(
    const std::string& file_path,
    const ForceScanConfig& config,
    std::function<void(const ForceScanResult&)> callback) {

    std::cout << "[INFO] RmrfRecovery::scan_with_force_mode: " << file_path << std::endl;

    // 打开文件
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[ERROR] Failed to open file: " << file_path << std::endl;
        return 0;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    uint32_t page_size = config.page_size > 0 ? config.page_size : page_size_;
    uint64_t num_pages = file_size / page_size;

    std::cout << "[INFO] FORCE scan: " << num_pages << " pages to scan" << std::endl;

    std::vector<uint8_t> page_data(page_size);
    uint32_t pages_found = 0;

    for (uint32_t page_no = 0; page_no < num_pages; ++page_no) {
        uint64_t offset = static_cast<uint64_t>(page_no) * page_size;

        file.seekg(offset, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size);

        const uint8_t* page = page_data.data();

        // Step 1: 检查 offset 24 的签名
        uint16_t page_type = innodb::read_be<uint16_t>(page, 24);
        if (page_type != 0x45BF && page_type != 0x26F1) {
            continue;  // 不是索引页
        }

        // Step 2: 验证 PAGE_INDEX_ID (offset 46)
        uint32_t page_index_id = innodb::read_be<uint32_t>(page, 46);
        if (config.expected_index_id != 0 &&
            page_index_id != config.expected_index_id) {
            continue;  // Index ID 不匹配
        }

        // Step 3: 验证叶子页标识 (offset 64-65 == 0)
        uint16_t leaf_indicator = innodb::read_be<uint16_t>(page, 64);
        bool is_leaf = (leaf_indicator == 0);

        // Step 4: 验证校验和
        bool checksum_valid = true;
        if (config.verify_checksum) {
            checksum_valid = validate_page_checksum(page, page_size, config.strict_mode);
            if (config.strict_mode && !checksum_valid) {
                continue;
            }
        }

        // 构建结果
        ForceScanResult result;
        result.file_offset = offset;
        result.page_number = innodb::read_be<uint32_t>(page, 4);
        result.space_id = innodb::read_be<uint32_t>(page, 34);
        result.page_type = page_type;
        result.lsn = innodb::read_be<uint64_t>(page, 16);
        result.index_id = page_index_id;
        result.is_leaf_page = is_leaf;
        result.checksum_valid = checksum_valid;
        result.data.assign(page_data.begin(), page_data.end());

        callback(result);
        pages_found++;

        // 进度报告
        if (page_no % 10000 == 0) {
            std::cout << "[INFO] FORCE scan: " << page_no << "/" << num_pages
                      << " pages scanned, " << pages_found << " found" << std::endl;
        }
    }

    std::cout << "[INFO] FORCE scan completed: found " << pages_found << " pages" << std::endl;
    return pages_found;
}

/**
 * @brief TRUNCATE 恢复：基于 LSN 过滤的页版本选择
 *
 * TRUNCATE 操作会保留数据页但更新元数据。本函数通过 LSN 过滤找到 TRUNCATE 前的数据版本。
 *
 * @param pages 所有找到的页版本
 * @param schema 表结构
 * @param max_lsn TRUNCATE 前的最大 LSN（可选）
 * @param callback 每恢复一条记录时调用的回调
 * @return 恢复的记录数
 */
uint32_t RmrfRecovery::recover_truncate_by_lsn(
    const std::vector<PageVersionInfo>& pages,
    const catalog::TableSchema& schema,
    uint64_t max_lsn,
    std::function<void(const std::map<std::string, std::string>&)> callback) {

    std::cout << "[INFO] RmrfRecovery::recover_truncate_by_lsn" << std::endl;
    std::cout << "[INFO] Total page versions: " << pages.size() << std::endl;
    std::cout << "[INFO] Max LSN filter: " << max_lsn << std::endl;

    // 按 (space_id, page_number) 分组
    std::map<std::pair<uint32_t, uint32_t>, std::vector<const PageVersionInfo*>> page_groups;
    for (const auto& page : pages) {
        auto key = std::make_pair(page.space_id, page.page_number);
        page_groups[key].push_back(&page);
    }

    std::cout << "[INFO] Unique pages: " << page_groups.size() << std::endl;

    uint32_t total_records = 0;

    // 对每个页组，选择 LSN 最老但数据完整的版本
    for (const auto& [key, versions] : page_groups) {
        const PageVersionInfo* best_version = nullptr;
        uint64_t best_lsn = 0;

        for (const auto* v : versions) {
            // 跳过 LSN 大于 max_lsn 的页（这些是 TRUNCATE 后的）
            if (v->lsn > max_lsn) continue;

            // 选择最早的有效版本
            if (!best_version || v->lsn < best_lsn) {
                // 验证页是否有效
                if (v->data.size() >= page_size_) {
                    best_version = v;
                    best_lsn = v->lsn;
                }
            }
        }

        if (!best_version) continue;

        // 解析页获取记录
        innodb::BPlusTreeParser parser(best_version->data);
        if (!parser.parse()) {
            // B+Tree 解析失败，尝试启发式
            auto records = extract_records_heuristic(best_version->data, schema);
            for (const auto& rec : records) {
                callback(rec);
                total_records++;
            }
            continue;
        }

        auto records = parser.parse_records(schema);
        for (const auto& rec : records) {
            callback(rec);
            total_records++;
        }
    }

    std::cout << "[INFO] TRUNCATE recovery by LSN: recovered " << total_records << " records" << std::endl;
    return total_records;
}

/**
 * @brief 验证页校验和
 */
bool RmrfRecovery::validate_page_checksum(
    const uint8_t* page,
    uint32_t page_size,
    bool strict) {

    if (page_size < 38) return !strict;

    // 读取存储的校验和 (offset 0-4)
    uint32_t stored_checksum = innodb::read_be<uint32_t>(page, 0);

    // 如果校验和为 0 或 0xFFFFFFFF，校验和可能被禁用
    if (stored_checksum == 0 || stored_checksum == 0xFFFFFFFF) {
        return true;  // 校验和禁用
    }

    // 尝试 CRC32C
    uint32_t computed_crc = compute_crc32c(page, page_size);
    if (computed_crc == stored_checksum) {
        return true;
    }

    // 尝试 InnoDB legacy 校验和
    uint32_t computed_innodb = compute_innodb_checksum(page, page_size);
    if (computed_innodb == stored_checksum) {
        return true;
    }

    return false;  // 校验和不匹配
}

/**
 * @brief 计算 CRC32C 校验和
 */
uint32_t RmrfRecovery::compute_crc32c(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

/**
 * @brief 计算 InnoDB legacy 校验和
 */
uint32_t RmrfRecovery::compute_innodb_checksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}

} // namespace recovery
} // namespace dbrecovery
