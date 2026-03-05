#ifndef DBRECOVERY_RECOVERY_COMMON_HPP
#define DBRECOVERY_RECOVERY_COMMON_HPP
#include <cstdint>

namespace dbrecovery {
namespace recovery {

struct Common {
    static constexpr size_t MAX_BLOB_SIZE = 1024 * 1024 * 1024; // 1GB
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_COMMON_HPP
