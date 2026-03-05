#ifndef DBRECOVERY_INNODB_PAGE_CACHE_HPP
#define DBRECOVERY_INNODB_PAGE_CACHE_HPP

#include <unordered_map>
#include <memory>
#include <mutex>
#include <cstdint>
#include "buffer/frame.hpp"
#include "common/logger.hpp"

namespace dbrecovery {
namespac einnodb {

class PageCache {
public:
    explicit PageCache(size_t capacity = 1024);
    ~PageCache();

    // 获取页：若缓存中不存在，则返回 nullptr；调用者需负责加载页
    Frame* get_page(page_id_t page_id);

    // 将页加入缓存（替换策略由内部实现）
    void put_page(Frame* frame);

    // 移除指定页（如释放或刷盘后）
    bool remove_page(page_id_t page_id);

    // 清空缓存（用于关闭或重置）
    void clear();

    size_t size() const { return frames_.size(); }
    size_t capacity() const { return capacity_; }

private:
    struct FrameHash {
        size_t operator()(const page_id_t& id) const noexcept {
            return std::hash<uint32_t>{}(id.first) ^ (std::hash<uint32_t>{}(id.second) << 1);
        }
    };

    using FrameMap = std::unordered_map<page_id_t, Frame*, FrameHash>;

    FrameMap frames_;
    size_t capacity_;
    mutable std::mutex mutex_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_CACHE_HPP
