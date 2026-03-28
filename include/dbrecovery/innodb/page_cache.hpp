#ifndef DBRECOVERY_INNODB_PAGE_CACHE_HPP
#define DBRECOVERY_INNODB_PAGE_CACHE_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <mutex>
#include <optional>
#include <memory>
#include <functional>
#include <chrono>

namespace dbrecovery {
namespace innodb {

/**
 * @brief PageCache: InnoDB 页缓存管理器
 *
 * 提供以下能力：
 * - LRU 缓存策略
 * - 缓存命中率统计
 * - 自动过期机制
 * - 线程安全访问
 */
class PageCache {
public:
    /**
     * @brief 构造函数
     *
     * @param max_size 最大缓存页数
     * @param page_size 页大小（字节）
     */
    explicit PageCache(size_t max_size = 1024, size_t page_size = 16384);
    ~PageCache();

    // 禁止拷贝
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    // 支持移动语义
    PageCache(PageCache&& other) noexcept;
    PageCache& operator=(PageCache&& other) noexcept;

    /**
     * @brief 从缓存中读取页
     *
     * @param page_no 页号
     * @return std::optional<std::vector<uint8_t>> 页数据，未命中返回空
     */
    std::optional<std::vector<uint8_t>> get(uint32_t page_no);

    /**
     * @brief 将页写入缓存
     *
     * @param page_no 页号
     * @param page_data 页数据
     */
    void put(uint32_t page_no, std::vector<uint8_t> page_data);

    /**
     * @brief 从缓存中删除页
     *
     * @param page_no 页号
     * @return true 删除成功
     * @return false 页不存在
     */
    bool remove(uint32_t page_no);

    /**
     * @brief 清空缓存
     */
    void clear();

    /**
     * @brief 设置缓存过期时间（秒）
     *
     * @param ttl 过期时间（秒）
     */
    void set_ttl(int ttl);

    /**
     * @brief 获取缓存统计信息
     */
    struct CacheStats {
        size_t hits = 0;              // 缓存命中次数
        size_t misses = 0;            // 缓存未命中次数
        size_t current_size = 0;      // 当前缓存大小
        size_t max_size = 0;          // 最大缓存大小
        double hit_rate = 0.0;        // 命中率
    };

    CacheStats get_stats() const;

    /**
     * @brief 重置统计信息
     */
    void reset_stats();

    /**
     * @brief 获取当前缓存大小
     */
    size_t size() const;

    /**
     * @brief 检查缓存是否已满
     */
    bool is_full() const;

    /**
     * @brief 设置页读取回调（用于缓存未命中时读取）
     *
     * @param reader 读取回调函数
     */
    void set_reader(std::function<std::optional<std::vector<uint8_t>>(uint32_t)> reader);

private:
    // LRU 节点
    struct CacheNode {
        uint32_t page_no;
        std::vector<uint8_t> page_data;
        std::chrono::steady_clock::time_point last_access;
        size_t access_count = 0;
    };

    // 移除最久未使用的页
    void evict_lru();

    // 更新访问时间
    void touch(CacheNode& node);

    // 检查是否过期
    bool is_expired(const CacheNode& node) const;

private:
    size_t max_size_;
    size_t page_size_;
    int ttl_seconds_;

    mutable std::mutex mutex_;
    std::map<uint32_t, CacheNode> cache_;
    std::function<std::optional<std::vector<uint8_t>>(uint32_t)> reader_;

    // 统计信息
    mutable size_t hits_;
    mutable size_t misses_;
};

/**
 * @brief PageCacheManager: 全局页缓存管理器（单例）
 */
class PageCacheManager {
public:
    /**
     * @brief 获取全局实例
     */
    static PageCacheManager& instance();

    /**
     * @brief 获取缓存
     *
     * @param space_id 表空间ID
     * @return PageCache& 对应的缓存
     */
    PageCache& get_cache(uint32_t space_id = 0);

    /**
     * @brief 设置全局缓存大小
     *
     * @param max_pages 最大页数
     */
    void set_max_pages(size_t max_pages);

    /**
     * @brief 设置全局页大小
     *
     * @param page_size 页大小
     */
    void set_page_size(size_t page_size);

    /**
     * @brief 清空所有缓存
     */
    void clear_all();

private:
    PageCacheManager();
    ~PageCacheManager();

    std::map<uint32_t, std::unique_ptr<PageCache>> caches_;
    size_t default_max_pages_;
    size_t default_page_size_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_CACHE_HPP
