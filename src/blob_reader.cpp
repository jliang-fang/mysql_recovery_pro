#include "blob_reader.h"

#include <cstring>
#include <stdexcept>
#include <iostream>

namespace blob {

BlobReader::BlobReader(const uint8_t* data, size_t size)
    : data_(data), size_(size), offset_(0) {}

bool BlobReader::ReadBytes(uint8_t* dest, size_t count) {
  if (offset_ + count > size_) {
    return false;
  }
  std::memcpy(dest, data_ + offset_, count);
  offset_ += count;
  return true;
}

bool BlobReader::ReadString(std::string& out, size_t max_len) {
  if (offset_ >= size_) {
    return false;
  }

  // Find null terminator or up to max_len bytes
  size_t len = 0;
  while (len < max_len && offset_ + len < size_ && data_[offset_ + len] != '\0') {
    ++len;
  }

  if (offset_ + len >= size_) {
    return false; // No null terminator found within bounds
  }

  out.assign(reinterpret_cast<const char*>(data_ + offset_), len);
  offset_ += len + 1; // Skip the null terminator
  return true;
}

bool BlobReader::ReadUint8(uint8_t& value) {
  if (offset_ + sizeof(uint8_t) > size_) {
    return false;
  }
  value = data_[offset_];
  offset_ += sizeof(uint8_t);
  return true;
}

bool BlobReader::ReadUint16LE(uint16_t& value) {
  if (offset_ + sizeof(uint16_t) > size_) {
    return false;
  }
  value = static_cast<uint16_t>(data_[offset_]) |
          (static_cast<uint16_t>(data_[offset_ + 1]) << 8);
  offset_ += sizeof(uint16_t);
  return true;
}

bool BlobReader::ReadUint32LE(uint32_t& value) {
  if (offset_ + sizeof(uint32_t) > size_) {
    return false;
  }
  value = static_cast<uint32_t>(data_[offset_]) |
          (static_cast<uint32_t>(data_[offset_ + 1]) << 8) |
          (static_cast<uint32_t>(data_[offset_ + 2]) << 16) |
          (static_cast<uint32_t>(data_[offset_ + 3]) << 24);
  offset_ += sizeof(uint32_t);
  return true;
}

bool BlobReader::ReadUint64LE(uint64_t& value) {
  if (offset_ + sizeof(uint64_t) > size_) {
    return false;
  }
  uint32_t low, high;
  if (!ReadUint32LE(low) || !ReadUint32LE(high)) {
    return false;
  }
  value = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
  return true;
}

bool BlobReader::Skip(size_t count) {
  if (offset_ + count > size_) {
    return false;
  }
  offset_ += count;
  return true;
}

} // namespace blob
