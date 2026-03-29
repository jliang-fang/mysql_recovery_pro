#pragma once
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/utils/file_mapper.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/disk/disk_scanner.hpp"
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include <cryptopp/filters.h>
#include <cryptopp/hex.h>
#include <cryptopp/sha256.h>
#include <cryptopp/rsa.h>
#include <cryptopp/base64.h>
#include <cryptopp/cryptlib.h>
#include <optional>
#include <unordered_set>
#include <cctype>
#include <stdexcept>

namespace dbrecovery {
namespace crypt {

// 最大密码尝试次数
constexpr size_t MAX_PASSWORD_ATTEMPTS = 10000;

// CryptoPP byte 类型别名
using byte = CryptoPP::byte;

// 简化的输出流缓冲器（移除复杂的 CryptoPP 继承）
class OStreamBuf {
public:
    std::ostream& os;
    OStreamBuf(std::ostream& s) : os(s) {}
    void put(byte b) { os.put(b); }
};

class CryptScanner {
private:
    // ✅ 恒定时间比较函数，防止时序攻击
    static bool constant_time_compare(const uint8_t* a, const uint8_t* b, size_t len) {
        volatile uint8_t result = 0;
        for (size_t i = 0; i < len; ++i)
            result |= a[i] ^ b[i];
        return result == 0;
    }

public:
    // 检测文件是否被加密
    static bool is_encrypted_file(const std::string& file_path, size_t sample_size = 16384);

    // 尝试解密文件
    static bool decrypt_file(const std::string& input_file, const std::string& output_file, const std::string& password,
                             const std::string& algorithm = "AES-256", bool use_bcrypt = false);

    // 尝试破解密码（✅ 修复：使用恒定时间比较防止时序攻击）
    static bool crack_password(const std::string& input_file, const std::vector<std::string>& passwords, std::string& cracked_password);

    // 检测是否是InnoDB文件
    static bool is_innodb_file(const std::string& file_path);

    // 加密文件
    static bool encrypt_file(const std::string& input_file, const std::string& output_file, const std::string& password,
                             const std::string& algorithm = "AES-256", bool use_bcrypt = false);

    // 计算文件熵值
    static double calculate_file_entropy(const std::string& file_path, size_t sample_size = 16384);

    // 生成密码字典
    static bool generate_password_dictionary(const std::string& output_file, int num_passwords = 1000,
                                           bool include_common = true, bool use_bcrypt = false);

    // 获取支持的加密算法
    static std::vector<std::string> get_supported_algorithms();

    // 从加密文件中提取元数据
    static bool extract_metadata_from_encrypted_file(const std::string& input_file,
                                                    std::string& algorithm,
                                                    size_t& iv_size);
};

} // namespace crypt
} // namespace dbrecovery
