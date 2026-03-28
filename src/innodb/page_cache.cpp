#include "dbrecovery/innodb/page_cache.hpp"
#include <algorithm>
#include <iostream>

namespace dbrecovery {
namespace innodb {

PageCache::PageCache(size_t max_size, size_t page_size)
    : max_size_(max_size), page_size_(page_size), ttl_seconds_(300),
      hits_(0), misses_(0) {
    if (max_size_ == 0) {
        max_size_ = 1024; // 默认 1024 页
    }
    if (page_size_ == 0) {
        page_size_ = 16384; // 默认 16KB
    }
}

PageCache::~PageCache() = default;

PageCache::PageCache(PageCache&& other) noexcept
    : max_size_(other.max_size_), page_size_(other.page_size_),
      ttl_seconds_(other.ttl_seconds_), cache_(std::move(other.cache_)),
      reader_(std::move(other.reader_)), hits_(other.hits_), misses_(other.misses_) {
    other.hits_ = 0;
    other.misses_ = 0;
}

PageCache& PageCache::operator=(PageCache&& other) noexcept {
    if (this != &other) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> other_lock(other.mutex_);
        
        max_size_ = other.max_size_;
        page_size_ = other.page_size_;
        ttl_seconds_ = other.ttl_seconds_;
        cache_ = std::move(other.cache_);
        reader_ = std::move(other.reader_);
        hits_ = other.hits_;
        misses_ = other.misses_;
        
        other.hits_ = 0;
        other.misses_ = 0;
    }
    return *this;
}

std::optional<std::vector<uint8_t>> PageCache::get(uint32_t page_no) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_.find(page_no);
    if (it == cache_.end()) {
        misses_++;
        
        // 尝试从 reader 读取
        if (reader_) {
            auto data = reader_(page_no);
            if (data.has_value()) {
                put(page_no, std::move(data.value()));
                hits_++;
                return data;
            }
        }
        
        return std::nullopt;
    }
    
    // 检查是否过期
    if (is_expired(it->second)) {
        cache_.erase(it);
        misses_++;
        return std::nullopt;
    }
    
    touch(it->second);
    hits_++;
    return it->second.page_data;
}

void PageCache::put(uint32_t page_no, std::vector<uint8_t> page_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查页大小
    if (!page_data.empty() && page_data.size() != page_size_) {
        std::cerr << "[WARN] Page size mismatch: expected " << page_size_
                  << ", got " << page_data.size() << std::endl;
    }
    
    // 检查是否已存在
    auto it = cache_.find(page_no);
    if (it != cache_.end()) {
        it->second.page_data = std::move(page_data);
        touch(it->second);
        return;
    }
    
    // 检查缓存是否已满
    while (cache_.size() >= max_size_) {
        evict_lru();
    }
    
    // 插入新节点
    CacheNode node;
    node.page_no = page_no;
    node.page_data = std::move(page_data);
    node.last_access = std::chrono::steady_clock::now();
    node.access_count = 1;
    
    cache_[page_no] = std::move(node);
}

bool PageCache::remove(uint32_t page_no) {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.erase(page_no) > 0;
}

void PageCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

void PageCache::set_ttl(int ttl) {
    std::lock_guard<std::mutex> lock(mutex_);
    ttl_seconds_ = ttl;
}

PageCache::CacheStats PageCache::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    CacheStats stats;
    stats.hits = hits_;
    stats.misses = misses_;
    stats.current_size = cache_.size();
    stats.max_size = max_size_;
    
    size_t total = hits_ + misses_;
    stats.hit_rate = (total > 0) ? static_cast<double>(hits_) / total : 0.0;
    
    return stats;
}

void PageCache::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    hits_ = 0;
    misses_ = 0;
}

size_t PageCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

bool PageCache::is_full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size() >= max_size_;
}

void PageCache::set_reader(std::function<std::optional<std::vector<uint8_t>>(uint32_t)> reader) {
    std::lock_guard<std::mutex> lock(mutex_);
    reader_ = std::move(reader);
}

void PageCache::evict_lru() {
    if (cache_.empty()) return;
    
    // 找到最久未访问的节点
    auto it = std::min_element(cache_.begin(), cache_.end(),
        [](const auto& a, const auto& b) {
            return a.second.last_access < b.second.last_access;
        });
    
    cache_.erase(it);
}

void PageCache::touch(CacheNode& node) {
    node.last_access = std::chrono::steady_clock::now();
    node.access_count++;
}

bool PageCache::is_expired(const CacheNode& node) const {
    if (ttl_seconds_ <= 0) return false; // 0 或负数表示永不过期
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - node.last_access).count();
    
    return elapsed > ttl_seconds_;
}

// ==================== PageCacheManager 实现 ====================

PageCacheManager& PageCacheManager::instance() {
    static PageCacheManager instance;
    return instance;
}

PageCacheManager::PageCacheManager()
    : default_max_pages_(1024), default_page_size_(16384) {
    // 初始化默认缓存
    caches_[0] = std::make_unique<PageCache>(default_max_pages_, default_page_size_);
}

PageCacheManager::~PageCacheManager() = default;

PageCache& PageCacheManager::get_cache(uint32_t space_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = caches_.find(space_id);
    if (it == caches_.end()) {
        // 创建新缓存
        auto cache = std::make_unique<PageCache>(default_max_pages_, default_page_size_);
        auto ptr = cache.get();
        caches_[space_id] = std::move(cache);
        return *ptr;
    }
    
    return *it->second;
}

void PageCacheManager::set_max_pages(size_t max_pages) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_max_pages_ = max_pages;
    
    // 更新所有现有缓存
    for (auto& [space_id, cache] : caches_) {
        // 注意：这里不直接修改 max_size_，因为需要线程安全
        // 实际应用中可能需要添加 set_max_size 方法
    }
}

void PageCacheManager::set_page_size(size_t page_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_page_size_ = page_size;
}

void PageCacheManager::clear_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [space_id, cache] : caches_) {
        cache->clear();
    }
}

} // namespace innodb
} // namespace dbrecovery
