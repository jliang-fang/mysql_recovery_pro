#include "dbrecovery/innodb/sdi_cache.hpp"
#include <zlib.h>
#include <sstream>
#include <stdexcept>

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
    
    // 简单 LRU 替换策略（实际可优化为双向链表）
    if (cache_.size() >= max_size_) {
        cache_.clear(); // 暂时采用最简单的清空策略
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

// 辅助函数：解压 zlib 数据
static std::string decompress_zlib(const uint8_t* data, size_t compressed_len) {
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    stream.avail_in = static_cast<uInt>(compressed_len);
    
    // 分配解压缓冲区
    std::vector<char> buffer(1024 * 1024); // 初始 1MB
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());

    int ret = inflateInit(&stream);
    if (ret != Z_OK) {
        throw std::runtime_error("zlib inflateInit failed");
    }

    ret = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);

    if (ret != Z_STREAM_END) {
        throw std::runtime_error("zlib decompression failed");
    }

    return std::string(buffer.data(), stream.total_out);
}

// 解析 SDI 页数据
SDIDescriptor SDICache::parse_sdi_page(const uint8_t* page_data, size_t page_size) {
    // 1. 检查页类型是否为 FIL_PAGE_SDI (9971)
    uint32_t page_type = *reinterpret_cast<const uint32_t*>(page_data);
    if (page_type != 9971) {
        throw std::runtime_error("Not a SDI page");
    }

    // 2. 解析 SDI payload
    // 偏移 38 开始是 payload，前 20 字节是 SDI header
    const uint8_t* sdi_payload = page_data + 38;
    
    if (page_size < 58) {
        throw std::runtime_error("SDI page too small");
    }

    // 解析 SDI Header (20 bytes)
    // type: uint32, id: uint64, uncompressed_len: uint32, compressed_len: uint32
    uint32_t sdi_type = *reinterpret_cast<const uint32_t*>(sdi_payload);
    if (sdi_type != 1) {
        throw std::runtime_error("Unsupported SDI type");
    }

    uint64_t id = *reinterpret_cast<const uint64_t*>(sdi_payload + 4);
    uint32_t uncompressed_len = *reinterpret_cast<const uint32_t*>(sdi_payload + 12);
    uint32_t compressed_len = *reinterpret_cast<const uint32_t*>(sdi_payload + 16);

    if (compressed_len == 0 || uncompressed_len == 0) {
        throw std::runtime_error("Invalid SDI length");
    }

    const uint8_t* compressed_data = sdi_payload + 20;
    
    // 3. 解压
    std::string json_str = decompress_zlib(compressed_data, compressed_len);
    if (json_str.size() != uncompressed_len) {
        throw std::runtime_error("SDI decompression length mismatch");
    }

    // 4. 解析 JSON (此处为伪代码，实际需集成 nlohmann/json)
    // SDIDescriptor sdi = json::parse(json_str);
    
    // TODO: 实际解析逻辑
    SDIDescriptor sdi;
    sdi.space_id = static_cast<uint32_t>(id);
    sdi.table_schema.table_name = "users"; // 占位符
    
    return sdi;
}

} // namespace innodb
} // namespace dbrecovery
