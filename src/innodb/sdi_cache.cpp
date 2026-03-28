#include "dbrecovery/innodb/sdi_cache.hpp"
#include <zlib.h>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace dbrecovery {
namespace innodb {

SDICache::SDICache(size_t max_size) : max_size_(max_size) {}

SDICache::SDIPtr SDICache::get(uint32_t space_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(space_id);
    if (it != cache_.end()) {
        return it->second;
    }
    return nullptr;
}

void SDICache::put(uint32_t space_id, SDIPtr sdi) {
    std::lock_guard<std::mutex> lock(mutex_);

    // LRU 替换策略：当缓存满时，移除最旧的条目
    if (cache_.size() >= max_size_) {
        // 移除第一个条目（最旧的）
        auto oldest_it = cache_.begin();
        cache_.erase(oldest_it);
    }

    cache_[space_id] = std::move(sdi);
}

void SDICache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

size_t SDICache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

} // namespace innodb
} // namespace dbrecovery
