#ifndef DBRECOVERY_INNODB_ENCRYPTED_TABLE_RECOVERY_HPP
#define DBRECOVERY_INNODB_ENCRYPTED_TABLE_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief 加密表恢复统计
 */
struct EncryptedTableRecoveryStats {
    size_t total_pages = 0;
    size_t encrypted_pages = 0;
    size_t decrypted_pages = 0;
    size_t recovered_records = 0;
    size_t failed_pages = 0;
    double decrypt_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "EncryptedTableRecoveryStats{pages=" << total_pages
            << ", encrypted=" << encrypted_pages
            << ", decrypted=" << decrypted_pages
            << ", records=" << recovered_records
            << ", failed=" << failed_pages
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

/**
 * @brief 加密表恢复器
 */
class EncryptedTableRecovery {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit EncryptedTableRecovery(size_t page_size = 16384);

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置主密钥
     */
    void set_primary_key(const std::vector<uint8_t>& key);

    /**
     * @brief 添加表密钥
     */
    void add_table_key(uint32_t key_id, const std::vector<uint8_t>& key,
                       TDEAlgorithm algorithm);

    /**
     * @brief 从 Keyring 文件加载密钥
     */
    bool load_keyring(const std::string& keyring_path);

    /**
     * @brief 设置是否尝试无密钥恢复
     */
    void set_try_recovery_without_key(bool try_recovery);

    /**
     * @brief 执行加密表恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 获取统计信息
     */
    const EncryptedTableRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取解密器
     */
    TDEDecryptor& get_decryptor() { return decryptor_; }

    /**
     * @brief 获取加密页列表
     */
    const std::vector<uint32_t>& get_encrypted_pages() const { return encrypted_pages_; }

    /**
     * @brief 获取解密失败的页
     */
    const std::vector<uint32_t>& get_failed_pages() const { return failed_pages_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;
    TDEDecryptor decryptor_;
    bool try_recovery_without_key_ = true;

    std::vector<uint32_t> encrypted_pages_;
    std::vector<uint32_t> failed_pages_;
    std::vector<std::map<std::string, std::string>> records_;
    EncryptedTableRecoveryStats stats_;

    // 扫描页并检测加密
    std::vector<ScannedPage> scan_pages();

    // 检测页是否加密
    bool is_page_encrypted(const uint8_t* page_data, size_t page_size);

    // 解密页数据
    std::vector<uint8_t> decrypt_page_data(const uint8_t* encrypted_data,
                                           size_t size, uint32_t page_no,
                                           uint32_t space_id);

    // 解析页中的记录
    std::vector<std::map<std::string, std::string>> parse_page_records(
        const std::vector<ScannedPage>& pages);
};

// ==================== EncryptedTableRecovery 实现 ====================

inline EncryptedTableRecovery::EncryptedTableRecovery(size_t page_size)
    : page_size_(page_size), try_recovery_without_key_(true) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void EncryptedTableRecovery::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void EncryptedTableRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void EncryptedTableRecovery::set_primary_key(const std::vector<uint8_t>& key) {
    decryptor_.set_primary_key(key);
}

inline void EncryptedTableRecovery::add_table_key(uint32_t key_id, const std::vector<uint8_t>& key,
                                                  TDEAlgorithm algorithm) {
    decryptor_.add_table_key(key_id, key, algorithm);
}

inline bool EncryptedTableRecovery::load_keyring(const std::string& keyring_path) {
    KeyringFile keyring;
    if (!keyring.load(keyring_path)) {
        return false;
    }

    for (const auto& entry : keyring.entries) {
        // Hash key name to numeric key_id for use during per-table key derivation
        uint32_t key_id = static_cast<uint32_t>(
            std::hash<std::string>{}(entry.key_name));
        decryptor_.add_table_key(key_id, entry.key_data, entry.algorithm);
    }

    return !keyring.entries.empty();
}

inline void EncryptedTableRecovery::set_try_recovery_without_key(bool try_recovery) {
    try_recovery_without_key_ = try_recovery;
}

inline void EncryptedTableRecovery::clear() {
    encrypted_pages_.clear();
    failed_pages_.clear();
    records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline std::vector<ScannedPage> EncryptedTableRecovery::scan_pages() {
    auto start = std::chrono::high_resolution_clock::now();

    encrypted_pages_.clear();
    std::vector<ScannedPage> pages;

    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return pages;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    size_t total_pages = file_size / page_size_;
    stats_.total_pages = total_pages;

    if (total_pages == 0) {
        return pages;
    }

    std::vector<uint8_t> buffer(std::min(file_size, size_t(1024 * 1024)));

    size_t offset = 0;
    while (file.readsome(reinterpret_cast<char*>(buffer.data()), buffer.size())) {
        size_t read_size = file.gcount();

        for (size_t page_offset = 0; page_offset + page_size_ <= read_size; page_offset += page_size_) {
            const uint8_t* page_data = buffer.data() + page_offset;

            ScannedPage page{};
            page.offset = static_cast<uint32_t>(offset + page_offset);
            page.page_no = static_cast<uint32_t>(page_offset / page_size_);
            page.page_type = read_le<uint16_t>(page_data, 16);
            page.prev_page = read_le<uint32_t>(page_data, 0);
            page.next_page = read_le<uint32_t>(page_data, 4);
            page.lsn = read_le<uint64_t>(page_data, 8);
            page.space_id = read_le<uint32_t>(page_data, 24);
            page.is_valid = PageScanner::is_innodb_page(page_data, page_size_);

            // 检测是否加密
            if (is_page_encrypted(page_data, page_size_)) {
                page.is_encrypted = true;
                encrypted_pages_.push_back(page.page_no);
            }

            pages.push_back(page);
        }

        offset += read_size;
    }

    stats_.encrypted_pages = encrypted_pages_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.decrypt_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return pages;
}

inline bool EncryptedTableRecovery::is_page_encrypted(const uint8_t* page_data, size_t page_size) {
    if (page_size < 44) {
        return false;
    }

    // 检查 TDE 页头标志
    uint8_t algo = page_data[0];
    return algo >= 0x01 && algo <= 0x09;
}

inline std::vector<uint8_t> EncryptedTableRecovery::decrypt_page_data(const uint8_t* encrypted_data,
                                                                      size_t size, uint32_t page_no,
                                                                      uint32_t space_id) {
    auto start = std::chrono::high_resolution_clock::now();

    // 尝试解密
    auto decrypted = decryptor_.decrypt_page(encrypted_data, size, space_id, page_no);

    auto end = std::chrono::high_resolution_clock::now();
    stats_.decrypt_time_ms += std::chrono::duration<double, std::milli>(end - start).count();

    if (decrypted.empty()) {
        failed_pages_.push_back(page_no);
    } else {
        stats_.decrypted_pages++;
    }

    return decrypted;
}

inline std::vector<std::map<std::string, std::string>> EncryptedTableRecovery::parse_page_records(
    const std::vector<ScannedPage>& pages) {

    auto start = std::chrono::high_resolution_clock::now();

    records_.clear();

    for (const auto& page : pages) {
        if (!page.is_valid) {
            continue;
        }

        // 读取页数据
        std::ifstream file(file_path_, std::ios::binary);
        if (!file) continue;

        size_t offset = static_cast<size_t>(page.page_no) * page_size_;
        file.seekg(offset, std::ios::beg);

        std::vector<uint8_t> page_data(page_size_);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
            continue;
        }

        // 如果页加密，尝试解密
        std::vector<uint8_t> working_data = page_data;
        if (page.is_encrypted) {
            working_data = decrypt_page_data(page_data.data(), page_data.size(), page.page_no, page.space_id);
            if (working_data.empty()) {
                if (!try_recovery_without_key_) {
                    continue;
                }
                // 尝试无密钥恢复
                working_data = page_data;
            }
        }

        // 解析页
        BPlusTreeParser parser(working_data);
        if (!parser.parse()) {
            continue;
        }

        // 解析记录
        auto page_records = parser.parse_records(schema_);
        records_.insert(records_.end(), page_records.begin(), page_records.end());
    }

    stats_.recovered_records = records_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.decrypt_time_ms + stats_.recovery_time_ms;

    return records_;
}

inline std::vector<std::map<std::string, std::string>> EncryptedTableRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描页
    auto all_pages = scan_pages();

    // 解析记录
    records_ = parse_page_records(all_pages);

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

// ==================== 加密表恢复管理器 ====================
class EncryptedTableRecoveryManager {
public:
    /**
     * @brief 构造函数
     */
    EncryptedTableRecoveryManager();

    /**
     * @brief 添加加密表
     */
    void add_encrypted_table(const std::string& table_name,
                             const std::string& tablespace_id,
                             uint32_t key_id);

    /**
     * @brief 设置表密钥
     */
    void set_table_key(const std::string& tablespace_id, uint32_t key_id,
                       const std::vector<uint8_t>& key, TDEAlgorithm algorithm);

    /**
     * @brief 设置主密钥
     */
    void set_primary_key(const std::vector<uint8_t>& key);

    /**
     * @brief 获取表的密钥ID
     */
    std::optional<uint32_t> get_table_key_id(const std::string& tablespace_id) const;

    /**
     * @brief 获取所有加密表
     */
    const std::map<std::string, EncryptedTableInfo>& get_tables() const { return tables_; }

    /**
     * @brief 从 ibd 文件头解析加密信息
     */
    std::optional<EncryptedTableInfo> parse_encryption_info(const std::string& ibd_path);

private:
    struct EncryptedTableInfo {
        std::string table_name;
        std::string tablespace_id;
        uint32_t key_id;
        TDEAlgorithm algorithm;
        bool has_key;
    };

    std::map<std::string, EncryptedTableInfo> tables_;
    std::vector<uint8_t> primary_key_;
    std::unordered_map<std::string, std::vector<uint8_t>> table_keys_;
};

// ==================== EncryptedTableRecoveryManager 实现 ====================

inline EncryptedTableRecoveryManager::EncryptedTableRecoveryManager() {}

inline void EncryptedTableRecoveryManager::add_encrypted_table(const std::string& table_name,
                                                                const std::string& tablespace_id,
                                                                uint32_t key_id) {
    EncryptedTableInfo info;
    info.table_name = table_name;
    info.tablespace_id = tablespace_id;
    info.key_id = key_id;
    info.algorithm = TDEAlgorithm::AES_256_CBC;  // 默认算法
    info.has_key = false;

    tables_[tablespace_id] = info;
}

inline void EncryptedTableRecoveryManager::set_table_key(const std::string& tablespace_id,
                                                          uint32_t key_id,
                                                          const std::vector<uint8_t>& key,
                                                          TDEAlgorithm algorithm) {
    auto it = tables_.find(tablespace_id);
    if (it != tables_.end()) {
        it->second.key_id = key_id;
        it->second.algorithm = algorithm;
        it->second.has_key = true;
    }
    table_keys_[tablespace_id] = key;
}

inline void EncryptedTableRecoveryManager::set_primary_key(const std::vector<uint8_t>& key) {
    primary_key_ = key;
}

inline std::optional<uint32_t> EncryptedTableRecoveryManager::get_table_key_id(
    const std::string& tablespace_id) const {
    auto it = tables_.find(tablespace_id);
    if (it != tables_.end()) {
        return it->second.key_id;
    }
    return std::nullopt;
}

inline std::optional<EncryptedTableRecoveryManager::EncryptedTableInfo> 
EncryptedTableRecoveryManager::parse_encryption_info(const std::string& ibd_path) {
    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 16384) {
        return std::nullopt;
    }

    std::vector<uint8_t> page_data(16384);
    file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

    // 检查页 0 是否加密
    if (page_data.size() >= 44 && page_data[0] >= 0x01 && page_data[0] <= 0x09) {
        EncryptedTableInfo info;
        info.key_id = read_le<uint32_t>(page_data.data(), 1);
        info.algorithm = static_cast<TDEAlgorithm>(page_data[0]);
        info.has_key = false;
        return info;
    }

    return std::nullopt;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_ENCRYPTED_TABLE_RECOVERY_HPP
