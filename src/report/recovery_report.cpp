/**
 * @file recovery_report.cpp
 * @brief RecoveryReport and ReportBuilder implementations
 *
 * Extracted from recovery_report.hpp for better compilation boundaries.
 */

#include "dbrecovery/report/recovery_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace dbrecovery {
namespace report {

// ==================== RecoveryReport (static methods) ====================

std::string RecoveryReport::generate_report_id() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << "RECOVERY-" << std::put_time(std::gmtime(&time_t_now), "%Y%m%d-%H%M%S")
        << "-" << std::hex << (reinterpret_cast<uint64_t>(&now) & 0xFFFF);
    return oss.str();
}

std::string RecoveryReport::format_time(const std::chrono::system_clock::time_point& tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// ==================== RecoveryReport::to_json ====================

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(static_cast<unsigned char>(c) >> 4) & 0x0f];
                    out += hex[static_cast<unsigned char>(c) & 0x0f];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string CapabilityReport::to_json() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"mysql_version\": \"" << json_escape(mysql_version) << "\",\n";
    oss << "  \"features\": [";
    for (size_t i = 0; i < features.size(); ++i) {
        const auto& feature = features[i];
        if (i > 0) oss << ",";
        oss << "\n    {\"id\": \"" << json_escape(feature.id)
            << "\", \"name\": \"" << json_escape(feature.name)
            << "\", \"status\": \"" << ::dbrecovery::report::to_string(feature.status)
            << "\", \"output_category\": \""
            << ::dbrecovery::report::to_string(feature.output_category)
            << "\", \"limitation\": \"" << json_escape(feature.limitation)
            << "\", \"evidence\": [";
        for (size_t j = 0; j < feature.evidence.size(); ++j) {
            if (j > 0) oss << ", ";
            oss << "\"" << json_escape(feature.evidence[j]) << "\"";
        }
        oss << "]}";
    }
    oss << "\n  ],\n  \"warnings\": [";
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "\"" << json_escape(warnings[i]) << "\"";
    }
    oss << "]\n}\n";
    return oss.str();
}

static std::string html_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

static std::string format_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024 && unit_idx < 4) {
        size /= 1024;
        unit_idx++;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit_idx];
    return oss.str();
}

static std::string fmt_num(uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(i, ",");
    }
    return s;
}

std::string RecoveryReport::to_json() const {
    nlohmann::json report = nlohmann::json::object();
    if (result_contract.has_value()) {
        report = tools::result_contract_object(*result_contract);
    }

    report["report_id"] = report_id;
    report["report_time"] = report_time;
    report["recovery_type"] = recovery_type;
    report["mysql_version"] = mysql_version;
    report["overall_status"] = overall_status;
    report["input"] = {
        {"source_type", input.source_type},
        {"source_path", input.source_path},
        {"source_size_bytes", input.source_size_bytes},
        {"has_schema", input.has_schema},
        {"schema_source", input.schema_source},
        {"has_undo_logs", input.has_undo_logs},
        {"has_redo_logs", input.has_redo_logs},
        {"files_processed", input.files_processed}
    };
    report["strategy"] = {
        {"primary", strategy.primary_strategy},
        {"attempted", strategy.strategies_attempted},
        {"fallbacks", strategy.fallback_strategies},
        {"parameters", strategy.parameters}
    };
    report["data"] = {
        {"total_pages_scanned", data.total_pages_scanned},
        {"index_pages_found", data.index_pages_found},
        {"blob_pages_found", data.blob_pages_found},
        {"corrupted_pages", data.corrupted_pages},
        {"skipped_pages", data.skipped_pages},
        {"total_records_attempted", data.total_records_attempted},
        {"records_fully_recovered", data.records_fully_recovered},
        {"records_partially_recovered", data.records_partially_recovered},
        {"records_failed", data.records_failed},
        {"recovery_rate_percent", data.recovery_rate_percent},
        {"blob_records_recovered", data.blob_records_recovered},
        {"history_versions_recovered", data.history_versions_recovered},
        {"flashback_statements_generated", data.flashback_statements_generated},
        {"records_by_type", data.records_by_type}
    };
    report["damage"] = {
        {"pages_with_header_damage", damage.pages_with_header_damage},
        {"pages_with_checksum_mismatch", damage.pages_with_checksum_mismatch},
        {"pages_with_record_corruption", damage.pages_with_record_corruption},
        {"pages_with_blob_corruption", damage.pages_with_blob_corruption},
        {"details", nlohmann::json::array()}
    };
    for (const auto& detail : damage.details) {
        report["damage"]["details"].push_back({
            {"page_no", detail.page_no},
            {"type", detail.damage_type},
            {"description", detail.description}
        });
    }
    report["failures"] = { {"categories", nlohmann::json::array()} };
    for (const auto& category : failures.categories) {
        report["failures"]["categories"].push_back({
            {"category", category.category},
            {"count", category.count},
            {"percentage", category.percentage},
            {"recommendation", category.recommendation}
        });
    }
    report["reconstructed_ddl"] = reconstructed_ddl;
    report["output"] = {
        {"directory", output.output_directory},
        {"files", output.output_files},
        {"total_size_bytes", output.total_output_size_bytes},
        {"format", output.format}
    };
    report["timing"] = {
        {"scan_seconds", timing.scan_time_seconds},
        {"parse_seconds", timing.parse_time_seconds},
        {"export_seconds", timing.export_time_seconds},
        {"total_seconds", timing.total_time_seconds}
    };
    return report.dump(2) + "\n";
}


std::string RecoveryReport::to_text() const {
    std::ostringstream oss;
    oss << "================================================================================\n";
    oss << "                    MySQL Recovery Pro v2.0 — Recovery Report                   \n";
    oss << "================================================================================\n\n";

    oss << "Report ID:     " << report_id << "\n";
    oss << "Report Time:   " << report_time << "\n";
    oss << "Recovery Type: " << recovery_type << "\n";
    oss << "MySQL Version: " << mysql_version << "\n";
    oss << "Status:        " << overall_status << "\n\n";

    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "INPUT\n";
    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "  Source:       " << input.source_type << " (" << input.source_path << ")\n";
    oss << "  Size:         " << format_bytes(input.source_size_bytes) << "\n";
    oss << "  Schema:       " << input.schema_source << "\n";
    oss << "  Undo Logs:    " << (input.has_undo_logs ? "Yes" : "No") << "\n";
    oss << "  Redo Logs:    " << (input.has_redo_logs ? "Yes" : "No") << "\n";
    oss << "  Files:        " << input.files_processed.size() << " processed\n\n";

    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "STRATEGY\n";
    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "  Primary:      " << strategy.primary_strategy << "\n";
    if (!strategy.fallback_strategies.empty()) {
        oss << "  Fallbacks:    ";
        for (size_t i = 0; i < strategy.fallback_strategies.size(); i++) {
            if (i > 0) oss << ", ";
            oss << strategy.fallback_strategies[i];
        }
        oss << "\n";
    }
    for (const auto& [k, v] : strategy.parameters) {
        oss << "  " << k << ": " << v << "\n";
    }
    oss << "\n";

    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "DATA STATISTICS\n";
    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "  Pages scanned:           " << fmt_num(data.total_pages_scanned) << "\n";
    oss << "  Index pages:             " << fmt_num(data.index_pages_found) << "\n";
    oss << "  Blob pages:              " << fmt_num(data.blob_pages_found) << "\n";
    oss << "  Corrupted pages:         " << fmt_num(data.corrupted_pages) << "\n";
    oss << "  Skipped pages:           " << fmt_num(data.skipped_pages) << "\n";
    oss << "  Records attempted:       " << fmt_num(data.total_records_attempted) << "\n";
    oss << "  Fully recovered:         " << fmt_num(data.records_fully_recovered) << "\n";
    oss << "  Partially recovered:     " << fmt_num(data.records_partially_recovered) << "\n";
    oss << "  Failed:                  " << fmt_num(data.records_failed) << "\n";
    oss << "  Recovery rate:           " << std::fixed << std::setprecision(2)
        << data.recovery_rate_percent << "%\n";
    oss << "  BLOB records:            " << fmt_num(data.blob_records_recovered) << "\n";
    oss << "  History versions:        " << fmt_num(data.history_versions_recovered) << "\n";
    oss << "  Flashback statements:    " << fmt_num(data.flashback_statements_generated) << "\n\n";

    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "DAMAGE ANALYSIS\n";
    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "  Header damage:     " << damage.pages_with_header_damage << " pages\n";
    oss << "  Checksum mismatch: " << damage.pages_with_checksum_mismatch << " pages\n";
    oss << "  Record corruption: " << damage.pages_with_record_corruption << " pages\n";
    oss << "  BLOB corruption:   " << damage.pages_with_blob_corruption << " pages\n";
    for (const auto& d : damage.details) {
        oss << "  - Page " << d.page_no << ": " << d.damage_type << " — " << d.description << "\n";
    }
    oss << "\n";

    if (!failures.categories.empty()) {
        oss << "────────────────────────────────────────────────────────────────────────────────\n";
        oss << "FAILURE ANALYSIS\n";
        oss << "────────────────────────────────────────────────────────────────────────────────\n";
        for (const auto& fc : failures.categories) {
            oss << "  " << fc.category << ": " << fc.count << " ("
                << std::fixed << std::setprecision(1) << fc.percentage << "%)\n";
            oss << "    → " << fc.recommendation << "\n";
        }
        oss << "\n";
    }

    if (!reconstructed_ddl.empty()) {
        oss << "────────────────────────────────────────────────────────────────────────────────\n";
        oss << "RECONSTRUCTED DDL\n";
        oss << "────────────────────────────────────────────────────────────────────────────────\n";
        for (const auto& ddl : reconstructed_ddl) {
            oss << ddl << "\n";
        }
        oss << "\n";
    }

    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "OUTPUT\n";
    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "  Directory: " << output.output_directory << "\n";
    oss << "  Format:    " << output.format << "\n";
    oss << "  Size:      " << format_bytes(output.total_output_size_bytes) << "\n";
    oss << "  Files:     " << output.output_files.size() << "\n";
    for (const auto& f : output.output_files) {
        oss << "    - " << f << "\n";
    }
    oss << "\n";

    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "TIMING\n";
    oss << "────────────────────────────────────────────────────────────────────────────────\n";
    oss << "  Scan:   " << std::fixed << std::setprecision(3) << timing.scan_time_seconds << "s\n";
    oss << "  Parse:  " << timing.parse_time_seconds << "s\n";
    oss << "  Export: " << timing.export_time_seconds << "s\n";
    oss << "  Total:  " << timing.total_time_seconds << "s\n";

    oss << "\n================================================================================\n";
    oss << "                        Report generated by MySQL Recovery Pro v2.0              \n";
    oss << "================================================================================\n";

    return oss.str();
}

std::string RecoveryReport::to_html() const {
    std::ostringstream oss;
    oss << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
    oss << "<meta charset=\"UTF-8\">\n";
    oss << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    oss << "<title>MySQL Recovery Report — " << html_escape(report_id) << "</title>\n";
    oss << "<style>\n";
    oss << "  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; ";
    oss << "margin: 0; padding: 20px; background: #f5f5f5; color: #333; }\n";
    oss << "  .container { max-width: 900px; margin: 0 auto; background: white; ";
    oss << "border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1); padding: 30px; }\n";
    oss << "  h1 { color: #1a73e8; border-bottom: 2px solid #1a73e8; padding-bottom: 10px; }\n";
    oss << "  h2 { color: #333; margin-top: 25px; border-bottom: 1px solid #ddd; padding-bottom: 5px; }\n";
    oss << "  .status { display: inline-block; padding: 4px 12px; border-radius: 4px; ";
    oss << "font-weight: bold; font-size: 14px; }\n";
    oss << "  .status-success { background: #e6f4ea; color: #137333; }\n";
    oss << "  .status-partial { background: #fef7e0; color: #b06000; }\n";
    oss << "  .status-failed { background: #fce8e6; color: #c5221f; }\n";
    oss << "  table { width: 100%; border-collapse: collapse; margin: 10px 0; }\n";
    oss << "  th, td { padding: 8px 12px; text-align: left; border-bottom: 1px solid #eee; }\n";
    oss << "  th { background: #f8f9fa; font-weight: 600; }\n";
    oss << "  .metric-value { font-weight: bold; font-size: 18px; color: #1a73e8; }\n";
    oss << "  .metric-label { font-size: 12px; color: #666; }\n";
    oss << "  .grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 15px; }\n";
    oss << "  .card { background: #f8f9fa; border-radius: 6px; padding: 15px; text-align: center; }\n";
    oss << "  .ddl-box { background: #1e1e1e; color: #d4d4d4; padding: 15px; border-radius: 6px; ";
    oss << "font-family: 'Courier New', monospace; font-size: 13px; overflow-x: auto; }\n";
    oss << "  pre { background: #f8f9fa; padding: 10px; border-radius: 4px; overflow-x: auto; }\n";
    oss << "</style>\n</head>\n<body>\n<div class=\"container\">\n";

    oss << "<h1>MySQL Recovery Pro v2.0 — Recovery Report</h1>\n";
    oss << "<p><strong>Report ID:</strong> " << html_escape(report_id) << "<br>\n";
    oss << "<strong>Time:</strong> " << html_escape(report_time) << "<br>\n";
    oss << "<strong>Type:</strong> " << html_escape(recovery_type) << " | ";
    oss << "<strong>MySQL:</strong> " << html_escape(mysql_version) << "<br>\n";
    oss << "<strong>Status:</strong> <span class=\"status status-";
    if (overall_status == "SUCCESS") oss << "success";
    else if (overall_status == "PARTIAL") oss << "partial";
    else oss << "failed";
    oss << "\">" << html_escape(overall_status) << "</span></p>\n";

    oss << "<h2>Key Metrics</h2>\n<div class=\"grid\">\n";
    oss << "  <div class=\"card\"><div class=\"metric-value\">" << std::fixed << std::setprecision(1)
        << data.recovery_rate_percent << "%</div><div class=\"metric-label\">Recovery Rate</div></div>\n";
    oss << "  <div class=\"card\"><div class=\"metric-value\">" << fmt_num(data.records_fully_recovered)
        << "</div><div class=\"metric-label\">Records Recovered</div></div>\n";
    oss << "  <div class=\"card\"><div class=\"metric-value\">" << std::fixed << std::setprecision(1)
        << timing.total_time_seconds << "s</div><div class=\"metric-label\">Total Time</div></div>\n";
    oss << "  <div class=\"card\"><div class=\"metric-value\">" << fmt_num(data.total_pages_scanned)
        << "</div><div class=\"metric-label\">Pages Scanned</div></div>\n";
    oss << "  <div class=\"card\"><div class=\"metric-value\">" << damage.pages_with_header_damage
        << "</div><div class=\"metric-label\">Damaged Pages</div></div>\n";
    oss << "  <div class=\"card\"><div class=\"metric-value\">" << data.blob_records_recovered
        << "</div><div class=\"metric-label\">BLOB Records</div></div>\n";
    oss << "</div>\n";

    oss << "<h2>Input</h2>\n<table>\n";
    oss << "<tr><th>Source Type</th><td>" << html_escape(input.source_type) << "</td></tr>\n";
    oss << "<tr><th>Source Path</th><td><code>" << html_escape(input.source_path) << "</code></td></tr>\n";
    oss << "<tr><th>Size</th><td>" << format_bytes(input.source_size_bytes) << "</td></tr>\n";
    oss << "<tr><th>Schema</th><td>" << html_escape(input.schema_source) << "</td></tr>\n";
    oss << "<tr><th>Undo Logs</th><td>" << (input.has_undo_logs ? "✓ Available" : "✗ Not available") << "</td></tr>\n";
    oss << "<tr><th>Redo Logs</th><td>" << (input.has_redo_logs ? "✓ Available" : "✗ Not available") << "</td></tr>\n";
    oss << "</table>\n";

    oss << "<h2>Data Statistics</h2>\n<table>\n";
    oss << "<tr><td>Total Records Attempted</td><td><strong>" << fmt_num(data.total_records_attempted) << "</strong></td></tr>\n";
    oss << "<tr><td>Fully Recovered</td><td style=\"color:green\"><strong>" << fmt_num(data.records_fully_recovered) << "</strong></td></tr>\n";
    oss << "<tr><td>Partially Recovered</td><td style=\"color:orange\"><strong>" << fmt_num(data.records_partially_recovered) << "</strong></td></tr>\n";
    oss << "<tr><td>Failed</td><td style=\"color:red\"><strong>" << fmt_num(data.records_failed) << "</strong></td></tr>\n";
    oss << "<tr><td>Recovery Rate</td><td><strong>" << std::fixed << std::setprecision(2)
        << data.recovery_rate_percent << "%</strong></td></tr>\n";
    oss << "</table>\n";

    oss << "<h2>Recovery Strategy</h2>\n";
    oss << "<p><strong>Primary:</strong> " << html_escape(strategy.primary_strategy) << "</p>\n";
    if (!strategy.fallback_strategies.empty()) {
        oss << "<p><strong>Fallbacks:</strong> ";
        for (size_t i = 0; i < strategy.fallback_strategies.size(); i++) {
            if (i > 0) oss << " → ";
            oss << html_escape(strategy.fallback_strategies[i]);
        }
        oss << "</p>\n";
    }

    if (damage.pages_with_header_damage > 0 || damage.pages_with_checksum_mismatch > 0) {
        oss << "<h2>Damage Analysis</h2>\n<table>\n";
        oss << "<tr><th>Type</th><th>Pages</th></tr>\n";
        oss << "<tr><td>Header Damage</td><td>" << damage.pages_with_header_damage << "</td></tr>\n";
        oss << "<tr><td>Checksum Mismatch</td><td>" << damage.pages_with_checksum_mismatch << "</td></tr>\n";
        oss << "<tr><td>Record Corruption</td><td>" << damage.pages_with_record_corruption << "</td></tr>\n";
        oss << "<tr><td>BLOB Corruption</td><td>" << damage.pages_with_blob_corruption << "</td></tr>\n";
        oss << "</table>\n";
    }

    if (!reconstructed_ddl.empty()) {
        oss << "<h2>Reconstructed DDL</h2>\n";
        for (const auto& ddl : reconstructed_ddl) {
            oss << "<div class=\"ddl-box\">" << html_escape(ddl) << "</div>\n";
        }
    }

    oss << "<h2>Output</h2>\n<table>\n";
    oss << "<tr><th>Directory</th><td><code>" << html_escape(output.output_directory) << "</code></td></tr>\n";
    oss << "<tr><th>Format</th><td>" << html_escape(output.format) << "</td></tr>\n";
    oss << "<tr><th>Size</th><td>" << format_bytes(output.total_output_size_bytes) << "</td></tr>\n";
    oss << "<tr><th>Files</th><td>" << output.output_files.size() << "</td></tr>\n";
    oss << "</table>\n";

    oss << "<p style=\"text-align:center;color:#999;margin-top:30px;\">";
    oss << "Generated by MySQL Recovery Pro v2.0</p>\n";
    oss << "</div>\n</body>\n</html>\n";
    return oss.str();
}

std::vector<std::string> RecoveryReport::write_files(
    const std::string& base_path, const std::string& formats) const {
    std::vector<std::string> written;
    std::string fmt_lower = formats;
    for (auto& c : fmt_lower) c = std::tolower(c);

    if (fmt_lower.find("json") != std::string::npos) {
        std::string path = base_path + ".json";
        std::ofstream f(path);
        f << to_json();
        written.push_back(path);
    }
    if (fmt_lower.find("text") != std::string::npos) {
        std::string path = base_path + ".txt";
        std::ofstream f(path);
        f << to_text();
        written.push_back(path);
    }
    if (fmt_lower.find("html") != std::string::npos) {
        std::string path = base_path + ".html";
        std::ofstream f(path);
        f << to_html();
        written.push_back(path);
    }
    return written;
}

// ==================== ReportBuilder ====================

ReportBuilder::ReportBuilder() {
    report_.report_id = RecoveryReport::generate_report_id();
    report_.report_time = RecoveryReport::format_time(std::chrono::system_clock::now());
}

ReportBuilder& ReportBuilder::set_recovery_type(const std::string& type) {
    report_.recovery_type = type;
    return *this;
}

ReportBuilder& ReportBuilder::set_mysql_version(const std::string& ver) {
    report_.mysql_version = ver;
    return *this;
}

ReportBuilder& ReportBuilder::set_status(const std::string& status) {
    report_.overall_status = status;
    return *this;
}

ReportBuilder& ReportBuilder::set_input(const InputInfo& input) {
    report_.input = input;
    return *this;
}

ReportBuilder& ReportBuilder::set_strategy(const StrategyInfo& strategy) {
    report_.strategy = strategy;
    return *this;
}

ReportBuilder& ReportBuilder::set_data(const DataStats& data) {
    report_.data = data;
    return *this;
}

ReportBuilder& ReportBuilder::add_damage(const DamageDetail& detail) {
    report_.damage.details.push_back(detail);
    if (detail.damage_type.find("header") != std::string::npos)
        report_.damage.pages_with_header_damage++;
    if (detail.damage_type.find("checksum") != std::string::npos)
        report_.damage.pages_with_checksum_mismatch++;
    if (detail.damage_type.find("record") != std::string::npos)
        report_.damage.pages_with_record_corruption++;
    if (detail.damage_type.find("blob") != std::string::npos)
        report_.damage.pages_with_blob_corruption++;
    return *this;
}

ReportBuilder& ReportBuilder::add_failure(const std::string& category, uint64_t count,
                                          double pct, const std::string& recommendation) {
    FailureCategory fc;
    fc.category = category;
    fc.count = count;
    fc.percentage = pct;
    fc.recommendation = recommendation;
    report_.failures.categories.push_back(fc);
    return *this;
}

ReportBuilder& ReportBuilder::add_ddl(const std::string& ddl) {
    report_.reconstructed_ddl.push_back(ddl);
    return *this;
}

ReportBuilder& ReportBuilder::set_output(const OutputInfo& output) {
    report_.output = output;
    return *this;
}

ReportBuilder& ReportBuilder::set_timing(const TimingInfo& timing) {
    report_.timing = timing;
    return *this;
}

RecoveryReport ReportBuilder::build() {
    if (report_.data.recovery_rate_percent == 0.0 &&
        report_.data.total_records_attempted > 0) {
        report_.data.recovery_rate_percent =
            100.0 * report_.data.records_fully_recovered /
            report_.data.total_records_attempted;
    }
    if (report_.overall_status.empty()) {
        if (report_.data.recovery_rate_percent >= 99.0)
            report_.overall_status = "SUCCESS";
        else if (report_.data.recovery_rate_percent > 0.0)
            report_.overall_status = "PARTIAL";
        else
            report_.overall_status = "FAILED";
    }
    return report_;
}

} // namespace report
} // namespace dbrecovery