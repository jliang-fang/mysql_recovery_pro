#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "dbrecovery/innodb/index_page_parser.hpp"

namespace {

constexpr const char* UNPARSABLE = "__JSON_UNPARSABLE__";

// ---------------------------------------------------------------------------
// Binary JSON builder helpers (MySQL binary JSON format)
// ---------------------------------------------------------------------------

void append_le16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

void append_le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

void append_le64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; i++) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

// Build a small-object binary JSON doc with literal/inline-int16 values only.
// Layout: [type][count][size][key_entries][value_entries][keys]
// All entry offsets are relative to data_start (index 1, right after type).
std::vector<uint8_t> build_small_object(
    const std::vector<std::pair<std::string, std::pair<uint8_t, uint16_t>>>& entries) {
    std::vector<uint8_t> doc;
    doc.push_back(0x00);  // small object
    const size_t count = entries.size();
    append_le16(doc, static_cast<uint16_t>(count));
    const size_t size_pos = doc.size();
    append_le16(doc, 0);  // placeholder for total_size

    // Relative to data_start = index 1:
    //   header (count+size) occupies rel 0..3, so key entries start at rel 4.
    const size_t key_entry_base_rel = 4;
    const size_t value_entry_base_rel = key_entry_base_rel + count * 4;
    size_t key_cursor = value_entry_base_rel + count * 3;

    std::vector<uint8_t> key_entries, value_entries, key_bytes;
    for (const auto& e : entries) {
        append_le16(key_entries, static_cast<uint16_t>(key_cursor));
        append_le16(key_entries, static_cast<uint16_t>(e.first.size()));
        value_entries.push_back(e.second.first);
        append_le16(value_entries, e.second.second);
        key_bytes.insert(key_bytes.end(), e.first.begin(), e.first.end());
        key_cursor += e.first.size();
    }
    doc.insert(doc.end(), key_entries.begin(), key_entries.end());
    doc.insert(doc.end(), value_entries.begin(), value_entries.end());
    doc.insert(doc.end(), key_bytes.begin(), key_bytes.end());

    const uint16_t total_size = static_cast<uint16_t>(doc.size() - 1);
    doc[size_pos] = static_cast<uint8_t>(total_size & 0xFF);
    doc[size_pos + 1] = static_cast<uint8_t>((total_size >> 8) & 0xFF);
    return doc;
}

// Build a small-array binary JSON doc with literal/inline-int16 values only.
std::vector<uint8_t> build_small_array(
    const std::vector<std::pair<uint8_t, uint16_t>>& entries) {
    std::vector<uint8_t> doc;
    doc.push_back(0x02);  // small array
    append_le16(doc, static_cast<uint16_t>(entries.size()));
    const size_t size_pos = doc.size();
    append_le16(doc, 0);
    for (const auto& e : entries) {
        doc.push_back(e.first);
        append_le16(doc, e.second);
    }
    const uint16_t total_size = static_cast<uint16_t>(doc.size() - 1);
    doc[size_pos] = static_cast<uint8_t>(total_size & 0xFF);
    doc[size_pos + 1] = static_cast<uint8_t>((total_size >> 8) & 0xFF);
    return doc;
}

// Build a small-array containing a stored (non-inline) value.
// `entry_type` is the value-entry type tag; `payload` is appended verbatim at
// the computed value offset.
std::vector<uint8_t> build_array_with_stored(uint8_t entry_type,
                                             const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> doc;
    doc.push_back(0x02);
    append_le16(doc, 1);
    const size_t size_pos = doc.size();
    append_le16(doc, 0);
    const size_t value_entry_pos = doc.size();
    doc.push_back(entry_type);
    append_le16(doc, 0);  // offset placeholder, relative to data_start
    const size_t payload_off_rel = doc.size() - 1;  // rel to data_start (offset 1)
    doc[value_entry_pos + 1] = static_cast<uint8_t>(payload_off_rel & 0xFF);
    doc[value_entry_pos + 2] = static_cast<uint8_t>((payload_off_rel >> 8) & 0xFF);
    doc.insert(doc.end(), payload.begin(), payload.end());
    const uint16_t total_size = static_cast<uint16_t>(doc.size() - 1);
    doc[size_pos] = static_cast<uint8_t>(total_size & 0xFF);
    doc[size_pos + 1] = static_cast<uint8_t>((total_size >> 8) & 0xFF);
    return doc;
}

std::vector<uint8_t> le16_bytes(uint16_t x) {
    return {static_cast<uint8_t>(x & 0xFF), static_cast<uint8_t>((x >> 8) & 0xFF)};
}
std::vector<uint8_t> le32_bytes(uint32_t x) {
    return {static_cast<uint8_t>(x & 0xFF), static_cast<uint8_t>((x >> 8) & 0xFF),
            static_cast<uint8_t>((x >> 16) & 0xFF), static_cast<uint8_t>((x >> 24) & 0xFF)};
}
std::vector<uint8_t> le64_bytes(uint64_t x) {
    std::vector<uint8_t> v;
    for (int i = 0; i < 8; i++) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
    return v;
}
std::vector<uint8_t> str_payload(const std::string& s) {
    std::vector<uint8_t> v;
    if (s.size() < 128) {
        v.push_back(static_cast<uint8_t>(s.size()));
    } else {
        v.push_back(static_cast<uint8_t>(0x80 | (s.size() & 0x7F)));
        v.push_back(static_cast<uint8_t>((s.size() >> 7) & 0xFF));
    }
    v.insert(v.end(), s.begin(), s.end());
    return v;
}

// ---------------------------------------------------------------------------
// Valid decoding cases
// ---------------------------------------------------------------------------

TEST(BinaryJsonDecode, RejectsNullAndEmpty) {
    EXPECT_EQ(decode_json_binary(nullptr, 0, 0), UNPARSABLE);
    uint8_t buf[4] = {0};
    EXPECT_EQ(decode_json_binary(buf, 0, 0), UNPARSABLE);
}

TEST(BinaryJsonDecode, SmallObjectLiterals) {
    auto doc = build_small_object({
        {"a", {0x04, 1}},   // true
        {"b", {0x04, 2}},   // false
        {"c", {0x04, 0}},   // null
    });
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()),
              "{\"a\":true,\"b\":false,\"c\":null}");
}

TEST(BinaryJsonDecode, SmallArrayInlineInts) {
    auto doc = build_small_array({
        {0x05, static_cast<uint16_t>(-12345)},  // INT16 inline (signed)
        {0x06, 65530},                          // UINT16 inline
        {0x04, 1},
    });
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), "[-12345,65530,true]");
}

TEST(BinaryJsonDecode, SmallObjectStoredValues) {
    // One key "k" mapped to stored int32 -42.
    std::vector<uint8_t> doc;
    doc.push_back(0x00);  // small object
    append_le16(doc, 1);
    const size_t size_pos = doc.size();
    append_le16(doc, 0);
    // key entry
    const size_t ke_pos = doc.size();
    append_le16(doc, 0);    // key offset placeholder
    append_le16(doc, 1);    // key length
    // value entry
    doc.push_back(0x07);    // int32 stored
    const size_t ve_off_pos = doc.size();
    append_le16(doc, 0);
    // key bytes
    const size_t key_off = doc.size() - 1;
    doc.push_back('k');
    // value bytes
    const size_t val_off = doc.size() - 1;
    append_le32(doc, static_cast<uint32_t>(-42));
    doc[ke_pos] = static_cast<uint8_t>(key_off & 0xFF);
    doc[ke_pos + 1] = static_cast<uint8_t>((key_off >> 8) & 0xFF);
    doc[ve_off_pos] = static_cast<uint8_t>(val_off & 0xFF);
    doc[ve_off_pos + 1] = static_cast<uint8_t>((val_off >> 8) & 0xFF);
    const uint16_t total = static_cast<uint16_t>(doc.size() - 1);
    doc[size_pos] = static_cast<uint8_t>(total & 0xFF);
    doc[size_pos + 1] = static_cast<uint8_t>((total >> 8) & 0xFF);

    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), "{\"k\":-42}");
}

TEST(BinaryJsonDecode, StoredInt64AndDouble) {
    auto d64 = build_array_with_stored(0x09, le64_bytes(static_cast<uint64_t>(-9000000000LL)));
    EXPECT_EQ(decode_json_binary(d64.data(), 0, d64.size()), "[-9000000000]");

    double dbl = 3.25;
    uint64_t bits;
    std::memcpy(&bits, &dbl, 8);
    auto dd = build_array_with_stored(0x0B, le64_bytes(bits));
    EXPECT_EQ(decode_json_binary(dd.data(), 0, dd.size()), "[3.25]");
}

TEST(BinaryJsonDecode, StoredStringWithEscapes) {
    auto doc = build_array_with_stored(0x0C, str_payload("a\"b\\c\nd"));
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), "[\"a\\\"b\\\\c\\nd\"]");
}

TEST(BinaryJsonDecode, KeyWithControlByteIsEscaped) {
    std::string key = "a";
    key.push_back('\x01');
    auto doc = build_small_object({{key, {0x04, 1}}});
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), "{\"a\\u0001\":true}");
}

TEST(BinaryJsonDecode, NestedObject) {
    // {"inner": { ... }} — nested small object embedded as stored value.
    auto inner = build_small_object({{"x", {0x04, 1}}});
    std::vector<uint8_t> inner_body(inner.begin() + 1, inner.end());  // strip type byte

    std::vector<uint8_t> doc;
    doc.push_back(0x00);
    append_le16(doc, 1);
    const size_t size_pos = doc.size();
    append_le16(doc, 0);
    const size_t ke_pos = doc.size();
    append_le16(doc, 0);
    append_le16(doc, 5);
    doc.push_back(0x00);  // nested small object
    const size_t ve_off = doc.size();
    append_le16(doc, 0);
    const size_t key_off = doc.size() - 1;
    doc.insert(doc.end(), {'i', 'n', 'n', 'e', 'r'});
    const size_t val_off = doc.size() - 1;
    doc.insert(doc.end(), inner_body.begin(), inner_body.end());
    doc[ke_pos] = static_cast<uint8_t>(key_off & 0xFF);
    doc[ke_pos + 1] = static_cast<uint8_t>((key_off >> 8) & 0xFF);
    doc[ve_off] = static_cast<uint8_t>(val_off & 0xFF);
    doc[ve_off + 1] = static_cast<uint8_t>((val_off >> 8) & 0xFF);
    const uint16_t total = static_cast<uint16_t>(doc.size() - 1);
    doc[size_pos] = static_cast<uint8_t>(total & 0xFF);
    doc[size_pos + 1] = static_cast<uint8_t>((total >> 8) & 0xFF);

    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()),
              "{\"inner\":{\"x\":true}}");
}

TEST(BinaryJsonDecode, EmptyObjectAndArray) {
    std::vector<uint8_t> obj = {0x00};
    append_le16(obj, 0);
    append_le16(obj, 4);  // total_size = 4 bytes after type
    EXPECT_EQ(decode_json_binary(obj.data(), 0, obj.size()), "{}");

    std::vector<uint8_t> arr = {0x02};
    append_le16(arr, 0);
    append_le16(arr, 4);
    EXPECT_EQ(decode_json_binary(arr.data(), 0, arr.size()), "[]");
}

// ---------------------------------------------------------------------------
// Malformed cases — every one must fail closed, never emit partial JSON
// ---------------------------------------------------------------------------

TEST(BinaryJsonDecode, RejectsTopLevelScalar) {
    // A bare INT16 type tag at top level is not a container: refuse.
    std::vector<uint8_t> doc = {0x05, 0x2A, 0x00};
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsTotalSizeMismatch) {
    auto doc = build_small_array({{0x04, 1}});
    doc[3] ^= 0xFF;  // corrupt total_size low byte (size field at doc[3..4])
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsExcessiveElementCount) {
    std::vector<uint8_t> doc;
    doc.push_back(0x02);
    append_le16(doc, 2000);          // count exceeds JSON_MAX_ELEMENTS
    append_le16(doc, 4 + 2000 * 3);  // consistent size claim
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsTruncatedValueEntry) {
    auto doc = build_small_array({{0x04, 1}, {0x04, 2}});
    doc.pop_back();  // truncate inside value entries; total_size no longer matches
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsOutOfRangeValueOffset) {
    auto doc = build_array_with_stored(0x07, le32_bytes(7));
    // Corrupt the stored value offset to point past the end.
    doc[5] = 0xF0;
    doc[6] = 0x7F;
    // Recompute total_size so only the offset is wrong.
    const uint16_t total = static_cast<uint16_t>(doc.size() - 1);
    doc[3] = static_cast<uint8_t>(total & 0xFF);
    doc[4] = static_cast<uint8_t>((total >> 8) & 0xFF);
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsTruncatedInt32) {
    // Value offset valid but fewer than 4 bytes remain.
    std::vector<uint8_t> doc;
    doc.push_back(0x02);
    append_le16(doc, 1);
    const size_t size_pos = doc.size();
    append_le16(doc, 0);
    doc.push_back(0x07);  // int32
    append_le16(doc, 7);  // offset rel to data_start → last 2 bytes of doc
    doc.push_back(0x01);
    doc.push_back(0x02);
    const uint16_t total = static_cast<uint16_t>(doc.size() - 1);
    doc[size_pos] = static_cast<uint8_t>(total & 0xFF);
    doc[size_pos + 1] = static_cast<uint8_t>((total >> 8) & 0xFF);
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsInvalidLiteral) {
    auto doc = build_small_array({{0x04, 9}});  // literal 9 undefined
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsUnknownTypeTag) {
    auto doc = build_small_array({{0x7E, 0}});
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsNonFiniteDouble) {
    uint64_t inf_bits = 0x7FF0000000000000ULL;  // +inf
    auto doc = build_array_with_stored(0x0B, le64_bytes(inf_bits));
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);

    uint64_t nan_bits = 0x7FF8000000000000ULL;  // quiet NaN
    auto doc2 = build_array_with_stored(0x0B, le64_bytes(nan_bits));
    EXPECT_EQ(decode_json_binary(doc2.data(), 0, doc2.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsTruncatedString) {
    // String claims 50 bytes but only 2 remain.
    std::vector<uint8_t> doc;
    doc.push_back(0x02);
    append_le16(doc, 1);
    const size_t size_pos = doc.size();
    append_le16(doc, 0);
    doc.push_back(0x0C);
    append_le16(doc, 7);
    doc.push_back(50);   // declared length
    doc.push_back('x');
    doc.push_back('y');
    const uint16_t total = static_cast<uint16_t>(doc.size() - 1);
    doc[size_pos] = static_cast<uint8_t>(total & 0xFF);
    doc[size_pos + 1] = static_cast<uint8_t>((total >> 8) & 0xFF);
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsExcessiveNesting) {
    // Build 40 nested arrays: [[[[...]]]] — exceeds JSON_MAX_DEPTH of 32.
    // Innermost: empty array body {count=0,size=4} = 4 bytes.
    std::vector<uint8_t> body = {0x00, 0x00, 0x04, 0x00};  // count=0, size=4
    for (int level = 0; level < 40; level++) {
        std::vector<uint8_t> wrap;
        // count=1, size = 4 (header) + 3 (value entry) + body.size()
        const uint16_t inner_size = static_cast<uint16_t>(4 + 3 + body.size());
        append_le16(wrap, 1);
        append_le16(wrap, inner_size);
        wrap.push_back(0x02);  // small array type
        append_le16(wrap, 7);  // nested body offset relative to data_start
        wrap.insert(wrap.end(), body.begin(), body.end());
        body = wrap;
    }
    std::vector<uint8_t> doc = {0x02};
    doc.insert(doc.end(), body.begin(), body.end());
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsOpaqueNonDecimal) {
    // Opaque type 0x0F with mysql_type 16 (BIT): unsupported → refuse.
    std::vector<uint8_t> payload = {16, 1, 0, 0, 0x80, 0x00};
    auto doc = build_array_with_stored(0x0F, payload);
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsKeyOutOfBounds) {
    auto doc = build_small_object({{"k", {0x04, 1}}});
    doc[5] = 0xF0;  // key offset → beyond end
    doc[6] = 0x7F;
    const uint16_t total = static_cast<uint16_t>(doc.size() - 1);
    doc[3] = static_cast<uint8_t>(total & 0xFF);
    doc[4] = static_cast<uint8_t>((total >> 8) & 0xFF);
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsZeroLengthKey) {
    auto doc = build_small_object({{"k", {0x04, 1}}});
    doc[7] = 0;  // key_length = 0
    doc[8] = 0;
    const uint16_t total = static_cast<uint16_t>(doc.size() - 1);
    doc[3] = static_cast<uint8_t>(total & 0xFF);
    doc[4] = static_cast<uint8_t>((total >> 8) & 0xFF);
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, RejectsNestedSizeOverflow) {
    // Nested object claims size beyond parent's bounds.
    auto inner = build_small_object({{"x", {0x04, 1}}});
    std::vector<uint8_t> inner_body(inner.begin() + 1, inner.end());
    // Corrupt nested declared size to huge value.
    inner_body[2] = 0xFF;
    inner_body[3] = 0x7F;

    std::vector<uint8_t> doc;
    doc.push_back(0x00);
    append_le16(doc, 1);
    const size_t size_pos = doc.size();
    append_le16(doc, 0);
    const size_t ke_pos = doc.size();
    append_le16(doc, 0);
    append_le16(doc, 1);
    doc.push_back(0x00);
    const size_t ve_off = doc.size();
    append_le16(doc, 0);
    const size_t key_off = doc.size() - 1;
    doc.push_back('k');
    const size_t val_off = doc.size() - 1;
    doc.insert(doc.end(), inner_body.begin(), inner_body.end());
    doc[ke_pos] = static_cast<uint8_t>(key_off & 0xFF);
    doc[ke_pos + 1] = static_cast<uint8_t>((key_off >> 8) & 0xFF);
    doc[ve_off] = static_cast<uint8_t>(val_off & 0xFF);
    doc[ve_off + 1] = static_cast<uint8_t>((val_off >> 8) & 0xFF);
    const uint16_t total = static_cast<uint16_t>(doc.size() - 1);
    doc[size_pos] = static_cast<uint8_t>(total & 0xFF);
    doc[size_pos + 1] = static_cast<uint8_t>((total >> 8) & 0xFF);

    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), UNPARSABLE);
}

TEST(BinaryJsonDecode, SignedInlineInt16IsSignExtended) {
    // INT16 inline value 0xFFFF must decode as -1, not 65535.
    auto doc = build_small_array({{0x05, 0xFFFF}});
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), "[-1]");
}

TEST(BinaryJsonDecode, StoredUint32KeepsUnsigned) {
    auto doc = build_array_with_stored(0x08, le32_bytes(4000000000u));
    EXPECT_EQ(decode_json_binary(doc.data(), 0, doc.size()), "[4000000000]");
}

TEST(BinaryJsonDecode, InvalidUtf8ReplacedNotDropped) {
    std::vector<uint8_t> payload = {4, 'a', 0xFF, 0xFE, 'b'};
    auto doc = build_array_with_stored(0x0C, payload);
    const std::string out = decode_json_binary(doc.data(), 0, doc.size());
    // Well-formed JSON, invalid bytes replaced by U+FFFD, never silently dropped.
    EXPECT_NE(out, UNPARSABLE);
    EXPECT_EQ(out.find("\xEF\xBF\xBD"), out.find('a') + 1);
    EXPECT_NE(out.find("\xEF\xBF\xBD", out.find('a') + 4), std::string::npos);
    EXPECT_EQ(out.back(), ']');
}

} // namespace
