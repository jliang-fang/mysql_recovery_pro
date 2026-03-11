#include "dbrecovery/recovery/blob_reconstructor.hpp"
#include <cstring>
#include <algorithm>

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
    if (data.empty()) return false;
    
    // ✅ 增强：验证偏移和数据大小的合理性
    if (offset > 0x100000000ULL) { // 限制最大偏移到4GB
        std::cerr << "[WARN] Blob chunk offset too large: " << offset << "\n";
        return false;
    }
    
    // Check for overlap and handle edge cases
    for (const auto& chunk : chunks_) {
        Offset new_start = offset;
        Offset new_end = offset + data.size();
        Offset old_start = chunk.offset;
        Offset old_end = chunk.offset + chunk.length;
        
        // 完全包含：新块被旧块完全覆盖，拒绝添加
        if (new_start >= old_start && new_end <= old_end) {
            return false;  // 新数据完全在旧数据范围内，无需添加
        }
        
        // 部分重叠：优先保留已有数据（假设旧数据更可靠）
        if (new_start < old_end && old_start < new_end) {
            // 如果新块的起始位置比旧块早，则允许追加到末尾
            if (new_start < old_start) {
                size_t overlap = static_cast<size_t>(old_start - new_start);
                if (overlap >= data.size()) return false; // 完全重叠
                // 只添加不重叠的部分
                DataChunk new_data(data.begin() + overlap, data.end());
                return AddChunk(new_start + overlap, new_data);
            }
            return false;  // 其他情况拒绝添加
        }
    }
    
    chunks_.push_back({offset, static_cast<Length>(data.size()), data});
    current_size_ = std::max(current_size_, offset + static_cast<Offset>(data.size()));
    std::sort(chunks_.begin(), chunks_.end(),
              [](const auto& a, const auto& b) { return a.offset < b.offset; });
    return true;
}

bool BlobReconstructor::AddChunk(Offset offset, const uint8_t* ptr, Length len) {
    if (!ptr || len == 0) return false;
    DataChunk data(ptr, ptr + len);
    return AddChunk(offset, data);
}

bool BlobReconstructor::IsComplete(Length expected_size) const {
    if (chunks_.empty()) return false;
    
    // Check for gaps
    Offset next_offset = 0;
    for (const auto& chunk : chunks_) {
        if (chunk.offset > next_offset) return false;  // Gap found
        next_offset = (next_offset > chunk.offset + chunk.length) ? next_offset : (chunk.offset + chunk.length);
    }
    
    return expected_size == 0 || next_offset == expected_size;
}

std::vector<uint8_t> BlobReconstructor::GetBlob() const {
    if (chunks_.empty()) return {};
    
    std::vector<uint8_t> result(current_size_);
    for (const auto& chunk : chunks_) {
        std::memcpy(result.data() + chunk.offset, chunk.data.data(), chunk.length);
    }
    return result;
}

std::optional<std::vector<uint8_t>> BlobReconstructor::reconstruct(const uint8_t* data, size_t len) {
    if (!data || len == 0) return std::nullopt;
    return std::vector<uint8_t>(data, data + len);
}

BlobReconstructor::Length 
BlobReconstructor::GetCurrentSize() const noexcept { return current_size_; }

BlobReconstructor::BlobId 
BlobReconstructor::GetBlobId() const noexcept { return blob_id_; }

} // namespace recovery
} // namespace dbrecovery
