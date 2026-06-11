#!/bin/bash
# =============================================================================
# MySQL 5M Records Recovery Testing Script
# Tests various corruption scenarios and measures recovery success rate
# =============================================================================

set -e

# Configuration
TEST_DIR="/app/mysql_recovery_pro"
SOURCE_IBD="/var/lib/mysql/recovery_test/test_users.ibd"
SOURCE_MD5=$(md5sum "$SOURCE_IBD" | cut -d' ' -f1)

# Golden hashes from MySQL data
GOLDEN_COUNT=5500000
GOLDEN_SUM_ID=15125002750000
GOLDEN_MIN_ID=1
GOLDEN_MAX_ID=5500000

# Test results tracking
declare -A RESULTS
PASS_COUNT=0
FAIL_COUNT=0

# Output directory
OUTPUT_DIR="/tmp/recovery_test_$$"
mkdir -p "$OUTPUT_DIR"

echo "=========================================="
echo "MySQL 5M Records Recovery Testing"
echo "=========================================="
echo ""

# Helper function to log results
log_result() {
    local test_name="$1"
    local status="$2"
    local details="$3"
    
    if [ "$status" == "PASS" ]; then
        RESULTS["$test_name"]="PASS: $details"
        ((PASS_COUNT++))
        echo "[PASS] $test_name"
    else
        RESULTS["$test_name"]="FAIL: $details"
        ((FAIL_COUNT++))
        echo "[FAIL] $test_name"
    fi
    
    echo "       $details"
}

# Helper function to get MySQL counts after test
get_recovered_count() {
    local output_sql="$1"
    local count=0
    
    if [ -f "$output_sql" ]; then
        count=$(grep -c "^INSERT" "$output_sql" 2>/dev/null || echo 0)
    fi
    
    echo "$count"
}

# Helper function to get record ID ranges
get_recovered_id_ranges() {
    local output_sql="$1"
    
    if [ -f "$output_sql" ]; then
        local min_id=$(grep "VALUES" "$output_sql" | sed "s/VALUES (\([0-9]*\).*/\1/" | sort -n | head -1)
        local max_id=$(grep "VALUES" "$output_sql" | sed "s/VALUES (\([0-9]*\).*/\1/" | sort -n | tail -1)
        local count=$(grep -c "VALUES" "$output_sql" 2>/dev/null || echo 0)
        echo "$count $min_id $max_id"
    else
        echo "0 0 0"
    fi
}

# Helper function to calculate recovery rate
calc_recovery_rate() {
    local recovered="$1"
    local golden="$2"
    
    if [ "$golden" -eq 0 ]; then
        echo "0"
        return
    fi
    
    local rate=$((recovered * 100 / golden))
    echo "$rate"
}

# Test 1: Baseline - No corruption
echo "Test 1: Baseline Recovery (No Corruption)"
echo "-------------------------------------------"

# Get golden SQL from MySQL
mysql -e "SELECT * FROM recovery_test.test_users" --batch 2>/dev/null | \
    head -100000 > /tmp/golden_sample.txt
mysql -e "SELECT 'COUNT', COUNT(*), 'SUM_ID', SUM(id), 'MIN_ID', MIN(id), 'MAX_ID', MAX(id) FROM recovery_test.test_users" 2>/dev/null > /tmp/golden_stats.txt
cat /tmp/golden_stats.txt

# Copy IBD to test location
cp "$SOURCE_IBD" "$TEST_DIR/test_recover.ibd"

# Run recovery
/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --ibd="$TEST_DIR/test_recover.ibd" \
    --default-recovery \
    --format=sql \
    --output="$OUTPUT_DIR/test1_baseline" \
    2>&1

# Check results
TEST1_COUNT=$(get_recovered_count "$OUTPUT_DIR/test1_baseline/recovered.sql")
TEST1_RANGES=$(get_recovered_id_ranges "$OUTPUT_DIR/test1_baseline/recovered.sql")
TEST1_RECOVERED=$(echo $TEST1_RANGES | cut -d' ' -f1)
TEST1_MIN=$(echo $TEST1_RANGES | cut -d' ' -f2)
TEST1_MAX=$(echo $TEST1_RANGES | cut -d' ' -f3)
TEST1_RATE=$(calc_recovery_rate $TEST1_RECOVERED $GOLDEN_COUNT)

log_result "test1_baseline" "$([ $TEST1_RATE -ge 90 ] && echo 'PASS' || echo 'FAIL')" \
    "Recovered: $TEST1_RECOVERED/$GOLDEN_COUNT ($TEST1_RATE%), ID Range: $TEST1_MIN-$TEST1_MAX"

echo ""

# Test 2: Page header corruption
echo "Test 2: Page Header Corruption (10% pages)"
echo "-------------------------------------------"

# Create corrupted IBD with corrupt page headers
cp "$SOURCE_IBD" "$TEST_DIR/test_corrupt_headers.ibd"

# Corrupt page headers (every 10th page)
python3 << 'PYTHON'
import sys

def corrupt_page_header(ibd_path, corrupt_rate=10):
    """Corrupt page headers at corrupt_rate percentage"""
    with open(ibd_path, 'r+b') as f:
        import os
        page_size = 16384
        file_size = os.path.getsize(ibd_path)
        total_pages = file_size // page_size
        
        corrupt_count = 0
        for page_no in range(0, total_pages, corrupt_rate):
            # Go to page header position
            f.seek(page_no * page_size + 0)  # FIL_PAGE_TYPE position
            
            # Corrupt page type (write garbage)
            f.write(b'\xFF\xFF')  # Invalid page type
            
            corrupt_count += 1
        
        print(f"Corrupted {corrupt_count} page headers")

corrupt_page_header(sys.argv[1], 10)
PYTHON

/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --ibd="$TEST_DIR/test_corrupt_headers.ibd" \
    --default-recovery \
    --format=sql \
    --output="$OUTPUT_DIR/test2_corrupt_headers" \
    2>&1

TEST2_RANGES=$(get_recovered_id_ranges "$OUTPUT_DIR/test2_corrupt_headers/recovered.sql")
TEST2_RECOVERED=$(echo $TEST2_RANGES | cut -d' ' -f1)
TEST2_RATE=$(calc_recovery_rate $TEST2_RECOVERED $GOLDEN_COUNT)

log_result "test2_corrupt_headers" "$([ $TEST2_RATE -ge 90 ] && echo 'PASS' || echo 'FAIL')" \
    "Recovered: $TEST2_RECOVERED/$GOLDEN_COUNT ($TEST2_RATE%) with 10% page header corruption"

echo ""

# Test 3: B+Tree chain broken
echo "Test 3: B+Tree Next/Prev Pointer Corruption"
echo "--------------------------------------------"

cp "$SOURCE_IBD" "$TEST_DIR/test_corrupt_chain.ibd"

python3 << 'PYTHON'
import sys

def corrupt_chain_pointers(ibd_path, corrupt_rate=5):
    """Corrupt next_page/prev_page pointers"""
    with open(ibd_path, 'r+b') as f:
        import os
        page_size = 16384
        file_size = os.path.getsize(ibd_path)
        total_pages = file_size // page_size
        
        corrupt_count = 0
        for page_no in range(1, total_pages, corrupt_rate):
            # Go to page header position
            # FIL_PAGE_NEXT is at offset 26, FIL_PAGE_PREV at offset 22
            f.seek(page_no * page_size + 26)  # next_page
            
            # Corrupt pointer
            f.write(b'\xFF\xFF\xFF\xFF')  # Invalid pointer
            
            corrupt_count += 1
        
        print(f"Corrupted {corrupt_count} chain pointers")

corrupt_chain_pointers(sys.argv[1], 5)
PYTHON

/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --ibd="$TEST_DIR/test_corrupt_chain.ibd" \
    --default-recovery \
    --format=sql \
    --output="$OUTPUT_DIR/test3_corrupt_chain" \
    2>&1

TEST3_RANGES=$(get_recovered_id_ranges "$OUTPUT_DIR/test3_corrupt_chain/recovered.sql")
TEST3_RECOVERED=$(echo $TEST3_RANGES | cut -d' ' -f1)
TEST3_RATE=$(calc_recovery_rate $TEST3_RECOVERED $GOLDEN_COUNT)

log_result "test3_corrupt_chain" "$([ $TEST3_RATE -ge 90 ] && echo 'PASS' || echo 'FAIL')" \
    "Recovered: $TEST3_RECOVERED/$GOLDEN_COUNT ($TEST3_RATE%) with chain corruption"

echo ""

# Test 4: Record header corruption (info_bits, next_offset)
echo "Test 4: Record Header Corruption"
echo "---------------------------------"

cp "$SOURCE_IBD" "$TEST_DIR/test_corrupt_records.ibd"

python3 << 'PYTHON'
import sys

def corrupt_record_headers(ibd_path, corrupt_count=5000):
    """Corrupt record headers randomly"""
    with open(ibd_path, 'r+b') as f:
        import os
        page_size = 16384
        import random
        
        corrupt_count = 0
        # Read entire file
        f.seek(0, 2)
        file_size = f.tell()
        
        # Generate random positions (avoid page headers)
        corrupt_positions = []
        for _ in range(corrupt_count):
            pos = random.randint(1024, file_size - 100)
            # Align to 4 bytes for simplicity
            pos = (pos // 4) * 4
            corrupt_positions.append(pos)
        
        # Write corruption
        for pos in corrupt_positions:
            f.seek(pos)
            # Corrupt info_bits and next_offset
            f.write(b'\xAA\xAA\xAA\xAA\xAA')
            corrupt_count += 1
        
        print(f"Corrupted {len(corrupt_positions)} record headers")

corrupt_record_headers(sys.argv[1], 5000)
PYTHON

/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --ibd="$TEST_DIR/test_corrupt_records.ibd" \
    --default-recovery \
    --format=sql \
    --output="$OUTPUT_DIR/test4_corrupt_records" \
    2>&1

TEST4_RANGES=$(get_recovered_id_ranges "$OUTPUT_DIR/test4_corrupt_records/recovered.sql")
TEST4_RECOVERED=$(echo $TEST4_RANGES | cut -d' ' -f1)
TEST4_RATE=$(calc_recovery_rate $TEST4_RECOVERED $GOLDEN_COUNT)

log_result "test4_corrupt_records" "$([ $TEST4_RATE -ge 90 ] && echo 'PASS' || echo 'FAIL')" \
    "Recovered: $TEST4_RECOVERED/$GOLDEN_COUNT ($TEST4_RATE%) with record corruption"

echo ""

# Test 5: File truncation (simulating partial recovery)
echo "Test 5: File Truncation (Simulated)"
echo "------------------------------------"

cp "$SOURCE_IBD" "$TEST_DIR/test_truncated.ibd"

python3 << 'PYTHON'
import sys

def truncate_file(ibd_path, preserve_percent=85):
    """Truncate file to preserve_percent"""
    with open(ibd_path, 'r+b') as f:
        import os
        file_size = os.path.getsize(ibd_path)
        new_size = int(file_size * preserve_percent / 100)
        
        # Align to page size
        page_size = 16384
        new_size = (new_size // page_size) * page_size
        
        f.truncate(new_size)
        print(f"Truncated from {file_size} to {new_size} bytes ({preserve_percent}%)")

truncate_file(sys.argv[1], 85)
PYTHON

/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --ibd="$TEST_DIR/test_truncated.ibd" \
    --truncate-recovery \
    --format=sql \
    --output="$OUTPUT_DIR/test5_truncated" \
    2>&1

TEST5_RANGES=$(get_recovered_id_ranges "$OUTPUT_DIR/test5_truncated/recovered.sql")
TEST5_RECOVERED=$(echo $TEST5_RANGES | cut -d' ' -f1)
TEST5_RATE=$(calc_recovery_rate $TEST5_RECOVERED $GOLDEN_COUNT)

log_result "test5_truncated" "$([ $TEST5_RATE -ge 90 ] && echo 'PASS' || echo 'FAIL')" \
    "Recovered: $TEST5_RECOVERED/$GOLDEN_COUNT ($TEST5_RATE%) from 85% file"

echo ""

# Test 6: Random page corruption (10%)
echo "Test 6: Random Page Corruption (10%)"
echo "-------------------------------------"

cp "$SOURCE_IBD" "$TEST_DIR/test_random_pages.ibd"

python3 << 'PYTHON'
import sys
import random

def corrupt_random_pages(ibd_path, corrupt_percent=10):
    """Corrupt random pages"""
    with open(ibd_path, 'r+b') as f:
        import os
        page_size = 16384
        file_size = os.path.getsize(ibd_path)
        total_pages = file_size // page_size
        corrupt_count = int(total_pages * corrupt_percent / 100)
        
        pages_to_corrupt = random.sample(range(1, total_pages), corrupt_count)
        
        for page_no in pages_to_corrupt:
            pos = page_no * page_size
            
            # Corrupt various parts of page
            f.seek(pos + 0)  # Page header
            f.write(b'\xDE\xAD\xBE\xEF')  # Corrupt type
            
            f.seek(pos + 20)  # Some internal data
            f.write(b'\xFF' * 100)
        
        print(f"Corrupted {len(pages_to_corrupt)} random pages ({corrupt_percent}%)")

corrupt_random_pages(sys.argv[1], 10)
PYTHON

/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --ibd="$TEST_DIR/test_random_pages.ibd" \
    --default-recovery \
    --format=sql \
    --output="$OUTPUT_DIR/test6_random_pages" \
    2>&1

TEST6_RANGES=$(get_recovered_id_ranges "$OUTPUT_DIR/test6_random_pages/recovered.sql")
TEST6_RECOVERED=$(echo $TEST6_RANGES | cut -d' ' -f1)
TEST6_RATE=$(calc_recovery_rate $TEST6_RECOVERED $GOLDEN_COUNT)

log_result "test6_random_pages" "$([ $TEST6_RATE -ge 90 ] && echo 'PASS' || echo 'FAIL')" \
    "Recovered: $TEST6_RECOVERED/$GOLDEN_COUNT ($TEST6_RATE%) with 10% random page corruption"

echo ""

# Test 7: SDI metadata loss
echo "Test 7: SDI Metadata Corruption"
echo "--------------------------------"

cp "$SOURCE_IBD" "$TEST_DIR/test_sdi_loss.ibd"

python3 << 'PYTHON'
import sys

def corrupt_sdi_pages(ibd_path):
    """Corrupt SDI pages (page type 9971 = 0x270B)"""
    with open(ibd_path, 'r+b') as f:
        import os
        page_size = 16384
        file_size = os.path.getsize(ibd_path)
        total_pages = file_size // page_size
        
        corrupt_count = 0
        for page_no in range(total_pages):
            f.seek(page_no * page_size + 24)  # FIL_PAGE_TYPE position
            import struct
            page_type = struct.unpack('<H', f.read(2))[0]
            
            if page_type == 9971:  # FIL_PAGE_SDI
                # Corrupt SDI payload
                f.seek(page_no * page_size + 38)
                f.write(b'\x00' * 100)  # Corrupt SDI data
                corrupt_count += 1
                print(f"Corrupted SDI page {page_no}")
        
        print(f"Corrupted {corrupt_count} SDI pages")

corrupt_sdi_pages(sys.argv[1])
PYTHON

/app/mysql_recovery_pro/build/mysql_recovery_pro \
    --ibd="$TEST_DIR/test_sdi_loss.ibd" \
    --default-recovery \
    --format=sql \
    --output="$OUTPUT_DIR/test7_sdi_loss" \
    2>&1

TEST7_RANGES=$(get_recovered_id_ranges "$OUTPUT_DIR/test7_sdi_loss/recovered.sql")
TEST7_RECOVERED=$(echo $TEST7_RANGES | cut -d' ' -f1)
TEST7_RATE=$(calc_recovery_rate $TEST7_RECOVERED $GOLDEN_COUNT)

log_result "test7_sdi_loss" "$([ $TEST7_RATE -ge 90 ] && echo 'PASS' || echo 'FAIL')" \
    "Recovered: $TEST7_RECOVERED/$GOLDEN_COUNT ($TEST7_RATE%) after SDI corruption"

echo ""

# Summary
echo "=========================================="
echo "RECOVERY TESTING SUMMARY"
echo "=========================================="
echo ""

TOTAL_TESTS=$((PASS_COUNT + FAIL_COUNT))
OVERALL_RATE=0
if [ $TOTAL_TESTS -gt 0 ]; then
    OVERALL_RATE=$((PASS_COUNT * 100 / TOTAL_TESTS))
fi

echo "Tests Passed: $PASS_COUNT / $TOTAL_TESTS ($OVERALL_RATE%)"
echo ""

echo "Detailed Results:"
echo "-----------------"
for test_name in "${!RESULTS[@]}"; do
    echo "  $test_name: ${RESULTS[$test_name]}"
done

echo ""
echo "Output directory: $OUTPUT_DIR"

# Cleanup temporary IBD files
rm -f "$TEST_DIR"/*.ibd 2>/dev/null

echo ""
echo "Testing completed."
