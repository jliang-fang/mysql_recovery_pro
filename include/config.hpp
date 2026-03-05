#pragma once

// Configuration header for InnoDB blob pointer handling
// This file defines compile-time constants and configuration options

#include <cstdint>

namespace innodb {

// Maximum length of a BLOB pointer in bytes (including metadata)
constexpr std::size_t MAX_BLOB_POINTER_SIZE = 20;

// Offset to the actual data when using external storage
constexpr std::uint32_t BLOB_POINTER_DATA_OFFSET = 4;

// Flag indicating whether the BLOB is stored externally
constexpr std::uint32_t BLOB_FLAG_EXTERNAL = 0x80000000U;

// Mask to extract the actual length from a BLOB pointer
constexpr std::uint32_t BLOB_LENGTH_MASK = 0x7FFFFFFFU;

} // namespace innodb
