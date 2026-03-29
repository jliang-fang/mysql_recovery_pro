#include "dbrecovery/recovery/rmrf_recovery.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/btree.hpp"
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
            uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
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
        uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

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
    uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
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
        uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
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

    // 逐页读取
    std::vector<uint8_t> page_data(page_size_);
    uint32_t recovered_pages = 0;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        // 检查页类型
        uint16_t page_type = innodb::read_le<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);
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
        recovered_pages++;
    }

    std::cout << "[INFO] RmrfRecovery: Recovered " << recovered_records.size() 
              << " records from " << recovered_pages << " pages" << std::endl;

    return !recovered_records.empty();
}

} // namespace recovery
} // namespace dbrecovery
