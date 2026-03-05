#pragma once

#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/innodb/blob_pointer.hpp"
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <boost/filesystem.hpp>
#include <optional>
#include <unordered_set>
#include <cctype>
#include <random>
#include <filesystem>
#include <sstream>
#include <stdexcept>

#ifdef __linux__
    #include <sys/vfs.h>
#endif

namespace DBRecovery::Disk {
    class DiskScanner {
    public:
        // 扫描磁盘并查找InnoDB文件
        static std::vector<std::pair<std::string, size_t>> scan_disk(const std::string& disk_path, size_t block_size = 4096);

        // 从磁盘中提取InnoDB文件
        static bool extract_innodb_file(const std::string& disk_path, size_t offset, size_t size, const std::string& output_file);

        // 检测是否是InnoDB页
        static bool is_innodb_page(const uint8_t* page, size_t page_size = DBRecovery::InnoDB::UNIV_PAGE_SIZE);

        // 检测文件是否是InnoDB文件
        static bool is_innodb_file(const std::string& file_path);

        // 从磁盘中扫描InnoDB块
        static std::vector<std::pair<size_t, size_t>> scan_for_innodb_blocks(const std::string& disk_path, size_t block_size = 4096);

        // 从磁盘中恢复删除的文件
        static std::vector<std::string> recover_deleted_files(const std::string& disk_path, const std::string& output_dir, size_t block_size = 4096);

        // 获取文件系统类型
        static std::string get_filesystem_type(const std::string& path) {
#ifdef __linux__
            struct statfs fs_info;
            if (statfs(path.c_str(), &fs_info) != 0) {
                return "unknown";
            }
            switch (fs_info.f_type) {
                case 0x5346544E: return "ntfs";       // NTFS
                case 0xEF53:     return "ext2/ext3/ext4";
                case 0x482B86C1: return "hfs/hfs+";
                case 0x7275:     return "rootfs";
                case 0x61617272: return "btrfs";
                case 0x31534646: return "f2fs";
                case 0x58465342: return "xfs";
                default:         return "unknown";
            }
#elif defined(__APPLE__) || defined(__MACH__)
            struct statfs fs_info;
            if (statfs(path.c_str(), &fs_info) != 0) {
                return "unknown";
            }
            std::string fstype = fs_info.f_fstypename;
            return fstype.empty() ? "unknown" : fstype;
#elif defined(_WIN32)
            // Windows平台：使用GetVolumeInformation获取文件系统类型
            wchar_t volume_name[MAX_PATH] = {0};
            wchar_t fs_name[MAX_PATH] = {0};
            DWORD serial_number = 0, max_component_len = 0, flags = 0;
            
            std::wstring wpath = boost::filesystem::path(path).wstring();
            if (GetVolumeInformationW(wpath.c_str(), volume_name, MAX_PATH,
                                      &serial_number, &max_component_len, &flags,
                                      fs_name, MAX_PATH)) {
                return std::string(fs_name);
            }
            return "unknown";
#else
            // Fallback for other Unix-like platforms (e.g., BSD)
            struct statfs fs_info;
            if (statfs(path.c_str(), &fs_info) != 0) {
                return "unknown";
            }
            std::string fstype = fs_info.f_fstypename;
            return fstype.empty() ? "unknown" : fstype;
#endif
        }

        // 从文件系统中获取已删除文件的元数据（平台特定实现）
        static std::vector<struct stat> get_deleted_file_metadata(const std::string& disk_path, const std::string& output_dir);

    private:
        // 平台特定：创建临时文件路径（用于恢复过程中的中间处理）
        static std::string create_temp_file_path() {
            auto temp_dir = std::filesystem::temp_directory_path();
#ifdef _WIN32
            // 使用加密随机数生成器（CryptGenRandom 或 CryptAcquireContext）
            HCRYPTPROV hProvider;
            if (!CryptAcquireContextA(&hProvider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
                throw std::runtime_error("Failed to acquire cryptographic context");
            }
            
            DWORD random_data;
            if (!CryptGenRandom(hProvider, sizeof(random_data), reinterpret_cast<BYTE*>(&random_data))) {
                CryptReleaseContext(hProvider, 0);
                throw std::runtime_error("Failed to generate cryptographically secure random number");
            }
            CryptReleaseContext(hProvider, 0);

            std::ostringstream oss;
            oss << "temp_" << std::hex << random_data << ".ibd";
            return (temp_dir / oss.str()).string();
#else
            // 使用 mkstemp 安全创建临时文件
            char path[] = "/tmp/dbrecovery_XXXXXX";
            int fd = mkstemp(path);
            if (fd == -1) {
                throw std::runtime_error("Failed to create secure temporary file");
            }
            close(fd);
            return path;
#endif
        }

#ifdef _WIN32
        // Windows平台：使用NTFS API恢复删除文件元数据（占位实现）
        static std::vector<struct stat> get_deleted_file_metadata_ntfs(const std::string& disk_path, const std::string& output_dir);
#endif

#ifdef __linux__
        // Linux平台：通过ext4/ext3的未分配inode信息恢复已删除文件元数据（占位实现）
        static std::vector<struct stat> get_deleted_file_metadata_ext(const std::string& disk_path, const std::string& output_dir);
#endif

#if defined(__APPLE__) || defined(__MACH__)
        // macOS平台：使用HFS+的B-tree结构恢复已删除文件元数据（占位实现）
        static std::vector<struct stat> get_deleted_file_metadata_hfs(const std::string& disk_path, const std::string& output_dir);
#endif
    };
}

// ==================== 实现部分 ====================
#ifdef _WIN32
std::vector<struct stat> DBRecovery::Disk::DiskScanner::get_deleted_file_metadata_ntfs(const std::string& disk_path, const std::string& output_dir) {
    // 实际实现需调用Windows NTFS底层API，如NtQueryDirectoryFile等
    // 此处为占位符，实际应结合Win32 API或使用第三方库（如libntfs-3g）
    throw std::runtime_error("NTFS deleted file recovery requires admin privileges and low-level disk access");
}
#endif

#ifdef __linux__
std::vector<struct stat> DBRecovery::Disk::DiskScanner::get_deleted_file_metadata_ext(const std::string& disk_path, const std::string& output_dir) {
    // 实际实现需解析磁盘镜像中的超级块、组描述符和inode表
    throw std::runtime_error("ext4 deleted file recovery requires low-level disk access and root privileges");
}
#endif

#if defined(__APPLE__) || defined(__MACH__)
std::vector<struct stat> DBRecovery::Disk::DiskScanner::get_deleted_file_metadata_hfs(const std::string& disk_path, const std::string& output_dir) {
    throw std::runtime_error("HFS+ deleted file recovery requires low-level disk access and root privileges");
}
#endif

// 默认实现：根据平台分发调用
std::vector<struct stat> DBRecovery::Disk::DiskScanner::get_deleted_file_metadata(const std::string& disk_path, const std::string& output_dir) {
#ifdef _WIN32
    return get_deleted_file_metadata_ntfs(disk_path, output_dir);
#elif defined(__linux__)
    return get_deleted_file_metadata_ext(disk_path, output_dir);
#elif defined(__APPLE__) || defined(__MACH__)
    return get_deleted_file_metadata_hfs(disk_path, output_dir);
#else
    // 其他平台暂不支持
    throw std::runtime_error("Deleted file recovery is not supported on this platform");
#endif
}
