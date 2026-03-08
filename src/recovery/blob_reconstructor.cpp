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
    
    // Check for overlap
    for (const auto& chunk : chunks_) {
        if (offset < chunk.offset + chunk.length && 
            chunk.offset < offset + data.size()) {
            return false;  // Overlap detected
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
