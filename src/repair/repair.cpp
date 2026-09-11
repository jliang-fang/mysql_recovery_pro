#include "dbrecovery/repair/repair.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/utils/innodb_checksum.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <vector>
#include <fstream>

namespace dbrecovery {
namespace repair {

double Repairer::calculate_entropy(const uint8_t* data, size_t size) {
    if (size == 0) return 0.0;

    // 使用16KB样本
    size_t sample_size = std::min(size, static_cast<size_t>(16384));

    // 初始化频率计数器
    int frequency[256] = {0};

    // 计算频率
    for (size_t i = 0; i < sample_size; ++i) {
        frequency[data[i]]++;
    }

    // 计算熵值
    double entropy = 0.0;
    for (int i = 0; i < 256; ++i) {
        if (frequency[i] > 0) {
            double probability = static_cast<double>(frequency[i]) / sample_size;
            entropy -= probability * std::log2(probability);
        }
    }

    return entropy;
}

bool Repairer::is_possibly_encrypted(const std::string& file_path, size_t sample_size) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return false;
    }

    // 读取样本数据
    std::vector<uint8_t> buffer(sample_size);
    file.read(reinterpret_cast<char*>(buffer.data()), sample_size);

    if (file.gcount() == 0) {
        return false;
    }

    // 计算熵值
    double entropy = calculate_entropy(buffer.data(), file.gcount());

    // 高熵值（接近8）可能表示加密或压缩数据
    return entropy > 7.5;
}

bool Repairer::is_page_valid(const uint8_t* page, size_t page_size) {
    if (page_size < dbrecovery::innodb::UNIV_PAGE_SIZE) {
        return false;
    }

    // 检查页类型 - 使用正确的偏移量（相对于页起始位置），大端序
    uint16_t type = dbrecovery::innodb::read_be<uint16_t>(page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

    // 检查页头（基本有效性检查）
    if (page[0] == 0 && page[1] == 0 && page[2] == 0 && page[3] == 0) {
        return false;
    }

    // 有效页类型
    return type == dbrecovery::innodb::FIL_PAGE_INDEX ||
           type == dbrecovery::innodb::FIL_PAGE_SDI ||
           type == dbrecovery::innodb::FIL_PAGE_TYPE_ALLOCATED ||
           type == dbrecovery::innodb::FIL_PAGE_TYPE_BLOB;
}

bool Repairer::is_page_corrupted(const uint8_t* page, size_t page_size) {
    if (!is_page_valid(page, page_size)) {
        return true;
    }

    // 检查校验和
    if (!dbrecovery::utils::InnoDBChecksum::validate_crc32(page, page_size)) {
        return true;
    }

    // 检查BLOB指针（如果有的话）
    if (page_size >= 16384) {
        // 检查可能的BLOB指针位置
        size_t blob_ptr_offset = page_size - 16384;
        if (blob_ptr_offset < 20) {
            return true;
        }

        try {
            auto blob_ptr = dbrecovery::innodb::parse_blob_pointer(page + blob_ptr_offset, 20);
            if (!blob_ptr.has_value() || blob_ptr->len == 0) {
                return true;
            }
        } catch (const std::exception&) {
            return true;
        }
    }

    return false;
}

uint32_t Repairer::calculate_innodb_checksum(const uint8_t* page, size_t page_size) {
    // 1. 从页头读取
    uint32_t c1 = 0;
    c1 = crc32(c1, page + 4, 26);          // Fil Header 部分
    c1 = crc32(c1, page + 38, page_size - 38 - 8); // 数据体
    return c1;
}

bool Repairer::fix_checksum(uint8_t* page, size_t page_size) {
    // 清零校验和字段
    for (int i = 0; i < 4; ++i) {
        page[24 + i] = 0;
    }
    
    // 使用 PageReader 计算 CRC32C 校验和
    std::vector<uint8_t> page_vec(page, page + page_size);
    uint32_t correct_checksum = dbrecovery::innodb::PageReader::compute_crc32c(page_vec);

    // 写入校验和字段（偏移 24-27）
    for (int i = 0; i < 4; ++i) {
        page[24 + i] = (correct_checksum >> (i * 8)) & 0xFF;
    }

    return dbrecovery::utils::InnoDBChecksum::validate_crc32(page, page_size);
}

bool Repairer::is_blob_pointer_safe(const dbrecovery::innodb::BlobPointer& ptr) {
    // 验证BLOB指针合理性
    if (ptr.space_id == 0 || ptr.page_no == 0 || ptr.offset >= 16384 || ptr.len == 0) {
        return false;
    }
    // 防止溢出和异常值
    if (ptr.len > 1024 * 1024 * 1024ULL) { // >1GB不合理
        return false;
    }
    return true;
}

std::optional<dbrecovery::innodb::BlobPointer> Repairer::get_blob_info_from_sdi(
    const uint8_t* page,
    size_t page_size,
    std::shared_ptr<dbrecovery::innodb::SDIManager> sdi_manager
) {
    if (!sdi_manager || !page || page_size < dbrecovery::innodb::UNIV_PAGE_SIZE) {
        return std::nullopt;
    }

    try {
        // 从 SDI 页面解析表结构
        // schema 单独不能推导真实 external storage pointer；BLOB 指针必须来自
        // 记录内证据，这里明确拒绝而不是返回猜测值。
        (void)page;
        (void)page_size;
    } catch (...) {
        // 忽略异常，返回空
    }

    return std::nullopt;
}

std::optional<dbrecovery::innodb::BlobPointer> Repairer::get_blob_info_from_neighbor_pages(
    const uint8_t* page,
    size_t page_size,
    size_t page_index,
    const std::vector<uint8_t>& file_data
) {
    // 从相邻页面查找BLOB信息
    if (file_data.size() < page_size || page_size == 0) {
        return std::nullopt;
    }

    size_t num_pages = file_data.size() / page_size;

    // 检查左侧邻接页
    for (int i = -1; i <= 1; ++i) {
        if (page_index + i < 0 || static_cast<size_t>(page_index + i) >= num_pages) {
            continue;
        }

        size_t offset = (static_cast<size_t>(page_index + i)) * page_size;
        const uint8_t* neighbor_page = file_data.data() + offset;

        // 检查是否是有效的BLOB页
        uint16_t type = dbrecovery::innodb::read_be<uint16_t>(neighbor_page, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
        if (type == dbrecovery::innodb::FIL_PAGE_TYPE_BLOB) {
            // 从BLOB页中提取BLOB指针（简化处理）
            if (page_size >= 20) {
                try {
                    auto blob_ptr = dbrecovery::innodb::parse_blob_pointer(neighbor_page + page_size - 16384, 20);
                    if (blob_ptr.has_value() && is_blob_pointer_safe(*blob_ptr)) {
                        return *blob_ptr;
                    }
                } catch (...) {
                    continue;
                }
            }
        }
    }

    return std::nullopt;
}

bool Repairer::fix_blob_pointer(uint8_t* page, size_t page_size) {
    if (page_size < dbrecovery::innodb::UNIV_PAGE_SIZE) {
        return false;
    }

    // 安全计算 BLOB 指针位置（避免负偏移）
    constexpr size_t MAX_BLOB_HEADER_SIZE = 20;
    constexpr size_t FIL_TRAILER_SIZE = 8;
    if (page_size <= MAX_BLOB_HEADER_SIZE + FIL_TRAILER_SIZE) {
        return true; // 无空间存储 BLOB 指针
    }

    size_t blob_ptr_offset = page_size - MAX_BLOB_HEADER_SIZE - FIL_TRAILER_SIZE;

    try {
        auto blob_ptr = dbrecovery::innodb::parse_blob_pointer(page + blob_ptr_offset, 20);

        // 校验原始指针有效性
        if (!blob_ptr.has_value() || !is_blob_pointer_safe(*blob_ptr)) {
            // 没有真实文件上下文时不能从虚构的邻居页猜测指针；保持原页不变。
            return false;
        }
    } catch (const std::exception&) {
        // 记录错误但不破坏原页
        return false;
    }

    return true;
}

bool Repairer::fix_sdi_record(std::vector<uint8_t>& data, size_t page_size) {
    if (data.size() < 20) {
        return false;
    }

    // Delegate to the bounded SDIManager which enforces 16 MiB / 20:1 limits
    // and validates the decompressed JSON.  The input is expected to be a
    // standard 20-byte LE SDI header + raw-deflate payload.
    auto result = dbrecovery::innodb::SDIManager::extract_standard_payload(
        data.data(), data.size());
    return result.success();
}

bool Repairer::repair_page(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, size_t page_size) {
    if (input.size() != page_size || output.size() != page_size) {
        return false;
    }

    // 复制原始数据
    std::copy(input.begin(), input.end(), output.begin());

    // 检查是否损坏
    if (is_page_corrupted(output.data(), page_size)) {
        // 尝试修复校验和
        if (fix_checksum(output.data(), page_size)) {
            // 尝试修复BLOB指针
            fix_blob_pointer(output.data(), page_size);

            // 尝试修复SDI记录
            if (is_page_valid(output.data(), page_size)) {
                fix_sdi_record(output, page_size);
            }
        }
    }

    // 验证修复结果
    return is_page_valid(output.data(), page_size);
}

bool Repairer::repair_file(const std::string& input_file, const std::string& output_file, std::optional<size_t> page_size) {
    // 打开输入文件
    std::ifstream input(input_file, std::ios::binary);
    if (!input) {
        std::cerr << "无法打开输入文件: " << input_file << std::endl;
        return false;
    }

    // 获取文件大小
    input.seekg(0, std::ios::end);
    size_t file_size = static_cast<size_t>(input.tellg());
    input.seekg(0, std::ios::beg);

    // 计算总页数
    size_t page_size_val = page_size.value_or(dbrecovery::innodb::UNIV_PAGE_SIZE);
    if (page_size_val == 0) {
        std::cerr << "无效的页大小" << std::endl;
        return false;
    }

    size_t total_pages = file_size / page_size_val;
    if (total_pages == 0) {
        std::cerr << "文件太小，不是有效的InnoDB文件" << std::endl;
        return false;
    }

    // 创建输出文件
    std::ofstream output(output_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "无法创建输出文件: " << output_file << std::endl;
        return false;
    }

    // 读取整个文件到内存（为修复操作做准备）
    std::vector<uint8_t> file_data(file_size);
    input.read(reinterpret_cast<char*>(file_data.data()), file_size);

    // 修复每一页
    std::vector<uint8_t> input_page(page_size_val);
    std::vector<uint8_t> output_page(page_size_val);

    size_t repaired_pages = 0;
    size_t skipped_pages = 0;

    for (size_t i = 0; i < total_pages; ++i) {
        // 读取页面
        std::memcpy(input_page.data(), file_data.data() + i * page_size_val, page_size_val);

        // 尝试修复
        if (repair_page(input_page, output_page, page_size_val)) {
            // 将修复后的页面写入文件
            std::memcpy(file_data.data() + i * page_size_val, output_page.data(), page_size_val);
            repaired_pages++;
        } else {
            // 无法修复，使用原始数据
            skipped_pages++;
        }
    }

    // 将修复后的数据写回文件
    output.write(reinterpret_cast<const char*>(file_data.data()), file_size);

    std::cout << "修复完成: " << repaired_pages << " 页已修复, "
              << skipped_pages << " 页跳过" << std::endl;

    return true;
}

} // namespace repair
} // namespace dbrecovery
