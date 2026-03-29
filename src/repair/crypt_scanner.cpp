#include <cstdlib>
#include <ctime>
#include <string>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <random>
#include <stdexcept>

// ✅ 包含必要的头文件以支持安全字节序处理与平台兼容性
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/utils/file_mapper.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/crypt/crypt_scanner.hpp"

namespace dbrecovery {
namespace crypt {

// ✅ 安全临时文件生成（跨平台，避免竞态条件）
std::string generate_temp_filename() {
#ifdef _WIN32
    wchar_t temp_path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_path)) {
        throw std::runtime_error("Failed to get temporary directory path");
    }

    wchar_t temp_name[MAX_PATH];
    if (!GetTempFileNameW(temp_path, L"dbrecovery_", 0, temp_name)) {
        throw std::runtime_error("Failed to generate unique temporary filename");
    }

    // Convert wide string to UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0, temp_name, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        throw std::runtime_error("Failed to convert temporary filename to UTF-8");
    }

    std::string path(len - 1, '\0'); // Exclude null terminator
    WideCharToMultiByte(CP_UTF8, 0, temp_name, -1, &path[0], len, nullptr, nullptr);
    return path;
#else
    char path[] = "/tmp/dbrecovery_XXXXXX";
    int fd = mkstemp(path);
    if (fd == -1) {
        throw std::runtime_error("Failed to create secure temporary file");
    }
    close(fd);
    return path;
#endif
}

// ✅ 安全的密码文件校验：防止篡改或错误加载（占位符）
std::string sha256(const std::string& /*file_path*/) {
    // 实际实现应调用安全哈希库（如 OpenSSL EVP）
    // 此处为示意伪代码，实际需替换为真实实现
    throw std::runtime_error("SHA-256 implementation not provided in this context");
}

// 解密文件的占位符实现（需要 CryptoPP 实际实现）
bool decrypt_file(const std::string& /*input_file*/, const std::string& /*output_file*/, const std::string& /*password*/,
                  const std::string& /*algorithm*/, bool /*use_bcrypt*/) {
    // 实际实现应使用 CryptoPP 进行解密
    // 此处返回 false 表示未实现
    return false;
}

} // namespace crypt
} // namespace dbrecovery

// ✅ 修复后的 crack_password 函数（无竞态条件、安全清理）
bool dbrecovery::crypt::CryptScanner::crack_password(const std::string& input_file,
                                  const std::vector<std::string>& passwords,
                                  std::string& cracked_password) {
    if (passwords.size() > dbrecovery::crypt::MAX_PASSWORD_ATTEMPTS) {
        throw std::runtime_error("Password list too large");
    }

    for (const auto& pwd : passwords) {
        // ✅ 使用安全生成的临时文件名（忽略调用者提供的 output_dir）
        std::string temp_file = dbrecovery::crypt::generate_temp_filename();

        try {
            if (dbrecovery::crypt::decrypt_file(input_file, temp_file, pwd, "AES-256", false)) {
                if (dbrecovery::disk::DiskScanner::is_innodb_file(temp_file)) {
                    cracked_password = pwd;
                    // ✅ 确保删除临时文件
                    std::remove(temp_file.c_str());
                    return true;
                }
            }
        } catch (...) {
            // 即使异常也确保清理临时文件
            try {
                std::remove(temp_file.c_str());
            } catch (...) {
                // 忽略清理过程中的异常，避免掩盖原始异常
            }
            throw;
        }

        // ✅ 即使失败也确保清理
        try {
            std::remove(temp_file.c_str());
        } catch (...) {
            // 忽略清理过程中的异常
        }
    }

    return false;
}
