#include "dbrecovery/recovery/raw_disk_carver.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/frm_parser.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/utils/lz4_helper.hpp"
#include "nlohmann/json.hpp"

#include <chrono>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <iomanip>

#ifdef __linux__
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#endif

namespace dbrecovery {
namespace recovery {

using namespace dbrecovery::innodb;

// FRM文件签名: \xfe\x01\n\x0c\x03\x00\x00\x10\x01\x00
static constexpr uint8_t FRM_SIGNATURE[] = {0xfe, 0x01, 0x0a, 0x0c, 0x03, 0x00, 0x00, 0x10, 0x01, 0x00};

RawDiskCarver::RawDiskCarver() = default;
RawDiskCarver::~RawDiskCarver() = default;

void RawDiskCarver::set_config(const Config& config) {
    config_ = config;
}

void RawDiskCarver::set_external_schema(const catalog::TableSchema& schema) {
    external_schema_ = schema;
}

// Page validation: checks if LSN in header matches LSN in footer
bool RawDiskCarver::is_valid_page_strict(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 64) return false;

    // Check page type first
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);

    // For compressed pages, try LZ4 decompression first then validate
    if (page_type == FIL_PAGE_COMPRESSED || page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
        // For compressed+encrypted, decryption is needed first — skip for now
        if (page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
            return true;  // Accept as valid, TDE handling happens downstream
        }
        // Try LZ4 decompression: compressed data starts after ZSD header (32 bytes)
        if (page_size > 32) {
            uint32_t orig_size = read_le<uint32_t>(data, 14);
            uint16_t zlen = read_le<uint16_t>(data, 8);
            if (zlen > 0 && zlen < page_size - 32) {
                auto decompressed = dbrecovery::utils::LZ4Helper::decompress(data + 32, zlen, orig_size);
                if (!decompressed.empty()) {
                    // Validate decompressed page
                    uint16_t decomp_type = read_be<uint16_t>(decompressed.data(), FIL_PAGE_TYPE_ABS);
                    if (decomp_type == FIL_PAGE_INDEX) {
                        return true;
                    }
                }
            }
        }
        return false;  // Can't validate compressed page
    }

    // Valid page types
    switch (page_type) {
        case FIL_PAGE_INDEX:        // 0x45BF
        case FIL_PAGE_SDI:          // 0x45BE
        case FIL_PAGE_SDI_ZIP:      // 0x45BD
        case FIL_PAGE_UNDO_LOG:     // 0x45B2
        case FIL_PAGE_COMPRESSED:   // 0x45B5
        case FIL_PAGE_COMPRESSED_AND_ENCRYPTED: // 0x45B7
        case FIL_PAGE_TYPE_ALLOCATED: // 0x0000
        case 0x0003:  // FIL_PAGE_INODE
        case 0x0005:  // FIL_PAGE_IBUF_BITMAP
        case 0x0008:  // FIL_PAGE_FSP_HEADER
            break;
        default:
            return false;
    }

    // IS_PAGE check: compare header and footer LSN
    uint32_t checksum_header = read_be<uint32_t>(data, 0);
    uint32_t lsn_low_header = read_be<uint32_t>(data, 4);
    uint32_t checksum_footer = read_be<uint32_t>(data, page_size - 8);
    uint32_t lsn_low_footer = read_be<uint32_t>(data, page_size - 4);

    // For allocated pages, skip LSN check
    if (page_type == FIL_PAGE_TYPE_ALLOCATED) {
        return true;
    }

    // Strict LSN match
    if (checksum_header == checksum_footer && lsn_low_header == lsn_low_footer) {
        return true;
    }

    // Relaxed check: allow small differences for deleted/recovered pages
    // The LSN values should at least be non-zero
    if (checksum_header != 0 || checksum_footer != 0) {
        return true;
    }

    return false;
}

bool RawDiskCarver::is_valid_page(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 64) return false;

    // InnoDB页验证策略:
    // 1. 检查页类型是否有效 (offset 24-25)
    // 2. 对于INDEX页，检查额外的结构完整性
    //
    // 注意：LSN检查在真实文件中可能不严格匹配
    // 实际文件可能LSN不完全匹配（如恢复后的文件、压缩页等）

    try {
        // 检查页类型 (offset 24-25)
        uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);

        // 有效页类型列表
        bool is_valid_type = false;
        switch (page_type) {
            case FIL_PAGE_INDEX:        // 0x45BF - INDEX页
            case FIL_PAGE_SDI:          // 0x45BE - SDI页
            case FIL_PAGE_SDI_ZIP:      // 0x45BD - 压缩SDI页/BLOB页
            case FIL_PAGE_UNDO_LOG:     // 0x45B2 - Undo日志页
            case FIL_PAGE_COMPRESSED:   // 0x45B5 - 压缩页
            case FIL_PAGE_COMPRESSED_AND_ENCRYPTED: // 0x45B7
            case FIL_PAGE_TYPE_ALLOCATED: // 0x0000 - 已分配
            case 0x0003:  // FIL_PAGE_INODE
            case 0x0005:  // FIL_PAGE_IBUF_BITMAP
            case 0x0008:  // FIL_PAGE_FSP_HEADER
                is_valid_type = true;
                break;
            default:
                // Try LZ4 decompression for potentially compressed pages
                if (page_size > 32) {
                    uint32_t orig_size = read_le<uint32_t>(data, 14);
                    uint16_t zlen = read_le<uint16_t>(data, 8);
                    if (zlen > 0 && zlen < page_size - 32 && orig_size > 0 && orig_size < 65536) {
                        auto decompressed = dbrecovery::utils::LZ4Helper::decompress(data + 32, zlen, orig_size);
                        if (!decompressed.empty() && decompressed.size() >= FIL_PAGE_TYPE_ABS + 2) {
                            uint16_t decomp_type = read_be<uint16_t>(decompressed.data(), FIL_PAGE_TYPE_ABS);
                            if (decomp_type == FIL_PAGE_INDEX) {
                                is_valid_type = true;
                                break;
                            }
                        }
                    }
                }
                return false;
        }

        // 对于INDEX页，额外检查index_id是否合理（非零）
        if (page_type == FIL_PAGE_INDEX && page_size >= 74) {
            uint64_t index_id = read_be<uint64_t>(data, 66);
            if (index_id == 0) {
                return false;  // INDEX页必须有有效的index_id
            }
        }

        // 可选的LSN验证（放宽条件）
        // 只检查LSN字段是否非零（对于活跃页）
        if (page_type != FIL_PAGE_TYPE_ALLOCATED && page_type != 0x0000) {
            uint64_t lsn_header = read_be<uint64_t>(data, 0);
            uint64_t lsn_footer = read_be<uint64_t>(data, page_size - 8);

            // LSN应该有一些有效的值（不全是零）
            if (lsn_header == 0 && lsn_footer == 0) {
                return false;
            }

            // 对于INDEX页，尝试检查LSN的下半部分是否接近
            // header的LSN low部分（offset 4-7）应该接近footer的LSN low部分
            uint32_t lsn_header_low = read_be<uint32_t>(data, 4);
            uint32_t lsn_footer_low = read_be<uint32_t>(data, page_size - 4);

            // 允许一定的偏差（最多相差10000，对应日志刷新差异）
            uint32_t diff = (lsn_header_low > lsn_footer_low) ?
                           (lsn_header_low - lsn_footer_low) :
                           (lsn_footer_low - lsn_header_low);
            if (diff > 10000 && lsn_header_low != 0 && lsn_footer_low != 0) {
                // LSN差异过大，可能是损坏的页
                // 但仍然返回true以允许恢复尝试
                // 只是标记为可能损坏
            }
        }

        return is_valid_type;
    } catch (...) {
        // 异常时返回false
        return false;
    }
}

bool RawDiskCarver::is_index_page(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 26) return false;
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    return page_type == FIL_PAGE_INDEX; // 0x45BF
}

bool RawDiskCarver::is_sdi_page(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 26) return false;
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    return page_type == FIL_PAGE_SDI || page_type == FIL_PAGE_SDI_ZIP;
}

bool RawDiskCarver::is_blob_page(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 26) return false;
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    // FIL_PAGE_SDI_ZIP (0x45BD) is used for both compressed SDI and BLOB pages
    // FIL_PAGE_COMPRESSED (0x45B5) is used for compressed pages
    return page_type == FIL_PAGE_SDI_ZIP || page_type == FIL_PAGE_COMPRESSED;
}

/**
 * @brief 检测FRM文件签名
 */
bool RawDiskCarver::is_frm_signature(const uint8_t* data, size_t len) {
    if (!data || len < 10) return false;
    return std::memcmp(data, FRM_SIGNATURE, 10) == 0;
}

uint64_t RawDiskCarver::extract_index_id(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 74) return 0;
    // index_id 在 offset 66-74 (8字节大端序)
    return read_be<uint64_t>(data, 66);
}

/**
 * @brief 从页数据提取space_id
 * space_id在offset 34-38 (4字节大端序)
 */
uint32_t RawDiskCarver::extract_space_id(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 38) return 0;
    return read_be<uint32_t>(data, 34);
}

/**
 * @brief 从页数据提取page_no
 * page_no在offset 4-7 (FIL_PAGE_OFFSET, 4字节大端序)
 */
uint32_t RawDiskCarver::extract_page_no(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 8) return 0;
    return read_be<uint32_t>(data, FIL_PAGE_OFFSET);
}

/**
 * @brief 检测页大小 (支持4KB/8KB/16KB/32KB/64KB)
 * MySQL 5.6-9.0 允许的页大小
 */
uint32_t RawDiskCarver::detect_page_size(const uint8_t* data, size_t max_len) {
    static constexpr std::array<uint32_t, 5> valid_sizes = {4096, 8192, 16384, 32768, 65536};

    for (uint32_t ps : valid_sizes) {
        if (max_len >= ps && is_valid_page(data, ps)) {
            return ps;
        }
    }
    return 0; // 无法检测
}

RawDiskCarver::ScanResult RawDiskCarver::scan(std::function<void(double, const std::string&)> progress) {
    ScanResult result;
    auto start_time = std::chrono::high_resolution_clock::now();

    // 检查设备路径
    if (config_.device_path.empty()) {
        result.errors.push_back("Device path not specified");
        return result;
    }

    // 获取设备/路径大小
    uint64_t device_size = 0;

#ifdef __linux__
    struct stat st;
    if (stat(config_.device_path.c_str(), &st) != 0) {
        result.errors.push_back("Cannot access device: " + config_.device_path);
        return result;
    }

    if (S_ISBLK(st.st_mode)) {
        // 块设备 - 使用ioctl获取大小
        int fd = open(config_.device_path.c_str(), O_RDONLY);
        if (fd >= 0) {
            ioctl(fd, BLKGETSIZE64, &device_size);
            close(fd);
        }
    } else if (S_ISREG(st.st_mode)) {
        // 普通文件
        device_size = st.st_size;
    } else if (S_ISDIR(st.st_mode)) {
        // 目录 - 递归扫描所有文件
        // 目录扫描模式：扫描目录中的所有.ibd文件
        if (config_.verbose) {
            std::cout << "[INFO] Directory scan mode: scanning all files in " << config_.device_path << "\n";
        }

        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(config_.device_path)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    // 扫描所有文件（不限于.ibd），因为可能有残留数据
                    std::string filepath = entry.path().string();

                    // 获取文件大小
                    try {
                        uint64_t file_size = std::filesystem::file_size(entry.path());
                        device_size += file_size;

                        if (config_.verbose && file_size > 0) {
                            std::cout << "[INFO] Found file: " << filepath << " (" << file_size << " bytes)\n";
                        }
                    } catch (...) {
                        // Skip files we can't read
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            result.errors.push_back("Directory scan error: " + std::string(e.what()));
            return result;
        }

        if (config_.verbose) {
            std::cout << "[INFO] Total files size: " << device_size << " bytes\n";
        }
    }
#else
    // 非Linux系统，使用文件系统获取大小
    try {
        if (std::filesystem::is_directory(config_.device_path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(config_.device_path)) {
                if (entry.is_regular_file()) {
                    try {
                        device_size += std::filesystem::file_size(entry.path());
                    } catch (...) {}
                }
            }
        } else {
            device_size = std::filesystem::file_size(config_.device_path);
        }
    } catch (...) {
        result.errors.push_back("Cannot get device size");
        return result;
    }
#endif

    if (device_size == 0) {
        result.errors.push_back("Device size is 0");
        return result;
    }

    // 计算实际扫描范围
    uint64_t scan_stop = std::min(config_.offset_stop, device_size);

    // 存储实际扫描范围到成员变量
    actual_scan_stop_ = scan_stop;

    if (config_.verbose) {
        std::cout << "[INFO] Device size: " << device_size << " bytes ("
                  << (device_size / 1024.0 / 1024.0 / 1024.0) << " GB)\n";
        std::cout << "[INFO] Scan range: " << config_.offset_start << " - " << scan_stop << "\n";
        std::cout << "[INFO] Page size: " << config_.page_size << "\n";
        std::cout << "[INFO] Workers: " << config_.num_workers << "\n";
    }

    // 初始化原子变量
    current_offset_ = config_.offset_start;
    pages_scanned_ = 0;
    pages_found_ = 0;
    should_stop_ = false;
    collected_pages_.clear();

    // 创建工作线程
    std::vector<std::thread> workers;
    for (uint32_t i = 0; i < config_.num_workers; ++i) {
        workers.emplace_back(&RawDiskCarver::worker_thread, this, i, progress);
    }

    // 等待所有工作线程完成
    for (auto& w : workers) {
        w.join();
    }

    // 收集结果
    result.total_pages_scanned = pages_scanned_.load();
    result.valid_pages_found = pages_found_.load();

    // 合并收集的页
    {
        std::lock_guard<std::mutex> lock(pages_mutex_);
        for (auto& [index_id, pages] : collected_pages_) {
            for (auto& page : pages) {
                result.pages_by_index_id[index_id].push_back(std::move(page));
            }
        }
    }

    // 统计各类页
    for (const auto& [index_id, pages] : result.pages_by_index_id) {
        result.index_pages_found += pages.size();
    }

    // 从 last_scan_result_ 复制 SDI 页（工作线程已经存储到这里）
    result.sdi_pages = last_scan_result_.sdi_pages;
    result.sdi_pages_found = last_scan_result_.sdi_pages_found;

    auto end_time = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
    result.success = true;

    if (config_.verbose) {
        std::cout << "[INFO] Scan completed:\n";
        std::cout << "  Pages scanned: " << result.total_pages_scanned << "\n";
        std::cout << "  Valid pages: " << result.valid_pages_found << "\n";
        std::cout << "  INDEX pages: " << result.index_pages_found << "\n";
        std::cout << "  SDI pages: " << result.sdi_pages_found << "\n";
        std::cout << "  Unique index_ids: " << result.pages_by_index_id.size() << "\n";
        std::cout << "  Time: " << result.elapsed_seconds << "s\n";
    }

    last_scan_result_ = result;
    return result;
}

void RawDiskCarver::worker_thread(uint32_t worker_id,
                                   std::function<void(double, const std::string&)> progress) {
    // 使用pread优化：避免ifstream的seekg开销，支持真正的并行IO
#ifdef __linux__
    int fd = open(config_.device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (config_.verbose) {
            std::cerr << "[Worker " << worker_id << "] Cannot open device\n";
        }
        return;
    }
#else
    std::ifstream device(config_.device_path, std::ios::binary);
    if (!device) {
        if (config_.verbose) {
            std::cerr << "[Worker " << worker_id << "] Cannot open device\n";
        }
        return;
    }
#endif

    // 优化大磁盘扫描：使用更大的缓冲区和智能扫描步长
    uint64_t scan_chunk_size = config_.chunk_size;
    std::vector<uint8_t> buffer(scan_chunk_size + config_.page_size);

    // 智能选择扫描步长：
    // - 页对齐扫描（4KB步长）用于正常情况
    // - 细粒度扫描（512字节）用于非对齐页恢复
    uint64_t scan_step = config_.use_page_aligned_scan ? config_.scan_step : 512;

    auto last_progress_time = std::chrono::steady_clock::now();
    uint64_t pages_in_chunk = 0;

    while (!should_stop_.load()) {
        // 获取下一个扫描块
        uint64_t chunk_start = current_offset_.fetch_add(scan_chunk_size);

        if (chunk_start >= actual_scan_stop_) {
            break;
        }

        uint64_t chunk_end = std::min(chunk_start + scan_chunk_size, actual_scan_stop_);
        uint64_t chunk_size = chunk_end - chunk_start;

        // 读取数据 - 使用pread进行更高效的并行IO
        size_t bytes_read = 0;
#ifdef __linux__
        // 使用pread直接读取，避免seekg开销
        ssize_t ret = pread(fd, buffer.data(), chunk_size, chunk_start);
        if (ret <= 0) {
            break;
        }
        bytes_read = static_cast<size_t>(ret);
#else
        device.seekg(chunk_start, std::ios::beg);
        device.read(reinterpret_cast<char*>(buffer.data()), chunk_size);
        if (!device || device.gcount() == 0) {
            break;
        }
        bytes_read = device.gcount();
#endif

        pages_in_chunk = 0;

        // 扫描页 - 使用多种扫描策略
        // 1. 页对齐扫描（高效）
        // 2. 细粒度扫描（捕获非对齐页）
        // 3. 签名扫描（捕获特殊页类型）

        for (uint64_t offset = 0; offset + config_.page_size <= bytes_read; offset += scan_step) {
            const uint8_t* page_data = buffer.data() + offset;

            // 快速预检查：先检查页类型再验证完整页
            uint16_t ptype = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);

            // 快速过滤无效页类型
            bool quick_valid = (ptype == FIL_PAGE_INDEX ||
                               ptype == FIL_PAGE_SDI ||
                               ptype == FIL_PAGE_SDI_ZIP ||
                               ptype == FIL_PAGE_UNDO_LOG ||
                               ptype == FIL_PAGE_COMPRESSED ||
                               ptype == FIL_PAGE_TYPE_ALLOCATED ||
                               ptype == 0x0003 || ptype == 0x0005 || ptype == 0x0008);

            if (!quick_valid) {
                // 额外检查：infimum/supremum系统记录检查
                // infimum记录在offset 54，记录头5字节后是"infimum\0"
                // 所以"infimum"字符串在offset 54+5=59
                constexpr uint16_t INFIMUM_STRING_OFFSET = 59;  // 54 + 5 bytes record header
                if (offset + INFIMUM_STRING_OFFSET + 8 <= bytes_read) {
                    const uint8_t* infimum_pos = page_data + INFIMUM_STRING_OFFSET;
                    if (memcmp(infimum_pos, "infimum", 7) == 0) {
                        // 找到infimum标记，可能是一个有效的INDEX页
                        // 即使页类型不在标准列表中
                        ptype = FIL_PAGE_INDEX;
                        quick_valid = true;
                    }
                }
                if (!quick_valid) continue;
            }

            // 使用两种页验证策略
            // 1. 标准验证（宽松）
            bool standard_valid = is_valid_page(page_data, config_.page_size);
            // 2. 严格验证
            bool strict_valid = is_valid_page_strict(page_data, config_.page_size);

            // 对于删除文件的恢复，放宽验证条件
            // 只要页类型有效，就尝试恢复
            if (!standard_valid && !strict_valid) {
                // 最后一次尝试：检查是否有有效的LSN
                uint64_t lsn_header = read_be<uint64_t>(page_data, 0);
                uint64_t lsn_footer = read_be<uint64_t>(page_data, config_.page_size - 8);
                if (lsn_header == 0 && lsn_footer == 0) {
                    continue;  // 完全无效的页
                }
                // 有LSN但验证失败，可能是损坏页，仍然尝试
            }

            pages_scanned_.fetch_add(1);
            pages_in_chunk++;

            // 检测页类型并收集
            PageType page_type = PageType::UNKNOWN;

            if (ptype == FIL_PAGE_INDEX) {
                page_type = PageType::INDEX;
            } else if (ptype == FIL_PAGE_SDI) {
                page_type = PageType::SDI;
            } else if (ptype == FIL_PAGE_SDI_ZIP) {
                page_type = PageType::SDI_ZIP;
            } else if (ptype == FIL_PAGE_TYPE_ALLOCATED) {
                page_type = PageType::ALLOCATED;
            }

            if (is_index_page(page_data, config_.page_size) || ptype == FIL_PAGE_INDEX) {
                uint64_t index_id = extract_index_id(page_data, config_.page_size);

                // index_id过滤逻辑 (支持TRUNCATE恢复)
                bool should_collect = true;

                // 如果指定了目标index_id，只收集这些
                if (!config_.target_index_ids.empty()) {
                    should_collect = std::find(config_.target_index_ids.begin(),
                                              config_.target_index_ids.end(),
                                              index_id) != config_.target_index_ids.end();
                }

                // 排除已知的系统表index_id
                if (should_collect && !config_.exclude_index_ids.empty()) {
                    should_collect = std::find(config_.exclude_index_ids.begin(),
                                              config_.exclude_index_ids.end(),
                                              index_id) == config_.exclude_index_ids.end();
                }

                if (should_collect) {
                    // 提取页数据
                    std::vector<uint8_t> page_copy(config_.page_size);
                    std::memcpy(page_copy.data(), page_data, config_.page_size);

                    {
                        std::lock_guard<std::mutex> lock(pages_mutex_);
                        collected_pages_[index_id].push_back(std::move(page_copy));
                    }

                    pages_found_.fetch_add(1);
                }
            }

            // 也检测SDI页用于提取schema
            if (config_.scan_sdi_pages && is_sdi_page(page_data, config_.page_size)) {
                // 提取SDI页数据
                std::vector<uint8_t> sdi_copy(config_.page_size);
                std::memcpy(sdi_copy.data(), page_data, config_.page_size);

                uint32_t space_id = extract_space_id(page_data, config_.page_size);

                {
                    std::lock_guard<std::mutex> lock(pages_mutex_);
                    // 存储SDI页用于后续schema提取
                    last_scan_result_.sdi_pages[space_id] = sdi_copy;
                    last_scan_result_.sdi_pages_found++;
                    pages_found_.fetch_add(1);
                }
            }

            // 检测FRM签名
            if (config_.scan_frm_files) {
                for (size_t frm_offset = 0; frm_offset + 10 <= config_.page_size; frm_offset++) {
                    if (is_frm_signature(page_data + frm_offset, config_.page_size - frm_offset)) {
                        pages_found_.fetch_add(1);
                    }
                }
            }
        }

        // 数据模式扫描：用于删除文件恢复
        // 当指定了data_patterns时，搜索这些模式并提取周围的数据块
        if (config_.scan_data_patterns && !config_.data_patterns.empty()) {
            for (const auto& pattern : config_.data_patterns) {
                if (pattern.empty()) continue;

                // 在当前缓冲区中搜索模式
                for (size_t search_offset = 0; search_offset + pattern.size() <= bytes_read; search_offset++) {
                    if (memcmp(buffer.data() + search_offset, pattern.data(), pattern.size()) == 0) {
                        // 找到匹配！
                        // 计算包含该模式的页对齐数据块
                        uint64_t abs_offset = chunk_start + search_offset;
                        uint64_t page_start = (abs_offset / config_.page_size) * config_.page_size;

                        // 确保不越界
                        if (search_offset >= (abs_offset - page_start)) {
                            size_t local_page_start = search_offset - (abs_offset - page_start);
                            if (local_page_start + config_.page_size <= bytes_read) {
                                const uint8_t* potential_page = buffer.data() + local_page_start;

                                // 尝试将其作为INDEX页解析
                                uint64_t index_id = extract_index_id(potential_page, config_.page_size);
                                if (index_id > 0) {
                                    // 收集该页
                                    std::vector<uint8_t> page_copy(config_.page_size);
                                    std::memcpy(page_copy.data(), potential_page, config_.page_size);

                                    {
                                        std::lock_guard<std::mutex> lock(pages_mutex_);
                                        collected_pages_[index_id].push_back(std::move(page_copy));
                                    }
                                    pages_found_.fetch_add(1);
                                }
                            }
                        }
                    }
                }
            }
        }

        // 进度报告（优化：使用毫秒级间隔）
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_progress_time).count();

        if (progress && elapsed_ms >= config_.progress_interval_ms) {
            double pct = static_cast<double>(chunk_start - config_.offset_start) /
                        static_cast<double>(actual_scan_stop_ - config_.offset_start) * 100.0;

            // 计算扫描速度
            double speed_mbps = 0.0;
            if (elapsed_ms > 0) {
                speed_mbps = (chunk_size / 1024.0 / 1024.0) / (elapsed_ms / 1000.0);
            }

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << pct << "%"
                << " (" << std::setprecision(0) << speed_mbps << " MB/s)"
                << " pages=" << pages_scanned_.load()
                << " found=" << pages_found_.load();
            progress(pct / 100.0, oss.str());
            last_progress_time = now;
        }
    }

#ifdef __linux__
    // 关闭文件描述符
    if (fd >= 0) {
        close(fd);
    }
#endif
}

RawDiskCarver::RecoveryResult RawDiskCarver::recover(
    const ScanResult& scan_result,
    std::function<void(double, const std::string&)> progress) {

    RecoveryResult result;

    if (!scan_result.success) {
        result.errors.push_back("Scan was not successful");
        return result;
    }

    // 创建输出目录
    try {
        std::filesystem::create_directories(config_.output_dir);
    } catch (const std::exception& e) {
        result.errors.push_back("Cannot create output directory: " + std::string(e.what()));
        return result;
    }

    // Step 1: 从SDI页提取schema (如果有)
    std::map<uint64_t, catalog::TableSchema> schemas_by_index_id;

    if (!scan_result.sdi_pages.empty()) {
        for (const auto& [space_id, sdi_data] : scan_result.sdi_pages) {
            auto schema_opt = extract_schema_from_sdi(sdi_data);
            if (schema_opt.has_value()) {
                // 假设每个表的主键index_id可以从schema推断
                // 这里简化处理，将schema应用到所有index_id
                // 实际应该根据index_id匹配
                if (schema_opt->columns.size() > 0) {
                    // 计算index_id (简化：使用第一个列的table信息)
                    // 实际应该从SDI中提取index信息
                    std::cerr << "[INFO] Extracted schema from SDI for space_id " << space_id
                              << ": " << schema_opt->columns.size() << " columns" << std::endl;
                    // 暂存schema，后续根据index_id匹配
                    result.inferred_schemas[space_id] = *schema_opt;
                }
            }
        }
    }

    size_t total_index_ids = scan_result.pages_by_index_id.size();
    size_t processed = 0;

    for (const auto& [index_id, pages] : scan_result.pages_by_index_id) {
        if (progress) {
            double pct = static_cast<double>(processed) / total_index_ids;
            progress(pct, "Recovering index_id " + std::to_string(index_id));
        }

        // 查找是否有匹配的schema
        std::optional<catalog::TableSchema> matched_schema;

        // 优先使用外部提供的schema (如FRM文件解析)
        if (external_schema_.has_value() && !external_schema_->columns.empty()) {
            matched_schema = external_schema_;
            if (dbrecovery::innodb::g_verbose)             std::cerr << "[DEBUG] Using external schema with " << matched_schema->columns.size()
                      << " columns for index_id " << index_id << std::endl;
        } else {
            // 尝试从SDI提取的schema中匹配
            for (const auto& [space_id, schema] : result.inferred_schemas) {
                // 简化匹配: 如果schema有列，就使用它
                // 实际应该根据index_id匹配
                if (!schema.columns.empty()) {
                    matched_schema = schema;
                    break;
                }
            }
        }

        // 对每个inex_id的页进行解析
        std::vector<std::map<std::string, std::string>> all_records;

        for (const auto& page_data : pages) {
            if (matched_schema.has_value()) {
                // 使用schema-aware解析
                auto records = parse_records_with_schema(page_data, *matched_schema);
                all_records.insert(all_records.end(), records.begin(), records.end());
            } else {
                // 使用schemaless解析
                auto records = parse_records_schemaless(page_data);
                all_records.insert(all_records.end(), records.begin(), records.end());
            }
        }

        if (!all_records.empty()) {
            result.records_by_index_id[index_id] = all_records;
            result.total_records += all_records.size();

            // 导出到CSV
            std::string output_file = config_.output_dir + "/index_" +
                                      std::to_string(index_id) + ".csv";
            std::ofstream out(output_file);
            if (out) {
                // 写入表头
                if (!all_records.empty()) {
                    bool first = true;
                    for (const auto& kv : all_records[0]) {
                        if (!first) out << ",";
                        out << kv.first;
                        first = false;
                    }
                    out << "\n";

                    // 写入数据
                    for (const auto& rec : all_records) {
                        first = true;
                        for (const auto& kv : rec) {
                            if (!first) out << ",";
                            // CSV转义
                            std::string val = kv.second;
                            if (val.find(',') != std::string::npos ||
                                val.find('"') != std::string::npos ||
                                val.find('\n') != std::string::npos) {
                                out << "\"";
                                for (char c : val) {
                                    if (c == '"') out << "\"\"";
                                    else out << c;
                                }
                                out << "\"";
                            } else {
                                out << val;
                            }
                            first = false;
                        }
                        out << "\n";
                    }
                    result.output_files.push_back(output_file);
                }
            }
        }

        processed++;
    }

    result.success = true;
    return result;
}

RawDiskCarver::RecoveryResult RawDiskCarver::scan_and_recover(
    std::function<void(double, const std::string&)> progress) {

    auto scan_result = scan([&](double pct, const std::string& msg) {
        if (progress) progress(pct * 0.7, msg);
    });

    if (!scan_result.success) {
        RecoveryResult result;
        result.errors = scan_result.errors;
        return result;
    }

    return recover(scan_result, [&](double pct, const std::string& msg) {
        if (progress) progress(0.7 + pct * 0.3, msg);
    });
}

dbrecovery::carve::ReassemblyStats RawDiskCarver::reassemble_to_tablespaces(
    const ScanResult& scan_result,
    const std::string& output_dir,
    std::function<void(double, const std::string&)> progress) {

    dbrecovery::carve::PageFragmentReassembler reassembler;

    // Convert FoundPage objects to PageIdentity objects
    for (const auto& found : scan_result.all_found_pages) {
        dbrecovery::carve::PageIdentity page_id;
        page_id.space_id = found.space_id;
        page_id.page_no = found.page_no;
        page_id.page_type = (found.type == RawDiskCarver::PageType::INDEX)  ? 17855 :
                            (found.type == RawDiskCarver::PageType::SDI)    ? 17854 :
                            (found.type == RawDiskCarver::PageType::SDI_ZIP) ? 17853 :
                            (found.type == RawDiskCarver::PageType::UNDO_LOG) ? 17842 :
                            (found.type == RawDiskCarver::PageType::BLOB)   ? 17853 : 0;
        page_id.lsn = 0; // Will be extracted during validation
        page_id.checksum = 0;
        page_id.checksum_valid = true; // Already validated by scanner
        page_id.is_compressed = false;
        page_id.is_encrypted = false;
        page_id.disk_offset = found.offset;
        page_id.data = found.data;
        page_id.page_size = config_.page_size;

        // Try to extract LSN from page data
        if (found.data.size() >= 24) {
            page_id.lsn = dbrecovery::innodb::read_be<uint64_t>(found.data.data(), 16);
        }

        reassembler.add_page(page_id);
    }

    if (progress) {
        progress(0.1, "Grouping " + std::to_string(scan_result.all_found_pages.size()) +
                 " pages by SPACE_ID...");
    }

    auto stats = reassembler.reassemble(output_dir);

    if (progress) {
        progress(1.0, "Reassembly complete: " + std::to_string(stats.spaces_reconstructed) +
                 " tablespaces reconstructed");
    }

    // Print the report
    std::cout << reassembler.generate_report() << std::endl;

    return stats;
}

const RawDiskCarver::ScanResult& RawDiskCarver::get_last_scan_result() const {
    return last_scan_result_;
}

std::vector<std::map<std::string, std::string>> RawDiskCarver::parse_records_schemaless(
    const std::vector<uint8_t>& page_data) {

    std::vector<std::map<std::string, std::string>> records;

    if (page_data.size() < 100) return records;

    const uint8_t* data = page_data.data();
    size_t page_size = page_data.size();

    // INDEX页格式验证
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_INDEX) return records;

    // 首先尝试使用InnoDBIndexParser进行schema-less解析
    // 它能自动检测字段数量和类型
    innodb::InnoDBIndexParser parser;
    parser.set_recover_deleted(config_.recover_deleted);
    parser.set_bad_page_mode(2);  // 暴力模式

    // 尝试推断字段数量
    // 从页目录获取记录数量
    uint16_t n_dir_slots = read_be<uint16_t>(data, 38);
    uint16_t n_heap = read_be<uint16_t>(data, 42) & 0x7FFF;  // 低15位
    uint16_t n_recs = read_be<uint16_t>(data, 54);

    if (n_recs > 0 && n_recs < 1000) {
        // 使用InnoDBIndexParser解析
        auto index_records = parser.parse_page(data, page_size, 0);

        for (const auto& rec : index_records) {
            if (rec.is_deleted && !config_.recover_deleted) continue;

            std::map<std::string, std::string> record;
            for (size_t i = 0; i < rec.fields.size(); i++) {
                std::string field_name = "field_" + std::to_string(i);
                if (rec.fields[i].is_null) {
                    record[field_name] = "NULL";
                } else if (!rec.fields[i].str_value.empty()) {
                    record[field_name] = rec.fields[i].str_value;
                } else if (!rec.fields[i].raw_bytes.empty()) {
                    // 尝试解码raw_bytes为可读格式
                    const auto& bytes = rec.fields[i].raw_bytes;

                    // 检查是否为可打印字符串
                    size_t printable = 0;
                    for (uint8_t b : bytes) {
                        if ((b >= 32 && b < 127) || b == '\n' || b == '\r' || b == '\t') {
                            printable++;
                        }
                    }

                    if (printable >= bytes.size() * 2 / 3 && !bytes.empty()) {
                        // 大部分是可打印字符，作为字符串输出
                        record[field_name] = std::string(bytes.begin(), bytes.end());
                    } else if (bytes.size() == 4) {
                        // 可能是INT或TIMESTAMP
                        uint32_t val = (static_cast<uint32_t>(bytes[0]) << 24) |
                                       (static_cast<uint32_t>(bytes[1]) << 16) |
                                       (static_cast<uint32_t>(bytes[2]) << 8) |
                                       bytes[3];
                        // 检查是否像Unix时间戳
                        if (val >= 946684800 && val <= 4102444800) {
                            time_t t = static_cast<time_t>(val);
                            struct tm tm_buf;
                            gmtime_r(&t, &tm_buf);
                            char buf[32];
                            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
                            record[field_name] = buf;
                        } else {
                            // 作为INT输出 (有符号)
                            int32_t int_val = static_cast<int32_t>(val) - 0x80000000L;
                            record[field_name] = std::to_string(int_val);
                        }
                    } else if (bytes.size() == 8) {
                        // 可能是BIGINT
                        uint64_t val = (static_cast<uint64_t>(bytes[0]) << 56) |
                                       (static_cast<uint64_t>(bytes[1]) << 48) |
                                       (static_cast<uint64_t>(bytes[2]) << 40) |
                                       (static_cast<uint64_t>(bytes[3]) << 32) |
                                       (static_cast<uint64_t>(bytes[4]) << 24) |
                                       (static_cast<uint64_t>(bytes[5]) << 16) |
                                       (static_cast<uint64_t>(bytes[6]) << 8) |
                                       bytes[7];
                        // 检查是否有符号位模式
                        if ((bytes[0] & 0x80) == 0x80) {
                            int64_t int_val = static_cast<int64_t>(val ^ 0x8000000000000000ULL);
                            record[field_name] = std::to_string(int_val);
                        } else {
                            record[field_name] = std::to_string(val);
                        }
                    } else {
                        // 对于其他二进制数据，输出为十六进制
                        std::ostringstream hex;
                        for (uint8_t b : bytes) {
                            hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
                        }
                        record[field_name] = hex.str();
                    }
                }
            }

            if (!record.empty()) {
                records.push_back(record);
            }
        }

        if (!records.empty()) {
            return records;
        }
    }

    // 回退：使用页目录（Page Directory）进行快速记录定位
    if (n_dir_slots < 2 || n_dir_slots > 1000) return records;

    size_t slot_dir_offset = page_size - 8 - n_dir_slots * 2;

    std::set<uint16_t> visited_offsets;

    for (uint16_t slot = 0; slot < n_dir_slots; slot++) {
        size_t slot_pos = slot_dir_offset + slot * 2;
        if (slot_pos + 2 > page_size - 8) break;

        uint16_t rec_offset = read_be<uint16_t>(data, slot_pos);
        if (rec_offset < 100 || rec_offset >= page_size - 50) continue;

        uint16_t current_offset = rec_offset;
        int max_iterations = 20;

        while (max_iterations-- > 0 && current_offset >= 100 && current_offset < page_size - 50) {
            if (visited_offsets.count(current_offset)) break;
            visited_offsets.insert(current_offset);

            // COMPACT格式记录头 (5字节)
            uint8_t n_owned = (data[current_offset + 1] >> 4) & 0x0F;
            uint8_t rec_type = data[current_offset + 2] & 0x07;
            bool is_deleted = (data[current_offset] & 0x10) != 0;

            if (rec_type == 2 || rec_type == 3) break;  // INFIMUM/SUPREMUM

            if (rec_type == 0) {  // ORDINARY
                size_t data_start = current_offset + 5;
                size_t max_data_len = page_size - data_start - 20;

                if (max_data_len > 0 && max_data_len < 8192) {
                    std::map<std::string, std::string> record;

                    // 尝试检测可变长字段
                    size_t field_idx = 0;
                    size_t pos = 0;

                    while (pos < max_data_len && field_idx < 50) {
                        // 检测字符串
                        if (pos + 4 <= max_data_len) {
                            size_t str_len = 0;
                            bool is_printable = true;

                            for (size_t i = 0; i < std::min(max_data_len - pos, (size_t)255); i++) {
                                char c = static_cast<char>(data[data_start + pos + i]);
                                if (c >= 32 && c <= 126) {
                                    str_len++;
                                } else if (c == 0) {
                                    break;
                                } else {
                                    is_printable = false;
                                    break;
                                }
                            }

                            if (str_len >= 2 && is_printable) {
                                std::string value(reinterpret_cast<const char*>(data + data_start + pos), str_len);
                                record["field_" + std::to_string(field_idx++)] = value;
                                pos += str_len;
                            } else {
                                // 尝试作为固定长度数值
                                if (pos + 8 <= max_data_len) {
                                    // 8字节整数
                                    uint64_t val = read_be<uint64_t>(data + data_start + pos);
                                    record["field_" + std::to_string(field_idx++)] = std::to_string(val);
                                    pos += 8;
                                } else if (pos + 4 <= max_data_len) {
                                    uint32_t val = read_be<uint32_t>(data + data_start + pos);
                                    record["field_" + std::to_string(field_idx++)] = std::to_string(val);
                                    pos += 4;
                                } else {
                                    break;
                                }
                            }
                        } else {
                            break;
                        }
                    }

                    if (!record.empty()) {
                        record["_deleted"] = is_deleted ? "1" : "0";
                        records.push_back(record);
                    }
                }
            }

            int16_t next_offset = static_cast<int16_t>(read_be<uint16_t>(data, current_offset + 3));
            if (next_offset <= 0) break;
            current_offset = current_offset + 5 + next_offset;
            if (n_owned > 0) break;
        }
    }

    // 最终回退：SchemaAgnosticScanner
    if (records.empty()) {
        innodb::SchemaAgnosticScanner::Config sa_config;
        sa_config.recover_deleted = config_.recover_deleted;

        innodb::SchemaAgnosticScanner scanner(sa_config);
        auto page_result = scanner.parse_page(page_data.data(), page_data.size(), 0);

        for (const auto& rec : page_result.records) {
            if (rec.is_deleted && !config_.recover_deleted) continue;
            if (!rec.is_valid) continue;

            std::map<std::string, std::string> record;
            for (size_t i = 0; i < rec.fields.size(); i++) {
                std::string field_name = "field_" + std::to_string(i);
                if (rec.fields[i].is_null) {
                    record[field_name] = "NULL";
                } else {
                    record[field_name] = rec.fields[i].str_value;
                }
            }
            records.push_back(record);
        }
    }

    return records;
}

std::vector<std::map<std::string, std::string>> RawDiskCarver::parse_records_with_schema(
    const std::vector<uint8_t>& page_data,
    const catalog::TableSchema& schema) {

    std::vector<std::map<std::string, std::string>> records;

    if (page_data.size() < 100 || schema.columns.empty()) return records;

    const uint8_t* data = page_data.data();
    size_t page_size = page_data.size();

    // INDEX页格式验证
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_INDEX) return records;

    // 将TableSchema转换为IndexColumnDef列表
    // CRITICAL: 必须按physical_pos顺序排列，与InnoDB存储顺序一致
    std::vector<innodb::IndexColumnDef> columns;

    // 先提取并按physical_pos排序
    std::vector<std::pair<uint32_t, catalog::ColumnInfo>> sorted_cols;
    for (const auto& [ordinal, col] : schema.columns) {
        sorted_cols.push_back({col.physical_pos > 0 ? col.physical_pos : ordinal, col});
    }
    std::sort(sorted_cols.begin(), sorted_cols.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [phys_pos, col] : sorted_cols) {
        innodb::IndexColumnDef col_def;
        col_def.name = col.name;
        col_def.col_type = col.mysql_type_id;
        col_def.col_length = col.column_length;
        col_def.is_nullable = col.is_nullable;
        col_def.is_unsigned = !col.is_signed;  // is_signed = true means signed, so is_unsigned = !is_signed
        col_def.precision = col.numeric_precision;
        col_def.scale = col.numeric_scale;
        col_def.datetime_precision = col.datetime_precision;
        col_def.char_set_id = col.charset_id;
        col_def.ordinal_position = col.ordinal;
        col_def.physical_pos = phys_pos;  // CRITICAL: 设置physical_pos
        col_def.enum_values = col.enum_values;
        col_def.set_values = col.set_values;

        // 计算mb_maxlen from charset
        // utf8mb4 = 4, utf8 = 3, latin1 = 1
        if (col.charset_id == 45 || col.charset_id == 46) {  // utf8mb4
            col_def.mb_maxlen = 4;
        } else if (col.charset_id == 33 || col.charset_id == 83) {  // utf8
            col_def.mb_maxlen = 3;
        }

        columns.push_back(col_def);
    }

    // 使用InnoDBIndexParser进行schema-aware解析
    innodb::InnoDBIndexParser parser;
    parser.set_columns(columns);
    parser.set_recover_deleted(config_.recover_deleted);
    parser.set_bad_page_mode(2);

    auto index_records = parser.parse_page(data, page_size, 0);

    // 转换为map格式
    for (const auto& rec : index_records) {
        if (rec.is_deleted && !config_.recover_deleted) continue;

        std::map<std::string, std::string> record;
        for (size_t i = 0; i < rec.fields.size() && i < columns.size(); i++) {
            const auto& col = columns[i];
            if (rec.fields[i].is_null) {
                record[col.name] = "NULL";
            } else {
                record[col.name] = rec.fields[i].str_value;
            }
        }
        if (!record.empty()) {
            records.push_back(record);
        }
    }

    if (dbrecovery::innodb::g_verbose) 
    std::cerr << "[DEBUG] Schema-aware parsing found " << records.size() << " records" << std::endl;
    return records;
}

std::vector<std::pair<size_t, size_t>> RawDiskCarver::detect_field_boundaries(
    const uint8_t* record_start, size_t max_len) {
    // 启发式字段边界检测

    std::vector<std::pair<size_t, size_t>> boundaries;

    // 简化实现: 使用固定长度分割
    // 实际实现应根据数据类型特征进行智能检测
    const size_t min_field_size = 4;
    const size_t max_field_size = 256;

    size_t pos = 0;
    while (pos + min_field_size < max_len) {
        // 尝试检测字符串字段 (可打印字符序列)
        size_t str_len = 0;
        while (pos + str_len < max_len && str_len < max_field_size) {
            char c = static_cast<char>(record_start[pos + str_len]);
            if (isprint(c) && c != '\0') {
                str_len++;
            } else {
                break;
            }
        }

        if (str_len >= min_field_size) {
            boundaries.push_back({pos, str_len});
            pos += str_len;
        } else {
            // 跳过非字符串字段
            pos += min_field_size;
        }
    }

    return boundaries;
}

// Helper function to parse SDI JSON to TableSchema
// static
catalog::TableSchema RawDiskCarver::parse_sdi_json_to_schema(const nlohmann::json& json_obj) {
    catalog::TableSchema schema;

    // MySQL 8.0+ SDI format: dd_object contains the table definition
    // Format 1: {"dd_object_type":"Table", "dd_object": {...}}
    // Format 2: {"dictionary": {"table": {...}}}

    nlohmann::json table_json;

    // Check for dd_object format (MySQL 8.0.46+ SDI format)
    if (json_obj.contains("dd_object") && json_obj["dd_object_type"] == "Table") {
        table_json = json_obj["dd_object"];
        if (dbrecovery::innodb::g_verbose)         std::cerr << "[DEBUG SDI] Using dd_object format" << std::endl;
    }
    // Check for dictionary format
    else if (json_obj.contains("dictionary")) {
        auto dict = json_obj["dictionary"];
        if (dict.contains("table")) {
            table_json = dict["table"];
            if (dbrecovery::innodb::g_verbose)             std::cerr << "[DEBUG SDI] Using dictionary format" << std::endl;
        }
    }
    // Skip Tablespace entries (not table schema)
    else if (json_obj.contains("dd_object_type") && json_obj["dd_object_type"] == "Tablespace") {
        if (dbrecovery::innodb::g_verbose)         std::cerr << "[DEBUG SDI] Skipping Tablespace entry" << std::endl;
        return schema;  // Empty schema
    }

    if (!table_json.is_null() && table_json.contains("columns")) {
        schema.table_name = table_json.value("name", "");

        uint32_t ordinal = 1;
        for (const auto& col : table_json["columns"]) {
            catalog::ColumnInfo column_info;
            column_info.name = col.value("name", "");

            // Handle type field - can be string or number
            if (col.contains("type")) {
                if (col["type"].is_number()) {
                    // Type is numeric (MySQL 8.0 SDI format)
                    column_info.mysql_type_id = col["type"].get<int>();
                } else if (col["type"].is_string()) {
                    std::string type_str = col["type"].get<std::string>();
                    // Map InnoDB type strings to internal IDs
                    if (type_str == "DECIMAL") column_info.mysql_type_id = 1;
                    else if (type_str == "TINY") column_info.mysql_type_id = 2;
                    else if (type_str == "SHORT") column_info.mysql_type_id = 3;
                    else if (type_str == "LONG") column_info.mysql_type_id = 4;
                    else if (type_str == "FLOAT") column_info.mysql_type_id = 5;
                    else if (type_str == "DOUBLE") column_info.mysql_type_id = 6;
                    else if (type_str == "TIMESTAMP") column_info.mysql_type_id = 8;
                    else if (type_str == "LONGLONG") column_info.mysql_type_id = 9;
                    else if (type_str == "INT24") column_info.mysql_type_id = 10;
                    else if (type_str == "DATE") column_info.mysql_type_id = 11;
                    else if (type_str == "TIME") column_info.mysql_type_id = 12;
                    else if (type_str == "DATETIME") column_info.mysql_type_id = 13;
                    else if (type_str == "YEAR") column_info.mysql_type_id = 14;
                    else if (type_str == "NEWDATE") column_info.mysql_type_id = 15;
                    else if (type_str == "VARCHAR") column_info.mysql_type_id = 16;
                    else if (type_str == "BIT") column_info.mysql_type_id = 17;
                    else if (type_str == "TIMESTAMP2") column_info.mysql_type_id = 18;
                    else if (type_str == "DATETIME2") column_info.mysql_type_id = 19;
                    else if (type_str == "TIME2") column_info.mysql_type_id = 20;
                    else if (type_str == "NEWDECIMAL") column_info.mysql_type_id = 21;
                    else if (type_str == "ENUM") column_info.mysql_type_id = 22;
                    else if (type_str == "SET") column_info.mysql_type_id = 23;
                    else if (type_str == "TINY_BLOB") column_info.mysql_type_id = 24;
                    else if (type_str == "MEDIUM_BLOB") column_info.mysql_type_id = 25;
                    else if (type_str == "LONG_BLOB") column_info.mysql_type_id = 26;
                    else if (type_str == "BLOB") column_info.mysql_type_id = 27;
                    else if (type_str == "VAR_STRING") column_info.mysql_type_id = 28;
                    else if (type_str == "STRING") column_info.mysql_type_id = 29;
                    else if (type_str == "GEOMETRY") column_info.mysql_type_id = 30;
                    else if (type_str == "JSON") column_info.mysql_type_id = 31;
                    else if (type_str == "VECTOR") column_info.mysql_type_id = 32;
                }
            }

            column_info.is_nullable = col.value("is_nullable", true);

            if (col.contains("length")) {
                column_info.column_length = col["length"];
            }

            if (col.contains("char_length")) {
                column_info.column_length = col["char_length"];
            }

            if (col.contains("charset_id")) {
                column_info.charset_id = col["charset_id"];
                column_info.collation_id = col.value("collation_id", 255);
            }

            if (col.contains("numeric_precision")) {
                column_info.numeric_precision = col["numeric_precision"];
            }
            if (col.contains("numeric_scale")) {
                column_info.numeric_scale = col["numeric_scale"];
            }

            if (col.contains("datetime_precision")) {
                if (!col["datetime_precision"].is_null()) {
                    column_info.datetime_precision = col["datetime_precision"].get<int>();
                }
            }

            // Parse is_unsigned flag for numeric types (stored as is_signed in ColumnInfo)
            if (col.contains("is_unsigned")) {
                column_info.is_signed = !col["is_unsigned"].get<bool>();
            }

            // Parse ENUM/SET elements (stored as base64 in "elements" array)
            // MySQL 8.0 SDI stores enum elements as base64 encoded names
            if (col.contains("elements") && col["elements"].is_array()) {
                static const std::string base64_chars =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::vector<std::string>* target_values =
                    (column_info.mysql_type_id == 23) ? &column_info.set_values : &column_info.enum_values;
                for (const auto& elem : col["elements"]) {
                    if (elem.contains("name")) {
                        std::string encoded = elem["name"].get<std::string>();
                        // Simple base64 decode
                        std::string decoded;
                        std::vector<int> T(256, -1);
                        for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;
                        int val = 0, valb = -8;
                        for (unsigned char c : encoded) {
                            if (T[c] == -1) break;
                            val = (val << 6) + T[c];
                            valb += 6;
                            if (valb >= 0) {
                                decoded.push_back(char((val >> valb) & 0xFF));
                                valb -= 8;
                            }
                        }
                        target_values->push_back(decoded);
                    }
                }
                if (dbrecovery::innodb::g_verbose)                 std::cerr << "[DEBUG SDI] Column '" << column_info.name << "' has "
                          << target_values->size() << " elements (type="
                          << (column_info.mysql_type_id == 23 ? "SET" : "ENUM") << ")" << std::endl;
            }

            // Parse physical_pos and ordinal_position
            if (col.contains("physical_pos")) {
                column_info.physical_pos = col["physical_pos"].get<uint32_t>();
            }
            if (col.contains("ordinal_position")) {
                column_info.ordinal = col["ordinal_position"].get<uint32_t>();
            }
            if (column_info.physical_pos == 0 && col.contains("ordinal_position")) {
                column_info.physical_pos = col["ordinal_position"].get<uint32_t>();
            }

            // Skip system columns
            if (column_info.name == "DB_TRX_ID" ||
                column_info.name == "DB_ROLL_PTR" ||
                column_info.name == "DB_ROW_ID") {
                continue;
            }

            if (column_info.ordinal == 0) {
                column_info.ordinal = ordinal;
            }
            schema.columns[ordinal++] = column_info;
        }

        // Sort columns by physical_pos
        if (!schema.columns.empty()) {
            std::vector<std::pair<uint32_t, catalog::ColumnInfo>> cols_vec;
            for (const auto& p : schema.columns) {
                cols_vec.push_back(p);
            }
            std::sort(cols_vec.begin(), cols_vec.end(),
                [](const auto& a, const auto& b) {
                    return a.second.physical_pos < b.second.physical_pos;
                });
            schema.columns.clear();
            uint32_t new_ordinal = 1;
            for (const auto& p : cols_vec) {
                schema.columns[new_ordinal++] = p.second;
            }
        }
    }

    return schema;
}

std::optional<catalog::TableSchema> RawDiskCarver::extract_schema_from_sdi(
    const std::vector<uint8_t>& page_data) {

    if (page_data.size() < 100) return std::nullopt;

    const uint8_t* data = page_data.data();
    size_t page_size = page_data.size();

    // 检查是否是SDI页
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_SDI && page_type != FIL_PAGE_SDI_ZIP) {
        return std::nullopt;
    }

    std::cerr << "[INFO] Found SDI page (type=0x" << std::hex << page_type << std::dec << ")" << std::endl;

    // SDI页格式:
    // 对于 FIL_PAGE_SDI_ZIP (0x45BD):
    //   - FIL_HEADER (38 bytes)
    //   - BTR_PAGE_HEADER (36 bytes at offset 38)
    //   - Record area with SDI records
    //
    // SDI记录存储在INDEX页格式中，每条记录包含压缩的SDI JSON

    // 对于 FIL_PAGE_SDI (0x45BE):
    //   - FIL_HEADER (38 bytes)
    //   - SDI data directly

    size_t sdi_payload_start = FIL_HEADER_SIZE;
    size_t sdi_payload_len = page_size - FIL_HEADER_SIZE - 8;  // FIL_TRAILER = 8

    if (page_type == FIL_PAGE_SDI_ZIP) {
        // FIL_PAGE_SDI_ZIP 使用 INDEX 页格式存储 SDI 记录
        // 但 InnoDBIndexParser 可能无法正确解析（SDI 记录格式特殊）
        // 直接搜索 zlib 签名 (78 9c) 并解压

        std::cerr << "[INFO] SDI page is compressed (0x45BD), searching for zlib signatures" << std::endl;

        // 搜索 zlib 压缩数据签名 (78 9c 或 78 da 等)
        // SDI 记录存储在 INDEX 页格式的 record area 中
        // 从 offset 100 开始搜索（跳过 FIL header 和 BTR header）
        const size_t search_start = 100;
        const size_t search_end = page_size - 8;  // 跳过 FIL trailer

        for (size_t offset = search_start; offset + 2 < search_end; offset++) {
            // 检查 zlib 签名: 78 9c (default compression), 78 da (best compression), 78 01 (no compression)
            if (data[offset] == 0x78 && (data[offset + 1] == 0x9c || data[offset + 1] == 0xda ||
                                          data[offset + 1] == 0x01 || data[offset + 1] == 0x5e)) {

                if (dbrecovery::innodb::g_verbose) 
                std::cerr << "[DEBUG] Found zlib signature at offset " << offset << std::endl;

                // 直接解压 zlib 数据（不包含 SDI header）
                const uint8_t* zlib_data = data + offset;
                size_t max_zlib_len = search_end - offset;

                // 使用 raw deflate 解压
                std::vector<uint8_t> decompressed;
                z_stream strm = {};
                strm.next_in = const_cast<Bytef*>(zlib_data);
                strm.avail_in = static_cast<uInt>(max_zlib_len);

                // 初始缓冲区大小 64KB
                decompressed.resize(64 * 1024);
                strm.next_out = decompressed.data();
                strm.avail_out = static_cast<uInt>(decompressed.size());

                // 使用自动检测窗口大小 (32 + 15 = 47) 自动检测 zlib/gzip 头
                int ret = inflateInit2(&strm, 47);
                if (ret != Z_OK) {
                    if (dbrecovery::innodb::g_verbose)                     std::cerr << "[DEBUG] inflateInit2 failed: " << ret << std::endl;
                    continue;
                }

                bool decompress_ok = false;
                do {
                    ret = inflate(&strm, Z_NO_FLUSH);
                    if (ret == Z_STREAM_END) {
                        decompress_ok = true;
                        break;
                    }
                    if (ret == Z_BUF_ERROR) {
                        // 扩展缓冲区
                        size_t new_size = decompressed.size() * 2;
                        if (new_size > 16 * 1024 * 1024) break;  // 最大 16MB
                        decompressed.resize(new_size);
                        strm.next_out = decompressed.data() + strm.total_out;
                        strm.avail_out = static_cast<uInt>(new_size - strm.total_out);
                    } else if (ret != Z_OK) {
                        break;
                    }
                } while (strm.avail_in > 0);

                inflateEnd(&strm);

                if (!decompress_ok || strm.total_out == 0) {
                    if (dbrecovery::innodb::g_verbose)                     std::cerr << "[DEBUG] Decompression failed at offset " << offset << std::endl;
                    continue;
                }

                decompressed.resize(strm.total_out);
                if (dbrecovery::innodb::g_verbose)                 std::cerr << "[DEBUG] Decompressed " << strm.total_out << " bytes from zlib" << std::endl;

                // 解析 JSON
                try {
                    auto json_obj = nlohmann::json::parse(decompressed.begin(), decompressed.end());

                    catalog::TableSchema schema;
                    schema = parse_sdi_json_to_schema(json_obj);

                    if (!schema.columns.empty()) {
                        std::cerr << "[INFO] Successfully extracted schema from zlib compressed SDI: "
                                  << schema.columns.size() << " columns" << std::endl;
                        return schema;
                    }
                } catch (const std::exception& e) {
                    if (dbrecovery::innodb::g_verbose)                     std::cerr << "[DEBUG] JSON parse failed: " << e.what() << std::endl;
                    // 继续搜索下一个 zlib 签名
                }
            }
        }

        // 如果直接搜索失败，尝试使用 InnoDBIndexParser
        std::cerr << "[INFO] Direct zlib search failed, trying InnoDBIndexParser" << std::endl;

        innodb::InnoDBIndexParser parser;
        auto records = parser.parse_page(data, page_size, 0);

        std::cerr << "[INFO] Found " << records.size() << " records in SDI page" << std::endl;

        // 每条记录包含一个 SDI payload
        for (const auto& rec : records) {
            if (rec.fields.size() >= 1 && !rec.fields[0].raw_bytes.empty()) {
                const auto& raw_bytes = rec.fields[0].raw_bytes;

                if (dbrecovery::innodb::g_verbose) 
                std::cerr << "[DEBUG] SDI record field size: " << raw_bytes.size() << std::endl;

                auto schema = innodb::SDIManager::parse_sdi_record(raw_bytes.data(), raw_bytes.size());
                if (!schema.columns.empty()) {
                    std::cerr << "[INFO] Successfully extracted schema from SDI record: "
                              << schema.columns.size() << " columns" << std::endl;
                    return schema;
                }
            }
        }

        std::cerr << "[WARN] No valid SDI schema found in SDI page" << std::endl;
        return std::nullopt;
    }

    // 对于未压缩的 SDI 页，直接解析
    std::cerr << "[INFO] SDI page is uncompressed (0x45BE)" << std::endl;

    auto schema = innodb::SDIManager::parse_sdi_record(data + sdi_payload_start, sdi_payload_len);

    if (schema.columns.empty()) {
        std::cerr << "[WARN] No columns extracted from SDI" << std::endl;
        return std::nullopt;
    }

    std::cerr << "[INFO] Successfully extracted schema with " << schema.columns.size() << " columns from SDI" << std::endl;
    return schema;
}

} // namespace recovery
} // namespace dbrecovery
