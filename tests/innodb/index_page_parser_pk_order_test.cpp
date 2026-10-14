// Clustered-leaf key-order guard regression test.
//
// Defect: a bit-flip in the first byte of a signed BIGINT pk (0x80 -> 0x81)
// left a leaf record's chain structure valid while fabricating pk
// 72057594037941770 out of row 13834. The checksum gate could not catch it
// (the validator does not model this 5.6 tablespace), and a clean duplicate
// page elsewhere supplied the true row, yielding 20001 rows instead of 20000.
//
// Fix: in strict schema-aware mode with a fixed-size integer pk, the record
// chain's non-decreasing key order is enforced; an order-violating record is
// dropped (or the ambiguous pair) instead of exported.

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "dbrecovery/innodb/index_page_parser.hpp"

namespace {

using dbrecovery::innodb::IndexColumnDef;
using dbrecovery::innodb::InnoDBIndexParser;

constexpr size_t PAGE_SIZE = 16384;
constexpr uint16_t INFIMUM_ORIGIN = 99;

// Big-endian write helpers
void be16(std::vector<uint8_t>& p, size_t off, uint16_t v) {
    p[off] = static_cast<uint8_t>(v >> 8);
    p[off + 1] = static_cast<uint8_t>(v & 0xFF);
}
void be32(std::vector<uint8_t>& p, size_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) p[off + i] = static_cast<uint8_t>((v >> (8 * (3 - i))) & 0xFF);
}
void be64(std::vector<uint8_t>& p, size_t off, uint64_t v) {
    for (int i = 0; i < 8; i++) p[off + i] = static_cast<uint8_t>((v >> (8 * (7 - i))) & 0xFF);
}

// Write a 5-byte COMPACT record header at `hdr_off`.
// bits layout (24b): instant(1)|version(1)|deleted(1)|min_rec(1)|n_owned(4)|
//                    heap_no(13)|rec_type(3) | next_offset(16s)
void write_rec_header(std::vector<uint8_t>& p, size_t hdr_off,
                      bool deleted, uint16_t heap_no, int16_t next_offset) {
    uint32_t bits = 0;
    if (deleted) bits |= 1u << 21;
    bits |= static_cast<uint32_t>(0) << 16;               // n_owned = 0
    bits |= (heap_no & 0x1FFF) << 3;
    bits |= 0;                                            // rec_type conventional
    p[hdr_off]     = static_cast<uint8_t>((bits >> 16) & 0xFF);
    p[hdr_off + 1] = static_cast<uint8_t>((bits >> 8) & 0xFF);
    p[hdr_off + 2] = static_cast<uint8_t>(bits & 0xFF);
    be16(p, hdr_off + 3, static_cast<uint16_t>(next_offset));
}

// Schema: id BIGINT pk (type 9), signed_value INT (type 4), name VARCHAR (type 16)
std::vector<IndexColumnDef> make_columns() {
    IndexColumnDef pk;
    pk.name = "id";
    pk.col_type = 9;
    pk.is_nullable = false;
    pk.is_unsigned = false;
    pk.physical_pos = 1;
    pk.ordinal_position = 1;
    pk.col_length = 8;

    IndexColumnDef sv;
    sv.name = "signed_value";
    sv.col_type = 4;
    sv.is_nullable = false;
    sv.is_unsigned = false;
    sv.physical_pos = 2;
    sv.ordinal_position = 2;
    sv.col_length = 4;

    IndexColumnDef name;
    name.name = "name";
    name.col_type = 16;
    name.is_nullable = true;
    name.is_unsigned = false;
    name.physical_pos = 3;
    name.ordinal_position = 3;
    name.col_length = 16;
    name.mb_minlen = 1;
    name.mb_maxlen = 1;
    name.char_set_id = 45;

    return {pk, sv, name};
}

// Build one user record body: [varchar size byte(s)][null bitmap] are NOT part
// of body — they're before the header. Body: pk(8) trx(6) rollptr(7) sv(4)
// name(N)
struct RecSpec {
    uint64_t pk_storage;   // stored 8 bytes (sign-flipped encoding)
    int32_t sv;
    std::string name;
};

// Lay out records into the page; returns origin offsets of user records.
std::vector<size_t> write_user_records(std::vector<uint8_t>& page,
                                       size_t& cursor,
                                       const std::vector<RecSpec>& specs,
                                       bool varchar_not_null,
                                       uint32_t n_pk_cols = 1) {
    std::vector<size_t> origins;
    for (size_t r = 0; r < specs.size(); ++r) {
        const auto& s = specs[r];
        // variable-size bytes + null bitmap precede the 5B header.
        // name is VARCHAR: 1 length byte (if not null); nullable cols: 1 bitmap byte.
        const uint8_t name_len = static_cast<uint8_t>(s.name.size());
        // layout at header_start-2: [name_len][null_bitmap][header5][body]
        const size_t hdr_off = cursor + 2;
        const size_t origin = hdr_off + 5;
        // var-size bytes read backward from pos=hdr_off: first read is
        // null bitmap (1 byte at hdr_off-1), then name size (1 byte at hdr_off-2).
        page[hdr_off - 1] = 0x00;              // null bitmap: name not null
        page[hdr_off - 2] = name_len;          // name length
        // Record layout is [pk fields][trx_id][roll_ptr][remaining fields].
        // The normal fixtures use one BIGINT pk; composite fixtures treat the
        // INT signed_value field as the second PK field and omit system bytes
        // until after it.
        be64(page, origin, s.pk_storage);
        const size_t system_offset = n_pk_cols == 2 ? origin + 12 : origin + 8;
        if (n_pk_cols == 2) {
            be32(page, origin + 8, static_cast<uint32_t>(s.sv) ^ 0x80000000u);
        }
        // trx_id 6B, roll_ptr 7B: arbitrary but fixed
        for (int i = 0; i < 13; i++) page[system_offset + i] = static_cast<uint8_t>(0x10 + i);
        if (n_pk_cols != 2) {
            // signed_value INT stored as value ^ 0x80000000 (sign-flip encoding)
            be32(page, origin + 21, static_cast<uint32_t>(s.sv) ^ 0x80000000u);
        }
        const size_t name_offset = origin + 25;
        std::memcpy(&page[name_offset], s.name.data(), s.name.size());

        const size_t rec_end = name_offset + s.name.size();
        write_rec_header(page, hdr_off, false, static_cast<uint16_t>(2 + r),
                         /*next_offset=*/0);  // patched below
        origins.push_back(origin);
        cursor = rec_end;
    }
    // Patch next offsets: each record's next = (next_origin - this_origin).
    for (size_t r = 0; r + 1 < origins.size(); ++r) {
        const size_t hdr_off = origins[r] - 5;
        const int16_t next = static_cast<int16_t>(origins[r + 1] - origins[r]);
        be16(page, hdr_off + 3, static_cast<uint16_t>(next));
    }
    // Last user record points back to SUPREMUM (origin = INFIMUM_ORIGIN + 13+8).
    if (!origins.empty()) {
        const size_t hdr_off = origins.back() - 5;
        const uint16_t supremum_origin = INFIMUM_ORIGIN + 8 + 13;  // 99+21=120
        const int16_t next = static_cast<int16_t>(supremum_origin) -
                             static_cast<int16_t>(origins.back());
        be16(page, hdr_off + 3, static_cast<uint16_t>(next));
    }
    return origins;
}

// Build a valid COMPACT leaf INDEX page containing the given user records.
std::vector<uint8_t> build_leaf_page(const std::vector<RecSpec>& specs,
                                     uint64_t index_id,
                                     uint32_t n_pk_cols = 1) {
    std::vector<uint8_t> page(PAGE_SIZE, 0);

    // FIL header: page_no (4), prev(8)/next(12) none, LSN(16), type(24)=INDEX,
    // flush lsn(26), space_id(34)
    be32(page, 4, 88);
    be32(page, 8, 0xFFFFFFFFu);
    be32(page, 12, 0xFFFFFFFFu);
    be64(page, 16, 3555714);
    be16(page, 24, 0x45BF);            // FIL_PAGE_INDEX
    be32(page, 34, 6);                 // space id

    // System records: infimum (origin 99) and supremum (origin 120).
    // Infimum record: 5B hdr at 94 + "infimum\0" (8B). Supremum: 5B hdr at 115
    // + "supremum" (8B).
    std::memcpy(&page[INFIMUM_ORIGIN], "infimum", 8);
    std::memcpy(&page[INFIMUM_ORIGIN + 21], "supremum", 8);
    // infimum header (heap_no=0, next = first user record origin - 99)
    // filled after records laid out.

    // PAGE header at 38
    const size_t records_start = INFIMUM_ORIGIN + 21 + 8;  // after supremum
    size_t cursor = records_start;
    auto origins = write_user_records(page, cursor, specs, true, n_pk_cols);

    const uint16_t heap_top = static_cast<uint16_t>(cursor);
    const uint16_t n_recs = static_cast<uint16_t>(specs.size());
    const uint16_t n_heap = static_cast<uint16_t>(specs.size() + 2) | 0x8000;

    // PAGE_N_DIR_SLOTS=2, HEAP_TOP, N_HEAP (bit15 = compact), N_RECS, LEVEL=0,
    // INDEX_ID
    be16(page, 38, 2);
    be16(page, 40, heap_top);
    be16(page, 42, n_heap);
    be16(page, 44, 0);              // page_free
    be16(page, 46, 0);              // garbage
    be16(page, 54, n_recs);
    be16(page, 64, 0);              // level 0 = leaf
    be64(page, 66, index_id);

    // Infimum header at 94: heap_no=0, next -> first user record
    if (!origins.empty()) {
        write_rec_header(page, 94, false, 0,
                         static_cast<int16_t>(origins[0] - INFIMUM_ORIGIN));
    }
    // Supremum header at 115: heap_no=1, next=0
    write_rec_header(page, 115, false, 1, 0);

    // FIL trailer: low4 lsn at ps-4 (matches FIL_PAGE_LSN low4)
    be32(page, PAGE_SIZE - 4, 3555714 & 0xFFFFFFFFu);
    // old checksum field at ps-8: leave 0 (gate not under test)
    return page;
}

uint64_t pk_storage(int64_t v) {
    return static_cast<uint64_t>(v) ^ 0x8000000000000000ULL;
}

// ---------------------------------------------------------------------------

TEST(PkOrderGuard, DropsLeadingRecordCorruptedAboveSuccessor) {
    // This is the real defect shape: the first record's pk byte is flipped
    // upward (0x80..01 -> 0x81..ff), so its key now exceeds every successor.
    // Truth order is 1,2,3; record 0 reads 0x81FF.. and record 1 reads 0x80..02,
    // so 0x81FF.. > 0x80..02 is a violation provable at the leading edge.
    std::vector<RecSpec> specs = {
        {0x81FFFFFFFFFFFFFFull, -10, "b10"},   // corrupted: was pk 2
        {pk_storage(3), -3, "c3"},
        {pk_storage(4), -4, "d4"},
    };
    auto page = build_leaf_page(specs, /*index_id=*/22);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);

    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].fields[0].int_value, 3);
    EXPECT_EQ(records[1].fields[0].int_value, 4);
    EXPECT_EQ(parser.key_order_violations_dropped(), 1u);
    EXPECT_EQ(parser.get_stats().key_order_violations_dropped, 1u);
}

TEST(PkOrderGuard, KeepsSignedBigintMinimumAtLeadingEdge) {
    // INT64_MIN encodes to all-zero stored bytes.  It is the smallest valid
    // signed BIGINT key and must survive the leading edge of a leaf chain.
    std::vector<RecSpec> specs = {
        {pk_storage(INT64_MIN), -10, "min"},
        {pk_storage(-1), -1, "neg1"},
        {pk_storage(1), 1, "one"},
    };
    auto page = build_leaf_page(specs, 22);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].fields[0].int_value, INT64_MIN);
    EXPECT_EQ(records[1].fields[0].int_value, -1);
    EXPECT_EQ(records[2].fields[0].int_value, 1);
    EXPECT_EQ(parser.key_order_violations_dropped(), 0u);
}

TEST(PkOrderGuard, KeepsMonotonicChain) {
    std::vector<RecSpec> specs = {
        {pk_storage(1), -1, "a1"},
        {pk_storage(2), -2, "b2"},
        {pk_storage(3), -3, "c3"},
    };
    auto page = build_leaf_page(specs, 22);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(parser.key_order_violations_dropped(), 0u);
}

TEST(PkOrderGuard, DoesNotFireOutsideStrictMode) {
    std::vector<RecSpec> specs = {
        {pk_storage(1), -1, "a1"},
        {pk_storage(10), -10, "b10"},
        {pk_storage(3), -3, "c3"},
    };
    auto page = build_leaf_page(specs, 22);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 1);
    parser.set_recover_deleted(false);
    // strict off: guard must not drop rows (non-strict callers accept
    // brute-force candidates anyway).
    auto records = parser.parse_page(page.data(), page.size(), 88);
    EXPECT_EQ(parser.key_order_violations_dropped(), 0u);
    EXPECT_EQ(records.size(), 3u);
}

TEST(PkOrderGuard, DoesNotFireForNonIntegerPk) {
    // pk column declared fixed-length BINARY — guard must stay off even in
    // strict mode (order for non-integer pk types is not provable from raw
    // bytes alone).
    std::vector<RecSpec> specs = {
        {pk_storage(1), -1, "a1"},
        {pk_storage(10), -10, "b10"},
        {pk_storage(3), -3, "c3"},
    };
    auto page = build_leaf_page(specs, 22);

    auto cols = make_columns();
    cols[0].col_type = 29;      // STRING/BINARY family: not an integer type
    cols[0].is_binary = true;
    cols[0].col_length = 8;
    cols[0].mb_minlen = 1;
    cols[0].mb_maxlen = 1;

    InnoDBIndexParser parser;
    parser.set_columns(cols, false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    EXPECT_EQ(parser.key_order_violations_dropped(), 0u);
    EXPECT_EQ(records.size(), 3u);
}

TEST(PkOrderGuard, UnsignedBigintPk) {
    // Unsigned BIGINT: stored raw BE — order preserved. Leading record is
    // corrupted above its successor (0x81FF.. vs 0x..03).
    std::vector<RecSpec> specs = {
        {0x81FFFFFFFFFFFFFFull, -10, "b10"},   // corrupted
        {3, -3, "c3"},
        {4, -4, "d4"},
    };
    auto page = build_leaf_page(specs, 22);

    auto cols = make_columns();
    cols[0].is_unsigned = true;

    InnoDBIndexParser parser;
    parser.set_columns(cols, false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].fields[0].str_value, "3");
    EXPECT_EQ(records[1].fields[0].str_value, "4");
    EXPECT_EQ(parser.key_order_violations_dropped(), 1u);
    EXPECT_EQ(parser.get_stats().key_order_violations_dropped, 1u);
}

TEST(PkOrderGuard, AmbiguousViolationDropsBothSides) {
    // 1, 10, 3 leaves two equally plausible explanations: dropping 10 yields
    // 1 <= 3, while dropping 3 yields 1 <= 10. The guard therefore makes a
    // bounded loss and removes both sides rather than exporting a candidate
    // it cannot prove.
    std::vector<RecSpec> specs = {
        {pk_storage(1), -1, "a1"},
        {pk_storage(10), -10, "b10"},
        {pk_storage(3), -3, "c3"},
    };
    auto page = build_leaf_page(specs, 22);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].fields[0].int_value, 1);
    EXPECT_EQ(parser.key_order_violations_dropped(), 2u);
}

TEST(PkOrderGuard, DoesNotFireWithoutSelectedClusteredIndex) {
    std::vector<RecSpec> specs = {
        {pk_storage(1), -1, "a1"},
        {pk_storage(10), -10, "b10"},
        {pk_storage(3), -3, "c3"},
    };
    auto page = build_leaf_page(specs, 22);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 1);
    parser.set_strict(true);
    parser.set_recover_deleted(false);

    EXPECT_EQ(parser.parse_page(page.data(), page.size(), 88).size(), 3u);
    EXPECT_EQ(parser.key_order_violations_dropped(), 0u);
}

TEST(PkOrderGuard, DoesNotFireForMismatchedClusteredIndex) {
    std::vector<RecSpec> specs = {
        {pk_storage(1), -1, "a1"},
        {pk_storage(10), -10, "b10"},
        {pk_storage(3), -3, "c3"},
    };
    auto page = build_leaf_page(specs, 22);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(23);
    parser.set_recover_deleted(false);

    EXPECT_EQ(parser.parse_page(page.data(), page.size(), 88).size(), 3u);
    EXPECT_EQ(parser.key_order_violations_dropped(), 0u);
}

TEST(PkOrderGuard, DoesNotFireForImplicitRowId) {
    std::vector<RecSpec> specs = {
        {pk_storage(1), -1, "a1"},
        {pk_storage(10), -10, "b10"},
        {pk_storage(3), -3, "c3"},
    };
    auto page = build_leaf_page(specs, 22);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), true, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    EXPECT_EQ(parser.parse_page(page.data(), page.size(), 88).size(), 3u);
    EXPECT_EQ(parser.key_order_violations_dropped(), 0u);
}

TEST(PkOrderGuard, CompositeKeyUsesVerifiedFieldOrder) {
    // The first user field is BIGINT id; the second is INT signed_value. For
    // equal ids the second verified key field decides the bytewise ordering.
    std::vector<RecSpec> specs = {
        {pk_storage(1), -1, "a1"},
        {pk_storage(1), 10, "b10"},
        {pk_storage(1), 3, "c3"},
        {pk_storage(1), 4, "d4"},
    };
    auto page = build_leaf_page(specs, 22, /*n_pk_cols=*/2);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 2);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].fields[1].int_value, -1);
    EXPECT_EQ(records[1].fields[1].int_value, 3);
    EXPECT_EQ(records[2].fields[1].int_value, 4);
    EXPECT_EQ(parser.key_order_violations_dropped(), 1u);
}

TEST(PkOrderGuard, SignedBigintOrderingCrossesZero) {
    std::vector<RecSpec> specs = {
        {pk_storage(-5), -1, "negative"},
        {pk_storage(0), 0, "zero"},
        {pk_storage(6), 1, "positive"},
    };
    auto page = build_leaf_page(specs, 22);

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    EXPECT_EQ(parser.parse_page(page.data(), page.size(), 88).size(), 3u);
    EXPECT_EQ(parser.key_order_violations_dropped(), 0u);
}

TEST(PkOrderGuard, InteriorPageIsNotKeyOrdered) {
    // The guard must not run on interior pages (level != 0): their record
    // bodies are node pointers, not schema rows.
    std::vector<RecSpec> specs = {
        {pk_storage(1), -1, "a1"},
        {pk_storage(10), -10, "b10"},
        {pk_storage(3), -3, "c3"},
    };
    auto page = build_leaf_page(specs, 22);
    // Force PAGE_LEVEL = 1 (interior).
    page[64] = 0x00;
    page[65] = 0x01;

    InnoDBIndexParser parser;
    parser.set_columns(make_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    parser.parse_page(page.data(), page.size(), 88);
    EXPECT_EQ(parser.key_order_violations_dropped(), 0u);
}

TEST(PkOrderGuard, CompositeIntVarcharExtractsSystemFieldsAfterSecondPk) {
    // Tests schema with composite PK: (id_a INT, id_b VARCHAR(16))
    // Verifies that DB_TRX_ID (6B) and DB_ROLL_PTR (7B) are skipped only AFTER
    // the second primary key column (id_b), so non-PK columns are not shifted.
    IndexColumnDef col_a;
    col_a.name = "id_a";
    col_a.col_type = 4; // INT
    col_a.is_nullable = false;
    col_a.physical_pos = 1;
    col_a.ordinal_position = 1;
    col_a.col_length = 4;

    IndexColumnDef col_b;
    col_b.name = "id_b";
    col_b.col_type = 16; // VARCHAR
    col_b.is_nullable = false;
    col_b.physical_pos = 2;
    col_b.ordinal_position = 2;
    col_b.col_length = 16;
    col_b.mb_minlen = 1;
    col_b.mb_maxlen = 1;

    IndexColumnDef col_val;
    col_val.name = "c_val";
    col_val.col_type = 4; // INT
    col_val.is_nullable = false;
    col_val.physical_pos = 3;
    col_val.ordinal_position = 3;
    col_val.col_length = 4;

    std::vector<IndexColumnDef> cols = {col_a, col_b, col_val};

    // Lay out one leaf page manually
    std::vector<uint8_t> page(PAGE_SIZE, 0);
    be32(page, 4, 88);
    be16(page, 24, 0x45BF); // FIL_PAGE_INDEX
    std::memcpy(&page[INFIMUM_ORIGIN], "infimum", 8);
    std::memcpy(&page[INFIMUM_ORIGIN + 21], "supremum", 8);

    // Record: id_a=100 (4B, sign-flipped), id_b="key_abc" (7B), trx(6B)+roll(7B), c_val=999 (4B)
    size_t cursor = INFIMUM_ORIGIN + 21 + 8;
    size_t hdr_off = cursor + 1; // 1 byte varchar len before header
    size_t origin = hdr_off + 5;
    page[hdr_off - 1] = 7; // id_b length = 7

    // id_a (INT 100 ^ 0x80000000)
    be32(page, origin, 100 ^ 0x80000000u);
    // id_b ("key_abc", 7 bytes)
    std::memcpy(&page[origin + 4], "key_abc", 7);
    // DB_TRX_ID (6B) + DB_ROLL_PTR (7B) starting at origin + 4 + 7 = origin + 11
    for (int i = 0; i < 13; i++) page[origin + 11 + i] = 0xAA;
    // c_val (INT 999 ^ 0x80000000) at origin + 11 + 13 = origin + 24
    be32(page, origin + 24, 999 ^ 0x80000000u);

    size_t rec_end = origin + 28;
    write_rec_header(page, hdr_off, false, 2, 0);
    // Point to supremum
    const uint16_t supremum_origin = INFIMUM_ORIGIN + 8 + 13;
    be16(page, hdr_off + 3, static_cast<uint16_t>(supremum_origin - origin));

    // Page header
    be16(page, 38, 2);
    be16(page, 40, static_cast<uint16_t>(rec_end));
    be16(page, 42, 3 | 0x8000);
    be16(page, 54, 1);
    be16(page, 64, 0); // leaf
    be64(page, 66, 22);

    // Infimum next
    write_rec_header(page, 94, false, 0, static_cast<int16_t>(origin - INFIMUM_ORIGIN));
    write_rec_header(page, 115, false, 1, 0);

    InnoDBIndexParser parser;
    parser.set_columns(cols, false, 2); // 2 PK columns
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    ASSERT_EQ(records.size(), 1u);
    ASSERT_EQ(records[0].fields.size(), 3u);
    EXPECT_EQ(records[0].fields[0].int_value, 100);
    EXPECT_EQ(records[0].fields[1].str_value, "key_abc");
    EXPECT_EQ(records[0].fields[2].int_value, 999);
}

// ---------------------------------------------------------------------------
// Off-page LOB pointer fail-closed guard
//
// A DYNAMIC/COMPACT record keeps only a 20-byte pointer when a LOB column
// overflows, and this carving path never reassembles the external chain.
// Exporting such a row would write the pointer bytes as the column value, so
// the record must be dropped instead.
// ---------------------------------------------------------------------------

// id BIGINT pk + c_blob TINY_BLOB that is big: InnoDB uses the 2-byte length
// prefix for BLOB/TEXT/JSON once the value exceeds 127 bytes (see
// build_index_column), and the parser relies on that flag.
std::vector<IndexColumnDef> make_blob_columns() {
    IndexColumnDef pk;
    pk.name = "id";
    pk.col_type = 9;
    pk.is_nullable = false;
    pk.physical_pos = 1;
    pk.ordinal_position = 1;
    pk.col_length = 8;

    IndexColumnDef blob;
    blob.name = "c_blob";
    blob.col_type = 24;              // TINY_BLOB
    blob.is_nullable = true;
    blob.physical_pos = 2;
    blob.ordinal_position = 2;
    blob.col_length = 255;
    blob.mb_minlen = 1;
    blob.mb_maxlen = 1;
    blob.is_big = true;
    blob.is_binary = true;
    return {pk, blob};
}

// Leaf page whose c_blob column carries either a 200-byte inline payload or an
// off-page pointer (encoded size 20000, past the COMPACT off-page threshold of
// 17172). Length prefixes follow InnoDB's is_big rule: one byte up to 127, then
// the (128 + len>>8, len&0xFF) two-byte encoding.
std::vector<uint8_t> build_blob_leaf_page(const std::vector<int64_t>& pks,
                                          bool off_page,
                                          uint32_t blob_len = 200) {
    std::vector<uint8_t> page(PAGE_SIZE, 0);
    be32(page, 4, 88);
    be32(page, 8, 0xFFFFFFFFu);
    be32(page, 12, 0xFFFFFFFFu);
    be64(page, 16, 3555714);
    be16(page, 24, 0x45BF);          // FIL_PAGE_INDEX
    be32(page, 34, 6);
    std::memcpy(&page[INFIMUM_ORIGIN], "infimum", 8);
    std::memcpy(&page[INFIMUM_ORIGIN + 21], "supremum", 8);

    const size_t body_len = off_page ? 20 : blob_len;
    const uint32_t encoded_size = off_page ? 20000u : blob_len;
    std::vector<uint8_t> prefix;
    if (encoded_size > 127) {
        prefix.push_back(static_cast<uint8_t>(128 + (encoded_size >> 8)));
        prefix.push_back(static_cast<uint8_t>(encoded_size & 0xFF));
    } else {
        prefix.push_back(static_cast<uint8_t>(encoded_size));
    }

    std::vector<size_t> origins;
    size_t cursor = INFIMUM_ORIGIN + 21 + 8;
    for (size_t r = 0; r < pks.size(); ++r) {
        const size_t hdr_off = cursor + prefix.size() + 1;  // +1 null-bitmap byte
        const size_t origin = hdr_off + 5;
        page[hdr_off - 1] = 0x00;                           // c_blob is not NULL
        for (size_t i = 0; i < prefix.size(); ++i) {
            page[hdr_off - 2 - i] = prefix[i];              // byte1 nearest header
        }
        be64(page, origin, pk_storage(pks[r]));
        for (int i = 0; i < 13; i++) page[origin + 8 + i] = static_cast<uint8_t>(0x20 + i);
        for (size_t i = 0; i < body_len; i++) {
            page[origin + 21 + i] = static_cast<uint8_t>(0xA0 + (i % 16));
        }
        write_rec_header(page, hdr_off, false, static_cast<uint16_t>(2 + r), 0);
        origins.push_back(origin);
        cursor = origin + 21 + body_len;
    }
    for (size_t r = 0; r + 1 < origins.size(); ++r) {
        be16(page, origins[r] - 5 + 3, static_cast<uint16_t>(origins[r + 1] - origins[r]));
    }
    const uint16_t supremum_origin = INFIMUM_ORIGIN + 8 + 13;
    be16(page, origins.back() - 5 + 3,
         static_cast<uint16_t>(supremum_origin - origins.back()));

    be16(page, 38, 2);
    be16(page, 40, static_cast<uint16_t>(cursor));
    be16(page, 42, static_cast<uint16_t>(pks.size() + 2) | 0x8000);
    be16(page, 44, 0);
    be16(page, 46, 0);
    be16(page, 54, static_cast<uint16_t>(pks.size()));
    be16(page, 64, 0);
    be64(page, 66, 22);
    write_rec_header(page, 94, false, 0, static_cast<int16_t>(origins[0] - INFIMUM_ORIGIN));
    write_rec_header(page, 115, false, 1, 0);
    be32(page, PAGE_SIZE - 4, 3555714 & 0xFFFFFFFFu);
    return page;
}

TEST(OffPageFailClosed, DropsRecordWhoseLobStillPointsOffPage) {
    auto page = build_blob_leaf_page({1, 2}, /*off_page=*/true);

    InnoDBIndexParser parser;
    parser.set_columns(make_blob_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(parser.overflow_pointer_records_dropped(), 2u);
    EXPECT_EQ(parser.get_stats().overflow_pointer_records_dropped, 2u);
}

TEST(OffPageFailClosed, KeepsInlineLobRowsAtFullLength) {
    auto page = build_blob_leaf_page({1, 2}, /*off_page=*/false);

    InnoDBIndexParser parser;
    parser.set_columns(make_blob_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    ASSERT_EQ(records.size(), 2u);
    ASSERT_EQ(records[0].fields.size(), 2u);
    EXPECT_FALSE(records[0].fields[1].is_overflow);
    // 200 bytes: an is_big column above 127 must take the two-byte length
    // prefix, not truncate to 128.
    EXPECT_EQ(records[0].fields[1].raw_bytes.size(), 200u);
    EXPECT_EQ(records[1].fields[1].raw_bytes.size(), 200u);
    EXPECT_EQ(parser.overflow_pointer_records_dropped(), 0u);
}

TEST(OffPageFailClosed, KeepsTrailingZeroLengthColumnAsEmpty) {
    // The last column of these records has length 0, so the record ends exactly
    // where that column starts.  A decode loop bounded by `pos < data_end` never
    // visits it and the gap-filling default is NULL, which is how 48 empty
    // c_longblob cells shipped as NULL while the sealed truth held ''.
    auto page = build_blob_leaf_page({1, 2}, /*off_page=*/false, /*blob_len=*/0);

    InnoDBIndexParser parser;
    parser.set_columns(make_blob_columns(), false, 1);
    parser.set_strict(true);
    parser.set_target_index_id(22);
    parser.set_recover_deleted(false);

    auto records = parser.parse_page(page.data(), page.size(), 88);
    ASSERT_EQ(records.size(), 2u);
    for (const auto& rec : records) {
        ASSERT_EQ(rec.fields.size(), 2u);
        EXPECT_FALSE(rec.fields[1].is_null) << "empty trailing column must not be NULL";
        EXPECT_TRUE(rec.fields[1].raw_bytes.empty());
        EXPECT_EQ(rec.fields[1].type,
                  dbrecovery::innodb::IndexFieldValue::Type::BINARY_VAL);
    }
    EXPECT_EQ(parser.overflow_pointer_records_dropped(), 0u);
}

TEST(OffPageFailClosed, FormattersSuppressUnreconstructedPointerBytes) {
    dbrecovery::innodb::IndexRecord record;
    dbrecovery::innodb::IndexFieldValue field;
    field.type = dbrecovery::innodb::IndexFieldValue::Type::OVERFLOW_PTR;
    field.is_overflow = true;
    record.fields.push_back(field);

    EXPECT_TRUE(record.has_unreconstructed_overflow());
    EXPECT_TRUE(record.to_csv_line().empty());
    EXPECT_TRUE(record.to_sql_values().empty());
}

} // namespace
