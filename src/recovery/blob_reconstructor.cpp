#include "dbrecovery/recovery/blob_reconstructor.hpp"
#include <algorithm>
#include <iostream>

namespace dbrecovery {
namespace recovery {

struct ChunkInfo {
    Offset offset;
    Length length;
    DataChunk data;
};

BlobReconstructor::BlobReconstructor(BlobId blob_id)
    : blob_id_(blob_id), current_size_(0) {}

BlobReconstructor::~BlobReconstructor() = default;

BlobReconstructor::BlobReconstructor(BlobReconstructor&& other) noexcept
    : chunks_(std::move(other.chunks_)),
      blob_id_(other.blob_id_),
      current_size_(other.current_size_) {
    other.current_size_ = 0;
}

BlobReconstructor& BlobReconstructor::operator=(BlobReconstructor&& other) noexcept {
    if (this != &other) {
        chunks_ = std::move(other.chunks_);
        blob_id_ = other.blob_id_;
        current_size_ = other.current_size_;
        other.current_size_ = 0;
    }
    return *this;
}

bool BlobReconstructor::AddChunk(Offset offset, const DataChunk& data) {
    if (data.empty()) {
        return false;
    }
    
    // 检查是否有重叠
    for (const auto& chunk : chunks_) {
        Offset old_start = chunk.offset;
        Offset old_end = chunk.offset + static_cast<Offset>(chunk.length);
        
        // 完全包含：新块被旧块完全覆盖，拒绝添加
        if (offset >= old_start && offset + data.size() <= old_end) {
            return false;  // 新数据完全在旧数据范围内，无需添加
        }
        
        // 部分重叠：优先保留已有数据（假设旧数据更可靠）
        if (offset < old_end && old_start < offset + data.size()) {
            std::cerr << "[WARN] Overlapping chunk detected, skipping" << std::endl;
            return false;  // 拒绝添加重叠数据
        }
    }
    
    chunks_.push_back({offset, static_cast<Length>(data.size()), data});
    current_size_ = std::max(current_size_, offset + static_cast<Offset>(data.size()));
    std::sort(chunks_.begin(), chunks_.end(),
              [](const auto& a, const auto& b) { return a.offset < b.offset; });
    return true;
}

bool BlobReconstructor::AddChunk(Offset offset, const uint8_t* ptr, Length len) {
    if (!ptr || len == 0) {
        return false;
    }
    
    DataChunk data(ptr, ptr + len);
    return AddChunk(offset, data);
}

bool BlobReconstructor::IsComplete(Length expected_size) const {
    if (chunks_.empty()) {
        return false;
    }
    
    // 检查是否有空洞
    std::vector<std::pair<Offset, Length>> sorted_chunks;
    for (const auto& chunk : chunks_) {
        sorted_chunks.emplace_back(chunk.offset, chunk.length);
    }
    std::sort(sorted_chunks.begin(), sorted_chunks.end());
    
    Offset expected_offset = 0;
    for (const auto& [offset, length] : sorted_chunks) {
        if (offset > expected_offset) {
            return false;  // 存在空洞
        }
        expected_offset = offset + length;
    }
    
    // 检查总长度是否匹配
    if (expected_size > 0 && expected_size != current_size_) {
        std::cerr << "[WARN] Expected size " << expected_size 
                  << " but got " << current_size_ << std::endl;
        return false;
    }
    
    return true;
}

std::vector<uint8_t> BlobReconstructor::GetBlob() const {
    if (chunks_.empty()) {
        return {};
    }
    
    std::vector<uint8_t> result(current_size_);
    
    // 按偏移排序
    std::vector<std::pair<Offset, Length>> sorted_chunks;
    for (const auto& chunk : chunks_) {
        sorted_chunks.emplace_back(chunk.offset, chunk.length);
    }
    std::sort(sorted_chunks.begin(), sorted_chunks.end());
    
    // 合并数据到结果缓冲区
    for (const auto& [offset, length] : sorted_chunks) {
        // 查找对应的 ChunkInfo
        for (const auto& chunk : chunks_) {
            if (chunk.offset == offset && chunk.length == length) {
                std::memcpy(result.data() + offset, chunk.data.data(), length);
                break;
            }
        }
    }
    
    return result;
}

std::optional<std::vector<uint8_t>> BlobReconstructor::reconstruct(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return std::nullopt;
    }
    
    // 简单实现：直接返回数据副本
    return std::vector<uint8_t>(data, data + len);
}

BlobReconstructor::Length BlobReconstructor::GetCurrentSize() const noexcept {
    return current_size_;
}

BlobReconstructor::BlobId BlobReconstructor::GetBlobId() const noexcept {
    return blob_id_;
}

} // namespace recovery
} // namespace dbrecovery
