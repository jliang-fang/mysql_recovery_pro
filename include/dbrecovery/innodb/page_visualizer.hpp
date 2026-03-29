#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <sstream>
#include <iomanip>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief PageVisualizer: 页级数据可视化工具
 *
 * 提供以下能力：
 * - 页头信息可视化
 * - 记录信息可视化
 * - 目录槽可视化
 * - B+Tree 结构可视化
 */
class PageVisualizer {
public:
    explicit PageVisualizer(const std::vector<uint8_t>& page_data);
    ~PageVisualizer();

    /**
     * @brief 设置页数据
     *
     * @param page_data 页数据
     */
    void set_page(const std::vector<uint8_t>& page_data);

    /**
     * @brief 获取页头信息
     *
     * @return std::string 页头信息字符串
     */
    std::string visualize_header() const;

    /**
     * @brief 获取记录信息
     *
     * @return std::string 记录信息字符串
     */
    std::string visualize_records() const;

    /**
     * @brief 获取目录槽信息
     *
     * @return std::string 目录槽信息字符串
     */
    std::string visualize_directory() const;

    /**
     * @brief 获取 B+Tree 信息
     *
     * @return std::string B+Tree 信息字符串
     */
    std::string visualize_btree() const;

    /**
     * @brief 获取完整页信息
     *
     * @return std::string 完整页信息字符串
     */
    std::string visualize_full() const;

    /**
     * @brief 获取页数据的十六进制转储
     *
     * @param offset 偏移
     * @param length 长度
     * @return std::string 十六进制转储字符串
     */
    std::string hex_dump(size_t offset = 0, size_t length = 256) const;

    /**
     * @brief 获取页健康状态
     *
     * @return std::string 健康状态字符串
     */
    std::string visualize_health() const;

private:
    std::vector<uint8_t> page_data_;
    bool parsed_ = false;
    
    // 解析结果
    uint16_t page_type_ = 0;
    uint32_t page_level_ = 0;
    uint16_t n_dir_slots_ = 0;
    uint16_t heap_top_ = 0;
    uint16_t n_heap_ = 0;
    uint32_t next_page_ = 0xFFFFFFFF;
    uint32_t prev_page_ = 0xFFFFFFFF;
    std::vector<uint16_t> record_offsets_;
    std::vector<uint16_t> dir_slots_;
};

} // namespace innodb
} // namespace dbrecovery
