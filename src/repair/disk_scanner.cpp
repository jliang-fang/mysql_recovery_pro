#include "dbrecovery/disk/disk_scanner.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <boost/filesystem.hpp>
#include <optional>
#include <unordered_set>
#include <cctype>
#include <memory>
#include <sys/vfs.h>
#include <cstdio>
#include <cstring>
#include <cerrno>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/file.h>
#endif

namespace dbrecovery {
namespace disk {

// 辅助结构：临时文件 RAII 管理器（用于自动清理）
struct TempFile {
    std::string path;
    ~TempFile() {
        try {
            boost::filesystem::remove(path);
        } catch (...) {}
    }
};

bool DiskScanner::is_innodb_page(const uint8_t* page, size_t page_size) {
    // 支持多种页大小：4KB/8KB/16KB/32KB/64KB
    static constexpr std::array<size_t, 5> valid_page_sizes = {4096, 8192, 16384, 32768, 65536};
    if (std::find(valid_page_sizes.begin(), valid_page_sizes.end(), page_size) == valid_page_sizes.end()) {
        return false;
    }

    // 检查页类型（使用正确的偏移量 FIL_PAGE_TYPE_ABS = 24，大端序）
    uint16_t type = dbrecovery::innodb::read_be<uint16_t>(page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

    // 检查页类型是否有效
    if (type != dbrecovery::innodb::FIL_PAGE_INDEX &&
        type != dbrecovery::innodb::FIL_PAGE_SDI &&
        type != dbrecovery::innodb::FIL_PAGE_TYPE_ALLOCATED &&
        type != dbrecovery::innodb::FIL_PAGE_TYPE_BLOB) {
        return false;
    }

    // FIL_PAGE_TYPE_ALLOCATED 类型页未启用校验和，跳过校验
    if (type == dbrecovery::innodb::FIL_PAGE_TYPE_ALLOCATED) {
        return true;
    }

    // 校验 CRC32C
    uint32_t stored_crc = dbrecovery::innodb::read_le<uint32_t>(page, 24);

    if (stored_crc == 0 || stored_crc == 0xFFFFFFFF) {
        // 允许 NONE 校验和（未启用校验）
        return true;
    }

    // 计算 CRC32C (Castagnoli)
    std::vector<uint8_t> page_vec(page, page + page_size);
    // 清零校验和字段
    for (int i = 0; i < 4; ++i) {
        page_vec[24 + i] = 0;
    }
    uint32_t computed_crc = dbrecovery::innodb::PageReader::compute_crc32c(page_vec);

    return stored_crc == computed_crc;
}

bool DiskScanner::is_innodb_file(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return false;
    }

    // 检查文件大小
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    if (file_size < dbrecovery::innodb::UNIV_PAGE_SIZE) {
        return false;
    }

    // 读取第一页
    std::vector<uint8_t> page(dbrecovery::innodb::UNIV_PAGE_SIZE);
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(page.data()), dbrecovery::innodb::UNIV_PAGE_SIZE);

    return is_innodb_page(page.data(), dbrecovery::innodb::UNIV_PAGE_SIZE);
}

std::vector<std::pair<std::string, size_t>> DiskScanner::scan_disk(const std::string& disk_path, size_t block_size) {
    std::vector<std::pair<std::string, size_t>> results;

    // 1. 检查路径是否存在
    struct stat stat_buf;
    if (stat(disk_path.c_str(), &stat_buf) != 0) {
        std::cerr << "无法访问路径: " << disk_path << std::endl;
        return results;
    }

    // 2. 如果是文件，直接检查
    if (S_ISREG(stat_buf.st_mode)) {
        if (is_innodb_file(disk_path)) {
            results.push_back({disk_path, stat_buf.st_size});
        }
        return results;
    }

    // 3. 如果是目录，递归扫描
    if (S_ISDIR(stat_buf.st_mode)) {
        DIR* dir = opendir(disk_path.c_str());
        if (!dir) {
            std::cerr << "无法打开目录: " << disk_path << std::endl;
            return results;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string full_path = disk_path + "/" + entry->d_name;

            // 递归扫描子目录
            auto sub_results = scan_disk(full_path, block_size);
            results.insert(results.end(), sub_results.begin(), sub_results.end());
        }

        closedir(dir);
    }

    return results;
}

bool DiskScanner::extract_innodb_file(const std::string& disk_path, size_t offset, size_t size, const std::string& output_file) {
    std::ifstream disk(disk_path, std::ios::binary);
    if (!disk) {
        std::cerr << "无法打开磁盘文件: " << disk_path << std::endl;
        return false;
    }

    // 1. 创建输出文件
    std::ofstream output(output_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "无法创建输出文件: " << output_file << std::endl;
        return false;
    }

    // 2. 定位到偏移位置
    disk.seekg(offset, std::ios::beg);

    // 3. 读取并写入数据
    const size_t buffer_size = 1024 * 1024; // 1MB缓冲区
    std::vector<char> buffer(buffer_size);

    size_t remaining = size;
    while (remaining > 0) {
        size_t to_read = std::min(buffer_size, remaining);
        disk.read(buffer.data(), to_read);
        size_t actually_read = disk.gcount();

        if (actually_read == 0) {
            break;
        }

        output.write(buffer.data(), actually_read);
        remaining -= actually_read;
    }

    return true;
}

std::vector<std::pair<size_t, size_t>> DiskScanner::scan_for_innodb_blocks(const std::string& disk_path, size_t block_size) {
    std::vector<std::pair<size_t, size_t>> results;

    std::ifstream disk(disk_path, std::ios::binary);
    if (!disk) {
        return results;
    }

    // 获取文件大小
    disk.seekg(0, std::ios::end);
    size_t file_size = disk.tellg();
    disk.seekg(0, std::ios::beg);

    // 扫描文件
    std::vector<uint8_t> buffer(block_size);

    for (size_t offset = 0; offset + block_size <= file_size; offset += block_size) {
        disk.seekg(offset, std::ios::beg);
        disk.read(reinterpret_cast<char*>(buffer.data()), block_size);

        if (is_innodb_page(buffer.data(), block_size)) {
            results.push_back({offset, block_size});
        }
    }

    return results;
}

std::vector<std::string> DiskScanner::recover_deleted_files(const std::string& disk_path, const std::string& output_dir, size_t block_size) {
    std::vector<std::string> recovered_files;

    // 1. 检查输出目录是否存在
    if (mkdir(output_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "无法创建输出目录: " << output_dir << std::endl;
        return recovered_files;
    }

    // 2. 扫描磁盘中的InnoDB块
    auto blocks = scan_for_innodb_blocks(disk_path, block_size);

    // 3. 为每个找到的块创建恢复文件（使用临时文件机制确保异常安全）
    size_t file_index = 0;
    for (const auto& [offset, size] : blocks) {
        std::string temp_output_file = output_dir + "/recovered_" + std::to_string(file_index) + ".tmp";
        TempFile temp_file{temp_output_file};

        if (extract_innodb_file(disk_path, offset, size, temp_output_file)) {
            // 成功写入后重命名为最终文件
            std::string final_output = output_dir + "/recovered_" + std::to_string(file_index) + ".ibd";
            try {
                boost::filesystem::rename(temp_output_file, final_output);
                recovered_files.push_back(final_output);
                file_index++;
            } catch (...) {
                // 重命名失败，继续下一个
            }
        }
    }

    return recovered_files;
}

} // namespace disk
} // namespace dbrecovery
