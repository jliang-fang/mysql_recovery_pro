#include "dbrecovery/innodb/page_visualizer.hpp"
#include "dbrecovery/innodb/page_parser_v2.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>

namespace dbrecovery {
namespace innodb {

// ==================== 颜色定义 ====================
static const std::map<std::string, std::string> ANSI_COLORS = {
    {"red", "\033[31m"},
    {"green", "\033[32m"},
    {"yellow", "\033[33m"},
    {"blue", "\033[34m"},
    {"magenta", "\033[35m"},
    {"cyan", "\033[36m"},
    {"white", "\033[37m"},
    {"bold", "\033[1m"},
    {"reset", "\033[0m"}
};

// ==================== PageVisualizer 实现 ====================

PageVisualizer::PageVisualizer(const VisualizationOptions& options)
    : options_(options) {
}

void PageVisualizer::set_options(const VisualizationOptions& options) {
    options_ = options;
}

std::string PageVisualizer::visualize(const uint8_t* data, size_t size) {
    PageParserV2 parser(data, size);
    PageParseResult result = parser.parse();
    return visualize(result);
}

std::string PageVisualizer::visualize(const PageParseResult& result) {
    if (options_.output_format == "html") {
        return to_html(result);
    } else if (options_.output_format == "dot") {
        // DOT 格式需要页链
        return to_dot({});
    } else {
        return to_ascii(result);
    }
}

std::string PageVisualizer::visualize_chain(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages) {
    if (options_.output_format == "html") {
        return PageChainVisualizer::visualize_html(pages);
    } else if (options_.output_format == "dot") {
        return PageChainVisualizer::visualize_dot(pages);
    } else {
        return PageChainVisualizer::visualize(pages);
    }
}

std::string PageVisualizer::format_hex(const uint8_t* data, size_t size, size_t width) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    for (size_t i = 0; i < size; i += width) {
        size_t line_size = std::min(width, size - i);
        
        // 地址
        oss << std::setw(8) << i << "  ";
        
        // 十六进制
        for (size_t j = 0; j < width; j++) {
            if (j < line_size) {
                oss << std::setw(2) << static_cast<int>(data[i + j]) << " ";
            } else {
                oss << "   ";
            }
        }
        
        // ASCII
        oss << " |";
        for (size_t j = 0; j < line_size; j++) {
            uint8_t c = data[i + j];
            oss << (c >= 32 && c < 127 ? static_cast<char>(c) : '.');
        }
        oss << "|\n";
    }

    return oss.str();
}

std::string PageVisualizer::format_binary(const uint8_t* data, size_t size) {
    std::ostringstream oss;
    for (size_t i = 0; i < size; i++) {
        oss << ((data[i] & 0x80) ? '1' : '0')
            << ((data[i] & 0x40) ? '1' : '0')
            << ((data[i] & 0x20) ? '1' : '0')
            << ((data[i] & 0x10) ? '1' : '0')
            << ((data[i] & 0x08) ? '1' : '0')
            << ((data[i] & 0x04) ? '1' : '0')
            << ((data[i] & 0x02) ? '1' : '0')
            << ((data[i] & 0x01) ? '1' : '0');
    }
    return oss.str();
}

std::string PageVisualizer::color_text(const std::string& text, const std::string& color) {
    if (!options_.color_output) {
        return text;
    }
    auto it = ANSI_COLORS.find(color);
    if (it != ANSI_COLORS.end()) {
        return it->second + text + ANSI_COLORS.at("reset");
    }
    return text;
}

std::string PageVisualizer::get_color_for_page_type(uint16_t type) {
    switch (type) {
        case FIL_PAGE_INDEX: return "cyan";
        case FIL_PAGE_SDI: return "magenta";
        case FIL_PAGE_UNDO_LOG: return "yellow";
        case FIL_PAGE_TYPE_ALLOCATED: return "green";
        case FIL_PAGE_TYPE_BLOB: return "blue";
        default: return "white";
    }
}

std::string PageVisualizer::get_color_for_record_type(uint8_t info_bits) {
    uint8_t type = info_bits & 0x07;
    switch (type) {
        case REC_STATUS_INFIMUM: return "red";
        case REC_STATUS_SUPREMUM: return "red";
        default: return "white";
    }
}

std::string PageVisualizer::to_ascii(const PageParseResult& result) {
    std::ostringstream oss;

    // 页头
    if (options_.show_header) {
        oss << color_text("═══════════════════════════════════════════════════════════════════", "cyan") << "\n";
        oss << color_text("                         INNODB PAGE HEADER                          ", "cyan") << "\n";
        oss << color_text("═══════════════════════════════════════════════════════════════════", "cyan") << "\n\n";

        oss << "Page Type:     " << PageTypeDetector::page_type_to_string(result.page_type);
        oss << " (" << std::hex << "0x" << result.page_type << std::dec << ")\n";
        oss << "Page Level:    " << result.page_level << "\n";
        oss << "Page Offset:   " << result.page_offset << "\n";
        oss << "Prev Page:     " << result.prev_page << "\n";
        oss << "Next Page:     " << result.next_page << "\n";
        oss << "Row Format:    ";
        switch (result.row_format) {
            case RowFormat::REDUNDANT: oss << "REDUNDANT"; break;
            case RowFormat::COMPACT: oss << "COMPACT"; break;
            case RowFormat::DYNAMIC: oss << "DYNAMIC"; break;
            case RowFormat::COMPRESSED: oss << "COMPRESSED"; break;
            default: oss << "UNKNOWN";
        }
        oss << "\n";
        oss << "MySQL Version: " << static_cast<int>(result.version) << "\n";
        oss << "\n";
    }

    // 页目录
    if (options_.show_directory && !result.records.empty()) {
        oss << color_text("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━", "yellow") << "\n";
        oss << color_text("                         PAGE DIRECTORY                              ", "yellow") << "\n";
        oss << color_text("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━", "yellow") << "\n\n";

        size_t count = std::min(result.records.size(), options_.max_directory_slots);
        for (size_t i = 0; i < count; i++) {
            const auto& record = result.records[i];
            oss << "  Slot " << std::setw(3) << i << ": Offset " << std::setw(5) << record.offset
                << " | Heap " << std::setw(3) << record.heap_no;
            if (record.is_deleted) {
                oss << " " << color_text("[DEL]", "red");
            }
            oss << "\n";
        }
        if (result.records.size() > options_.max_directory_slots) {
            oss << "  ... (" << (result.records.size() - options_.max_directory_slots) << " more)\n";
        }
        oss << "\n";
    }

    // 记录
    if (options_.show_records && !result.records.empty()) {
        oss << color_text("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━", "cyan") << "\n";
        oss << color_text("                           RECORDS                                   ", "cyan") << "\n";
        oss << color_text("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━", "cyan") << "\n\n";

        size_t count = std::min(result.records.size(), options_.max_records);
        for (size_t i = 0; i < count; i++) {
            const auto& record = result.records[i];
            oss << "Record #" << (i + 1) << ":\n";
            oss << "  Offset:    " << record.offset << "\n";
            oss << "  Heap No:   " << record.heap_no << "\n";
            oss << "  Next Off:  " << record.next_offset << "\n";
            oss << "  Info Bits: 0x" << std::hex << std::setfill('0') << std::setw(2) 
                << static_cast<int>(record.info_bits) << std::dec;
            if (record.is_deleted) {
                oss << " " << color_text("(DELETED)", "red");
            }
            oss << "\n\n";
        }
        if (result.records.size() > options_.max_records) {
            oss << "  ... (" << (result.records.size() - options_.max_records) << " more records)\n\n";
        }
    }

    // 空闲空间
    if (options_.show_free_space && result.success) {
        oss << color_text("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━", "green") << "\n";
        oss << color_text("                         FREE SPACE                                  ", "green") << "\n";
        oss << color_text("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━", "green") << "\n\n";

        oss << "Checksum Valid:   " << (result.checksum_valid ? "YES" : "NO") << "\n";
        oss << "Checksum Stored:  " << result.checksum_stored << "\n";
        oss << "Checksum Computed:" << result.checksum_computed << "\n";
        oss << "\n";
    }

    return oss.str();
}

std::string PageVisualizer::to_html(const PageParseResult& result) {
    std::ostringstream oss;

    oss << "<!DOCTYPE html>\n";
    oss << "<html>\n<head>\n<title>InnoDB Page Visualization</title>\n";
    oss << "<style>\n";
    oss << "body { font-family: monospace; background: #1e1e1e; color: #d4d4d4; padding: 20px; }\n";
    oss << ".header { background: #007acc; color: white; padding: 10px; margin: 10px 0; }\n";
    oss << ".section { border: 1px solid #444; padding: 10px; margin: 10px 0; }\n";
    oss << ".section-title { background: #2d2d2d; padding: 5px; margin-bottom: 10px; }\n";
    oss << ".record { border: 1px solid #444; padding: 5px; margin: 5px 0; background: #252526; }\n";
    oss << ".deleted { color: #f44336; }\n";
    oss << ".info { display: grid; grid-template-columns: 150px 1fr; gap: 5px; }\n";
    oss << ".key { color: #4ec9b0; }\n";
    oss << ".value { color: #ce9178; }\n";
    oss << "</style>\n</head>\n<body>\n";

    oss << "<div class='header'><h1>InnoDB Page Visualization</h1></div>\n";

    // 页信息
    oss << "<div class='section'>\n";
    oss << "<div class='section-title'>Page Information</div>\n";
    oss << "<div class='info'>\n";
    oss << "<span class='key'>Page Type:</span><span class='value'>" << result.page_type << "</span>\n";
    oss << "<span class='key'>Page Level:</span><span class='value'>" << result.page_level << "</span>\n";
    oss << "<span class='key'>Page Offset:</span><span class='value'>" << result.page_offset << "</span>\n";
    oss << "<span class='key'>Prev Page:</span><span class='value'>" << result.prev_page << "</span>\n";
    oss << "<span class='key'>Next Page:</span><span class='value'>" << result.next_page << "</span>\n";
    oss << "<span class='key'>Row Format:</span><span class='value\">" << static_cast<int>(result.row_format) << "</span>\n";
    oss << "<span class='key'>MySQL Version:</span><span class='value'>" << static_cast<int>(result.version) << "</span>\n";
    oss << "</div>\n</div>\n";

    // 记录
    if (!result.records.empty()) {
        oss << "<div class='section'>\n";
        oss << "<div class='section-title'>Records (" << result.records.size() << " total)</div>\n";
        
        size_t count = std::min(result.records.size(), options_.max_records);
        for (size_t i = 0; i < count; i++) {
            const auto& record = result.records[i];
            oss << "<div class='record";
            if (record.is_deleted) oss << " deleted";
            oss << "'>\n";
            oss << "<strong>Record #" << (i + 1) << "</strong><br>\n";
            oss << "Offset: " << record.offset << " | ";
            oss << "Heap: " << record.heap_no << " | ";
            oss << "Next: " << record.next_offset << " | ";
            if (record.is_deleted) oss << "<span class='deleted'>DELETED</span>";
            oss << "</div>\n";
        }
        if (result.records.size() > options_.max_records) {
            oss << "<div class='record' style='color: #888;'>... " 
                << (result.records.size() - options_.max_records) << " more records</div>\n";
        }
        oss << "</div>\n";
    }

    oss << "</body>\n</html>\n";

    return oss.str();
}

std::string PageVisualizer::to_dot(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages) {
    std::ostringstream oss;

    oss << "digraph PageChain {\n";
    oss << "    rankdir=LR;\n";
    oss << "    node [shape=box];\n\n";

    for (size_t i = 0; i < pages.size(); i++) {
        const auto& [page_no, data] = pages[i];
        oss << "    page_" << page_no << " [label=\"Page " << page_no << "\"];\n";
    }

    for (size_t i = 0; i < pages.size() - 1; i++) {
        const auto& [page_no, data] = pages[i];
        const auto& [next_page_no, next_data] = pages[i + 1];
        oss << "    page_" << page_no << " -> page_" << next_page_no << ";\n";
    }

    oss << "}\n";

    return oss.str();
}

// ==================== PageStatisticsVisualizer 实现 ====================

std::string PageStatisticsVisualizer::visualize(const PageStatistics& stats) {
    std::ostringstream oss;

    oss << "═══════════════════════════════════════════════════════════════════\n";
    oss << "                        PAGE STATISTICS                             \n";
    oss << "═══════════════════════════════════════════════════════════════════\n\n";

    oss << "Total Pages:    " << stats.total_pages << "\n";
    oss << "Valid Pages:    " << stats.valid_pages << "\n";
    oss << "Invalid Pages:  " << stats.invalid_pages << "\n";
    oss << "Total Records:  " << stats.total_records << "\n";
    oss << "Total Bytes:    " << stats.total_bytes << "\n\n";

    oss << "Page Types:\n";
    for (const auto& [type, count] : stats.page_type_counts) {
        oss << "  " << PageTypeDetector::page_type_to_string(type) << ": " << count << "\n";
    }
    oss << "\n";

    oss << "MySQL Versions:\n";
    for (const auto& [version, count] : stats.version_counts) {
        oss << "  MySQL " << static_cast<int>(version) << ": " << count << "\n";
    }
    oss << "\n";

    oss << "Row Formats:\n";
    for (const auto& [format, count] : stats.format_counts) {
        oss << "  " << static_cast<int>(format) << ": " << count << "\n";
    }
    oss << "\n";

    return oss.str();
}

std::string PageStatisticsVisualizer::visualize_html(const PageStatistics& stats) {
    std::ostringstream oss;

    oss << "<div class='statistics'>\n";
    oss << "<h2>Page Statistics</h2>\n";
    oss << "<table>\n";
    oss << "<tr><th>Metric</th><th>Value</th></tr>\n";
    oss << "<tr><td>Total Pages</td><td>" << stats.total_pages << "</td></tr>\n";
    oss << "<tr><td>Valid Pages</td><td>" << stats.valid_pages << "</td></tr>\n";
    oss << "<tr><td>Invalid Pages</td><td>" << stats.invalid_pages << "</td></tr>\n";
    oss << "<tr><td>Total Records</td><td>" << stats.total_records << "</td></tr>\n";
    oss << "<tr><td>Total Bytes</td><td>" << stats.total_bytes << "</td></tr>\n";
    oss << "</table>\n";
    oss << "</div>\n";

    return oss.str();
}

std::string PageStatisticsVisualizer::visualize_markdown(const PageStatistics& stats) {
    std::ostringstream oss;

    oss << "## Page Statistics\n\n";
    oss << "| Metric | Value |\n";
    oss << "|--------|-------|\n";
    oss << "| Total Pages | " << stats.total_pages << " |\n";
    oss << "| Valid Pages | " << stats.valid_pages << " |\n";
    oss << "| Invalid Pages | " << stats.invalid_pages << " |\n";
    oss << "| Total Records | " << stats.total_records << " |\n";
    oss << "| Total Bytes | " << stats.total_bytes << " |\n";

    return oss.str();
}

// ==================== PageHealthVisualizer 实现 ====================

std::string PageHealthVisualizer::visualize(const PageHealthChecker::HealthInfo& info) {
    std::ostringstream oss;

    oss << "═══════════════════════════════════════════════════════════════════\n";
    oss << "                         PAGE HEALTH                                 \n";
    oss << "═══════════════════════════════════════════════════════════════════\n\n";

    oss << "Valid:          " << (info.is_valid ? "YES" : "NO") << "\n";
    oss << "Status:         " << info.status_message << "\n";
    oss << "Records:        " << info.n_records << "\n";
    oss << "Directory Slots:" << info.n_dir_slots << "\n";
    oss << "Page Level:     " << info.page_level << "\n";
    oss << "Checksum Match: " << (info.checksum_match ? "YES" : "NO") << "\n";
    if (info.checksum_match) {
        oss << "Checksum:       " << info.checksum_expected << "\n";
    }
    oss << "\n";

    return oss.str();
}

std::string PageHealthVisualizer::visualize_html(const PageHealthChecker::HealthInfo& info) {
    std::ostringstream oss;

    oss << "<div class='health' style='border: " << (info.is_valid ? "1px solid #4caf50" : "1px solid #f44336") 
        << "; padding: 10px;'>\n";
    oss << "<h3>Page Health: " << (info.is_valid ? "OK" : "ERROR") << "</h3>\n";
    oss << "<p>" << info.status_message << "</p>\n";
    oss << "</div>\n";

    return oss.str();
}

std::string PageHealthVisualizer::visualize_markdown(const PageHealthChecker::HealthInfo& info) {
    std::ostringstream oss;

    oss << "## Page Health\n\n";
    oss << "| Metric | Value |\n";
    oss << "|--------|-------|\n";
    oss << "| Valid | " << (info.is_valid ? "Yes" : "No") << " |\n";
    oss << "| Status | " << info.status_message << " |\n";
    oss << "| Records | " << info.n_records << " |\n";
    oss << "| Checksum Match | " << (info.checksum_match ? "Yes" : "No") << " |\n";

    return oss.str();
}

// ==================== PageTypeVisualizer 实现 ====================

std::string PageTypeVisualizer::visualize(uint16_t page_type) {
    std::ostringstream oss;

    oss << "Page Type: " << PageTypeDetector::page_type_to_string(page_type);
    oss << " (0x" << std::hex << std::setfill('0') << std::setw(4) << page_type << std::dec << ")\n";

    return oss.str();
}

std::string PageTypeVisualizer::visualize_html(uint16_t page_type) {
    std::ostringstream oss;

    oss << "<span class='page-type' style='color: " << get_color(page_type) << "'>"
        << PageTypeDetector::page_type_to_string(page_type) << "</span>";

    return oss.str();
}

std::string PageTypeVisualizer::get_color(uint16_t page_type) {
    switch (page_type) {
        case FIL_PAGE_INDEX: return "#00ffff";
        case FIL_PAGE_SDI: return "#ff00ff";
        case FIL_PAGE_UNDO_LOG: return "#ffff00";
        case FIL_PAGE_TYPE_ALLOCATED: return "#00ff00";
        case FIL_PAGE_TYPE_BLOB: return "#0000ff";
        default: return "#ffffff";
    }
}

// ==================== PageChainVisualizer 实现 ====================

std::string PageChainVisualizer::visualize(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages) {
    std::ostringstream oss;

    oss << "═══════════════════════════════════════════════════════════════════\n";
    oss << "                         PAGE CHAIN                                  \n";
    oss << "═══════════════════════════════════════════════════════════════════\n\n";

    for (size_t i = 0; i < pages.size(); i++) {
        const auto& [page_no, data] = pages[i];
        oss << "Page " << page_no;
        if (i > 0) {
            uint32_t prev = read_le<uint32_t>(data.data(), 24);
            oss << " (prev: " << prev << ")";
        }
        if (i < pages.size() - 1) {
            uint32_t next = read_le<uint32_t>(data.data(), 28);
            oss << " (next: " << next << ")";
        }
        oss << "\n";
    }

    return oss.str();
}

std::string PageChainVisualizer::visualize_html(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages) {
    std::ostringstream oss;

    oss << "<div class='page-chain'>\n";
    oss << "<h2>Page Chain (" << pages.size() << " pages)</h2>\n";
    oss << "<ul>\n";
    for (const auto& [page_no, data] : pages) {
        oss << "<li>Page " << page_no << "</li>\n";
    }
    oss << "</ul>\n";
    oss << "</div>\n";

    return oss.str();
}

std::string PageChainVisualizer::visualize_dot(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages) {
    std::ostringstream oss;

    oss << "digraph PageChain {\n";
    oss << "    rankdir=LR;\n";
    oss << "    node [shape=box style=filled fillcolor=lightblue];\n\n";

    for (size_t i = 0; i < pages.size(); i++) {
        const auto& [page_no, data] = pages[i];
        oss << "    page_" << page_no << " [label=\"Page " << page_no << "\"];\n";
    }

    for (size_t i = 0; i < pages.size() - 1; i++) {
        const auto& [page_no, data] = pages[i];
        const auto& [next_page_no, next_data] = pages[i + 1];
        oss << "    page_" << page_no << " -> page_" << next_page_no << ";\n";
    }

    oss << "}\n";

    return oss.str();
}

} // namespace innodb
} // namespace dbrecovery
