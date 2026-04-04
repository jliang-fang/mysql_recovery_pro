#ifndef DBRECOVERY_INNODB_PAGE_VISUALIZER_HPP
#define DBRECOVERY_INNODB_PAGE_VISUALIZER_HPP

/**
 * @file page_visualizer.hpp
 * @brief 页数据可视化功能
 *
 * 支持将页数据以多种方式可视化：
 * - ASCII 艺术图
 * - HTML 表格
 * - 终端彩色输出
 * - Graphviz DOT 格式
 */

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/innodb/page_exporter.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 页可视化选项 ====================
struct VisualizationOptions {
    bool show_header = true;              // 显示页头
    bool show_records = true;             // 显示记录
    bool show_directory = true;           // 显示页目录
    bool show_free_space = true;          // 显示空闲空间
    bool color_output = true;             // 使用颜色输出
    size_t max_records = 50;              // 最大显示记录数
    size_t max_directory_slots = 20;      // 最大显示目录槽
    std::string output_format = "ascii";  // ascii, html, dot
};

// ==================== 页可视化器 ====================
/**
 * @brief PageVisualizer: 页可视化器
 */
class PageVisualizer {
public:
    explicit PageVisualizer(const VisualizationOptions& options = VisualizationOptions());

    // 从页数据可视化
    std::string visualize(const uint8_t* data, size_t size);

    // 从解析结果可视化
    std::string visualize(const PageParseResult& result);

    // 从页链可视化
    std::string visualize_chain(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    // 设置可视化选项
    void set_options(const VisualizationOptions& options);

private:
    VisualizationOptions options_;

    // 辅助函数
    std::string format_hex(const uint8_t* data, size_t size, size_t width = 16);
    std::string format_binary(const uint8_t* data, size_t size);
    std::string color_text(const std::string& text, const std::string& color);
    std::string get_color_for_page_type(uint16_t type);
    std::string get_color_for_record_type(uint8_t info_bits);

    // 各种格式的可视化
    std::string to_ascii(const PageParseResult& result);
    std::string to_html(const PageParseResult& result);
    std::string to_dot(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    // 页头可视化
    std::string visualize_header(const uint8_t* data, size_t size);

    // 记录可视化
    std::string visualize_records(const std::vector<Record>& records);

    // 页目录可视化
    std::string visualize_directory(const uint8_t* data, size_t size, uint16_t n_dir_slots);

    // 空闲空间可视化
    std::string visualize_free_space(const uint8_t* data, size_t size);
};

// ==================== 页统计可视化 ====================
/**
 * @brief PageStatisticsVisualizer: 页统计可视化器
 */
class PageStatisticsVisualizer {
public:
    // 可视化页统计信息
    static std::string visualize(const PageStatistics& stats);

    // 可视化统计信息为 HTML
    static std::string visualize_html(const PageStatistics& stats);

    // 可视化统计信息为 Markdown
    static std::string visualize_markdown(const PageStatistics& stats);
};

// ==================== 页健康状态可视化 ====================
/**
 * @brief PageHealthVisualizer: 页健康状态可视化器
 */
class PageHealthVisualizer {
public:
    // 可视化健康状态
    static std::string visualize(const PageHealthChecker::HealthInfo& info);

    // 可视化健康状态为 HTML
    static std::string visualize_html(const PageHealthChecker::HealthInfo& info);

    // 可视化健康状态为 Markdown
    static std::string visualize_markdown(const PageHealthChecker::HealthInfo& info);
};

// ==================== 页类型可视化 ====================
/**
 * @brief PageTypeVisualizer: 页类型可视化器
 */
class PageTypeVisualizer {
public:
    // 可视化页类型
    static std::string visualize(uint16_t page_type);

    // 可视化页类型为 HTML
    static std::string visualize_html(uint16_t page_type);

    // 获取页类型颜色
    static std::string get_color(uint16_t page_type);
};

// ==================== 页链可视化 ====================
/**
 * @brief PageChainVisualizer: 页链可视化器
 */
class PageChainVisualizer {
public:
    // 可视化页链
    static std::string visualize(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    // 可视化页链为 HTML
    static std::string visualize_html(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    // 可视化页链为 DOT (Graphviz)
    static std::string visualize_dot(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_VISUALIZER_HPP
