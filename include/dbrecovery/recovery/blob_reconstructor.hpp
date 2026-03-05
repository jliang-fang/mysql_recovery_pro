#ifndef DBRECOVERY_RECOVERY_BLOB_RECONSTRUCTOR_HPP
#define DBRECOVERY_RECOVERY_BLOB_RECONSTRUCTOR_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <optional>

namespace dbrecovery {
namespace recovery {

class BlobReconstructor {
public:
    using BlobId = uint64_t;
    using Offset = uint64_t;
    using Length = uint32_t;
    using DataChunk = std::vector<uint8_t>;

    explicit BlobReconstructor(BlobId blob_id);
    ~BlobReconstructor();

    // 禁止拷贝
    BlobReconstructor(const BlobReconstructor&) = delete;
    BlobReconstructor& operator=(const BlobReconstructor&) = delete;

    // 允许移动
    BlobReconstructor(BlobReconstructor&&) noexcept;
    BlobReconstructor& operator=(BlobReconstructor&&) noexcept;

    bool AddChunk(Offset offset, const DataChunk& data);
    bool AddChunk(Offset offset, const uint8_t* ptr, Length len);

    bool IsComplete(Length expected_size = 0) const;
    DataChunk GetBlob() const;

    std::optional<std::vector<uint8_t>> reconstruct(const uint8_t* data, size_t len);

    // 声明（实现移至 .cpp）
    Length GetCurrentSize() const noexcept;
    BlobId GetBlobId() const noexcept;

private:
    struct ChunkInfo {
        Offset offset;
        Length length;
        DataChunk data;
    };

    std::vector<ChunkInfo> chunks_;
    BlobId blob_id_;
    Length current_size_ = 0;
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_BLOB_RECONSTRUCTOR_HPP
