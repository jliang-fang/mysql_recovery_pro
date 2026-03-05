#ifndef DBRECOVERY_INNODB_SDI_CACHE_HPP
#define DBRECOVERY_INNODB_SDI_CACHE_HPP

#include <unordered_map>
#include <string>
#include <memory>
#include <mutex>
#include "dbrecovery/innodb/sdi_types.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief SDI（Space Descriptor Index）缓存类，用于缓存表空间元数据。
 *
 * 在InnoDB中，SDI记录存储在每个表空间的头部区域，包含表结构信息等。
 * 本缓存用于加速对已解析SDI内容的访问，避免重复读取和解析。
 */
class SDICache {
public:
    using SDIPtr = std::shared_ptr<const SDIDescriptor>;

    /**
     * @brief 构造函数，默认最大缓存条目数为1024。
     */
    explicit SDICache(size_t max_size = 1024);

    /**
     * @brief 析构函数。
     */
    ~SDICache() = default;

    // 禁用拷贝构造和赋值（单例或资源管理类风格）
    SDICache(const SDICache&) = delete;
    SDICache& operator=(const SDICache&) = delete;

    /**
     * @brief 从缓存中获取指定表空间ID的SDI信息。
     * @param space_id 表空间ID
     * @return 若存在则返回SDI描述符指针，否则返回nullptr
     */
    SDIPtr get(uint32_t space_id) const;

    /**
     * @brief 将解析后的SDI信息存入缓存。
     * @param space_id 表空间ID
     * @param sdi SDI描述符指针
     */
    void put(uint32_t space_id, SDIPtr sdi);

    /**
     * @brief 清空整个缓存。
     */
    void clear();

    /**
     * @brief 获取当前缓存中条目数量。
     * @return 当前缓存大小
     */
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, SDIPtr> cache_;
    size_t max_size_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SDI_CACHE_HPP
