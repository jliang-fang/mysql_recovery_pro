#pragma once
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/utils/file_mapper.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/innodb/blob_pointer.hpp"
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

namespace DBRecovery::Crypt {
    // 自定义输出流缓冲器，用于 CryptoPP 流式解密
    struct OStreamBuf : public CryptoPP::Bufferless<CryptoPP::OutputIterator> {
        std::ostream& os;
        OStreamBuf(std::ostream& s) : os(s) {}
        void Put(byte b) { os.put(b); }
    };

    class CryptScanner {
    private:
        static constexpr size_t MAX_PASSWORD_ATTEMPTS = 10000;

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
        static bool crack_password(const std::string& input_file, const std::string& password_file, std::string& cracked_password) {
            std::ifstream ifs(password_file);
            if (!ifs.is_open()) {
                return false;
            }

            std::vector<std::string> passwords;
            std::string line;
            while (std::getline(ifs, line)) {
                // 去除行尾空白字符
                size_t end = line.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) {
                    line.erase(end + 1);
                }
                passwords.push_back(line);
            }

            // 假设已知 InnoDB 文件头魔数为 0x00000f21（4字节）
            const uint8_t expected_header[] = {0x00, 0x00, 0x0f, 0x21};
            constexpr size_t header_len = sizeof(expected_header);

            for (const auto& pwd : passwords) {
                std::string decrypted_file = input_file + ".tmp";
                if (decrypt_file(input_file, decrypted_file, pwd)) {
                    // ✅ 使用恒定时间比较验证解密结果（校验已知明文头）
                    FileMapper mapper(decrypted_file);
                    if (mapper.size() >= header_len) {
                        const uint8_t* data = reinterpret_cast<const uint8_t*>(mapper.data());
                        if (constant_time_compare(data, expected_header, header_len)) {
                            std::remove(decrypted_file.c_str());
                            cracked_password = pwd;
                            return true;
                        }
                    }
                    std::remove(decrypted_file.c_str());
                } else {
                    std::remove(decrypted_file.c_str());
                }
            }

            return false;
        }

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
}
