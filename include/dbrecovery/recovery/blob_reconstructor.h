#pragma once
#include <vector>
#include <cstdint>

namespace dbrecovery {
namespace recovery {

class BlobReconstructor {
public:
    static std::vector<uint8_t> reconstruct(const uint8_t* data, size_t len);
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_BLOB_RECONSTRUCTOR_H
