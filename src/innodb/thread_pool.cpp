#include "dbrecovery/innodb/thread_pool.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include <iostream>

namespace dbrecovery {
namespace innodb {

// ==================== ThreadPool 实现 ====================

// 模板实现已在头文件中

// ==================== ParallelPageParser 实现 ====================

ParallelPageParser::ParallelPageParser(size_t num_threads) 
    : thread_pool_(std::make_unique<ThreadPool>(num_threads)) {}

ParallelPageParser::~ParallelPageParser() = default;

void ParallelPageParser::set_pages(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages) {
    pages_ = pages;
    results_.clear();
    success_count_ = 0;
    failed_count_ = 0;
}

std::vector<std::pair<uint32_t, bool>> ParallelPageParser::parse_all(
    const std::function<std::unique_ptr<BPlusTreeParser>(const std::vector<uint8_t>&)>& parser_factory) {
    
    if (pages_.empty()) {
        return {};
    }

    results_.clear();
    results_.reserve(pages_.size());
    success_count_ = 0;
    failed_count_ = 0;

    std::vector<std::future<void>> futures;
    futures.reserve(pages_.size());

    for (const auto& [page_no, page_data] : pages_) {
        futures.push_back(thread_pool_->submit([this, page_no, page_data, parser_factory]() {
            auto parser = parser_factory(page_data);
            bool success = parser->parse();
            
            {
                std::lock_guard<std::mutex> lock(result_mutex_);
                results_.emplace_back(page_no, success);
            }
            
            if (success) {
                success_count_++;
            } else {
                failed_count_++;
            }
        }));
    }

    // 等待所有任务完成
    for (auto& future : futures) {
        future.get();
    }

    return results_;
}

} // namespace innodb
} // namespace dbrecovery
