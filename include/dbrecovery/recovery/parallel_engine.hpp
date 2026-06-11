#ifndef DBRECOVERY_RECOVERY_PARALLEL_ENGINE_HPP
#define DBRECOVERY_RECOVERY_PARALLEL_ENGINE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/engine/parallel_scanner.hpp"

namespace dbrecovery {
namespace recovery {

/**
 * @brief ParallelRecoveryEngine: 并行恢复引擎
 *
 * 继承 RecoveryEngine 并添加并行扫描和实时进度显示功能
 */
class ParallelRecoveryEngine : public RecoveryEngine {
public:
    explicit ParallelRecoveryEngine();
    ~ParallelRecoveryEngine();

    /**
     * @brief 设置并行线程数
     */
    void set_thread_count(size_t threads);

    /**
     * @brief 设置进度回调
     */
    void set_progress_callback(std::function<void(double)> callback);

    /**
     * @brief 并行恢复被 Truncate 的表
     */
    std::vector<std::map<std::string, std::string>> parallel_recover_truncate(
        const catalog::TableSchema& schema);

    /**
     * @brief 获取扫描统计信息
     */
    struct ScanStats {
        size_t total_pages = 0;
        size_t valid_pages = 0;
        size_t index_pages = 0;
        double elapsed_seconds = 0.0;
        double pages_per_second = 0.0;
    };

    ScanStats get_scan_stats() const;

private:
    // 并行扫描工作函数
    bool process_page_batch(uint32_t start_page, uint32_t end_page,
                           const catalog::TableSchema& schema,
                           std::vector<std::map<std::string, std::string>>& records);

    // 并行处理器
    std::unique_ptr<engine::ParallelScanner> parallel_scanner_;
    std::function<void(double)> progress_callback_;
    ScanStats scan_stats_;
    size_t thread_count_ = 0;
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_PARALLEL_ENGINE_HPP
