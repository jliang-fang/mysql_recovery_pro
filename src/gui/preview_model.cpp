// src/gui/preview_model.cpp
#include "gui/preview_model.hpp"
#include <QByteArray>
#include <fstream>
#include <filesystem>

void PreviewModel::setScanResult(const DBRecovery::Engine::ParallelScanner::ScanResult& result) {
    beginResetModel();
    
    schemas_ = result.schemas;
    rows_.clear();

    // ✅ 先预览：仅加载前 N 行（避免内存爆炸）
    constexpr size_t MAX_PREVIEW_ROWS = 100;
    for (const auto& [schema, records] : result.records) {
        if (rows_.size() >= MAX_PREVIEW_ROWS) break;

        for (size_t i = 0; i < std::min(records.size(), MAX_PREVIEW_ROWS - rows_.size()); ++i) {
            rows_.push_back(records[i]);
        }
    }

    endResetModel();
}

void PreviewModel::exportData(const QString& path) {
    if (rows_.empty()) return;

    // ✅ 安全写入：先写临时文件，再原子替换
    std::filesystem::path temp_path = 
        std::filesystem::temp_directory_path() / 
        ("recovery_" + std::to_string(std::time(nullptr)) + ".tmp");
    
    std::ofstream out(temp_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Failed to create temp file for export");
    }

    // 写入数据（简化示例）
    for (const auto& row : rows_) {
        for (const auto& field : row) {
            std::visit([&out](auto&& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int64_t>) out << v << "\t";
                else if constexpr (std::is_same_v<T, double>) out << v << "\t";
                else if constexpr (std::is_same_v<T, std::string>) out << "\"" << v << "\"\t";
                else if constexpr (std::is_same_v<T, std::monostate>) out << "NULL\t";
            }, field);
            out << "\n";
        }
    }

    out.close();

    // ✅ 原子替换（避免残留临时文件）
    std::filesystem::path final_path(path.toStdString());
    std::filesystem::rename(temp_path, final_path);
}