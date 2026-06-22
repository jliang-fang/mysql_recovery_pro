#include "dbrecovery/recovery/raw_disk_carver.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/frm_parser.hpp"

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

bool RawDiskCarver::is_valid_page(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 64) return false;

    // InnoDB页验证策略:
    // 1. 检查页类型是否有效 (offset 24-25)
    // 2. 对于INDEX页，检查额外的结构完整性
    //
    // 注意：LSN检查在真实文件中可能不严格匹配
    // ibd2sql使用: data[:4] + data[4:8] == data[-8:-4] + data[-4:]
    // 但实际文件可能LSN不完全匹配（如恢复后的文件、压缩页等）

    try {
        // 检查页类型 (offset 24-25)
        uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);

        // 有效页类型列表 (参考 ibd2sql)
        bool is_valid_type = false;
        switch (page_type) {
            case FIL_PAGE_INDEX:        // 0x45BF - INDEX页
            case FIL_PAGE_SDI:          // 0x45BE - SDI页
            case FIL_PAGE_SDI_ZIP:      // 0x45BD - 压缩SDI页/BLOB页
            case FIL_PAGE_UNDO_LOG:     // 0x45B2 - Undo日志页
            case FIL_PAGE_COMPRESSED:   // 0x45B5 - 压缩页
            case FIL_PAGE_TYPE_ALLOCATED: // 0x0000 - 已分配
            case 0x0003:  // FIL_PAGE_INODE
            case 0x0005:  // FIL_PAGE_IBUF_BITMAP
            case 0x0008:  // FIL_PAGE_FSP_HEADER
                is_valid_type = true;
                break;
            default:
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
 * 参考 ibd2sql IS_FRM: data[:10] == b'\xfe\x01\n\x0c\x03\x00\x00\x10\x01\x00'
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
 * page_no在offset 8-12 (4字节大端序) - 这是FIL_PAGE_OFFSET
 */
uint32_t RawDiskCarver::extract_page_no(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 12) return 0;
    return read_be<uint32_t>(data, 8);
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

    // 获取设备大小
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
    }
#else
    // 非Linux系统，使用文件系统获取大小
    try {
        device_size = std::filesystem::file_size(config_.device_path);
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

    auto end_time = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
    result.success = true;

    if (config_.verbose) {
        std::cout << "[INFO] Scan completed:\n";
        std::cout << "  Pages scanned: " << result.total_pages_scanned << "\n";
        std::cout << "  Valid pages: " << result.valid_pages_found << "\n";
        std::cout << "  INDEX pages: " << result.index_pages_found << "\n";
        std::cout << "  Unique index_ids: " << result.pages_by_index_id.size() << "\n";
        std::cout << "  Time: " << result.elapsed_seconds << "s\n";
    }

    last_scan_result_ = result;
    return result;
}

void RawDiskCarver::worker_thread(uint32_t worker_id,
                                   std::function<void(double, const std::string&)> progress) {
    std::ifstream device(config_.device_path, std::ios::binary);
    if (!device) {
        if (config_.verbose) {
            std::cerr << "[Worker " << worker_id << "] Cannot open device\n";
        }
        return;
    }

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

        // 读取数据
        device.seekg(chunk_start, std::ios::beg);
        device.read(reinterpret_cast<char*>(buffer.data()), chunk_size);

        if (!device || device.gcount() == 0) {
            break;
        }

        size_t bytes_read = device.gcount();
        pages_in_chunk = 0;

        // 扫描页
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
                continue;
            }

            if (!is_valid_page(page_data, config_.page_size)) {
                continue;
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

            if (is_index_page(page_data, config_.page_size)) {
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

        // 对每个index_id的页进行解析
        std::vector<std::map<std::string, std::string>> all_records;

        for (const auto& page_data : pages) {
            auto records = parse_records_schemaless(page_data);
            all_records.insert(all_records.end(), records.begin(), records.end());
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
                    // 十六进制输出
                    std::ostringstream hex;
                    for (uint8_t b : rec.fields[i].raw_bytes) {
                        hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
                    }
                    record[field_name] = hex.str();
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
    // 参考: ibd2sql get_all_rows_fast 方法
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

std::vector<std::pair<size_t, size_t>> RawDiskCarver::detect_field_boundaries(
    const uint8_t* record_start, size_t max_len) {
    // 启发式字段边界检测
    // 参考: ibd2sql的schema-less解析

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

    // SDI页格式:
    // - FIL_HEADER (24 bytes)
    // - SDI数据 (压缩或未压缩)
    // - FIL_TRAILER (8 bytes)

    // 对于FIL_PAGE_SDI (0x45BE): 数据未压缩
    // 对于FIL_PAGE_SDI_ZIP (0x45BD): 数据使用zlib压缩

    constexpr size_t FIL_TRAILER_SIZE = 8;

    size_t sdi_offset = FIL_HEADER_SIZE;  // 38 bytes (FIL_PAGE_DATA)
    size_t sdi_len = page_size - FIL_HEADER_SIZE - FIL_TRAILER_SIZE;

    if (page_type == FIL_PAGE_SDI_ZIP) {
        // 需要解压缩
        // 参考 ibd2sql innodb_page/page.py
        // 压缩数据从offset 38开始

        // 使用zlib解压 (简化实现)
        // 实际需要完整实现zlib解压
        std::cerr << "[INFO] SDI page is compressed (0x45BD), attempting decompression" << std::endl;

        // TODO: 实现zlib解压缩
        return std::nullopt;
    }

    // 未压缩的SDI数据
    // SDI格式是JSON，需要解析JSON来获取schema

    try {
        // 跳过SDI header (参考 ibd2sql)
        // SDI header structure:
        // - type (1 byte)
        // - compressed_len (4 bytes LE)
        // - uncompressed_len (4 bytes LE)

        if (sdi_offset + 9 > page_size) return std::nullopt;

        uint8_t sdi_type = data[sdi_offset];
        uint32_t compressed_len = read_le<uint32_t>(data, sdi_offset + 1);
        uint32_t uncompressed_len = read_le<uint32_t>(data, sdi_offset + 5);

        size_t json_start = sdi_offset + 9;
        size_t json_len = uncompressed_len;

        if (json_start + json_len > page_size) {
            json_len = page_size - json_start;
        }

        // 解析JSON数据
        std::string json_str(reinterpret_cast<const char*>(data + json_start), json_len);

        // 使用nlohmann::json解析 (如果可用)
        // 简化实现：返回空schema
        // 实际应该解析JSON提取列信息

        std::cerr << "[INFO] SDI JSON length: " << json_len << " bytes" << std::endl;

        // TODO: 完整的JSON解析和schema提取
        return std::nullopt;

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to extract schema from SDI: " << e.what() << std::endl;
        return std::nullopt;
    }
}

} // namespace recovery
} // namespace dbrecovery
