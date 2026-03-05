#pragma once
#include <QAbstractItemModel>
#include "dbrecovery/engine/parallel_scanner.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

class PreviewModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Role { RowData = Qt::UserRole + 1, HexDump, ConfidenceScore };

    explicit PreviewModel(QObject* parent = nullptr);

    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex&) const override;
    int rowCount(const QModelIndex& = {}) const override;
    int columnCount(const QModelIndex& = {}) const override;
    QVariant data(const QModelIndex&, int role) const override;

    // ✅ 先预览：仅加载前 N 行（避免内存爆炸）
    void setScanResult(const DBRecovery::Engine::ParallelScanner::ScanResult&);

    // ✅ 导出接口：触发实际恢复流程
    Q_INVOKABLE void exportData(const QString& path);

private:
    std::vector<DBRecovery::Catalog::TableSchema> schemas_;
    std::vector<std::vector<FieldValue>> rows_; // 预览缓存（最大 100 行）
};
