#!/bin/bash
# =============================================================================
# dd-style Corruption Script for MySQL 5.6 IBD Files
# Author: jliang.fang (Global MySQL Kernel QA Expert)
# Purpose: Generate specific dd-style corruption scenarios using Bash/dd
# =============================================================================

set -e

# ==============================================================================
# Configuration
# ==============================================================================
PAGE_SIZE=${PAGE_SIZE:-16384}
CORRUPTION_DIR=${CORRUPTION_DIR:-./dd_corruption}
OUTPUT_DIR=${OUTPUT_DIR:-./corruption_output}
SEED=${SEED:-42}

# ==============================================================================
# Utility Functions
# ==============================================================================

usage() {
    echo "Usage: $0 <command> [options]"
    echo ""
    echo "Commands:"
    echo "  torn <file> <num_pages>        - Corrupt page headers (power loss sim)"
    echo "  dd <file> <page> <offset> <len> - dd arbitrary bytes"
    echo "  truncate <file> <percent>      - Truncate file"
    echo "  random <file> <num_bytes>      - Corrupt random bytes"
    echo "  all <file>                     - Run all scenarios"
    echo ""
    echo "Options:"
    echo "  PAGE_SIZE     Page size in bytes (default: 16384)"
    echo "  SEED          Random seed for reproducibility (default: 42)"
    exit 1
}

create_output_dir() {
    mkdir -p "$OUTPUT_DIR"
    mkdir -p "$CORRUPTION_DIR"
}

copy_original() {
    local src="$1"
    local dst="$OUTPUT_DIR/$(basename "$src" .ibd)_original.ibd"
    cp "$src" "$dst"
    echo "Original copied to: $dst"
    echo "$dst"
}

# ==============================================================================
# Corruption Scenarios
# ==============================================================================

# Scenario 1: Torn Pages (Power Loss Simulation)
# Simulates incomplete writes due to power failure
corrupt_torn_pages() {
    local file="$1"
    local num_pages="${2:-100}"
    
    echo "===Scenario: Torn Pages (Power Loss) ==="
    echo "Original: $file"
    
    # Create output file
    local output="$OUTPUT_DIR/torn_pages.ibd"
    cp "$file" "$output"
    
    # Get file size
    local file_size=$(stat -c%s "$file")
    local total_pages=$((file_size / PAGE_SIZE))
    
    # Corrupt first 40 bytes of selected pages (FIL header area)
    local corrupted=0
    for i in $(seq 1 $num_pages); do
        # Pick a random page
        local page=$((RANDOM % total_pages))
        local offset=$((page * PAGE_SIZE))
        
        # Corrupt first 40 bytes (FIL_PAGE_PREV to FIL_PAGE_ARCH_LOG_NO)
        dd if=/dev/urandom of="$output" bs=1 count=40 seek=$offset conv=notrunc 2>/dev/null
        
        corrupted=$((corrupted + 1))
        
        if [ $((corrupted % 20)) -eq 0 ]; then
            echo "  Corrupted $corrupted pages..."
        fi
    done
    
    echo "Torn pages corruption complete: $output ($corrupted pages)"
    echo "Recovery command: --header-repair"
}

# Scenario 2: dd-style arbitrary corruption
corrupt_dd_style() {
    local file="$1"
    local page="$2"
    local offset="${3:-0}"
    local len="${4:-40}"
    
    echo "=== Scenario: dd-style Corruption ==="
    echo "Original: $file"
    echo "Target: Page $page, Offset $offset, Length $len"
    
    local output="$OUTPUT_DIR/dd_corrupt_p${page}_o${offset}_l${len}.ibd"
    cp "$file" "$output"
    
    # Calculate absolute offset
    local abs_offset=$((page * PAGE_SIZE + offset))
    
    # Perform dd overwrite
    dd if=/dev/urandom of="$output" bs=1 count=$len seek=$abs_offset conv=notrunc 2>/dev/null
    
    echo "dd corruption complete: $output"
    echo "Recovery command: Try --default-recovery or --header-repair"
}

# Scenario 3: File Truncation (TRUNCATE TABLE simulation)
corrupt_truncate() {
    local file="$1"
    local percent="${2:-10}"
    
    echo "=== Scenario: File Truncation ==="
    echo "Original: $file"
    echo "Truncating to ${percent}% of original size"
    
    local output="$OUTPUT_DIR/truncated_${percent}pct.ibd"
    cp "$file" "$output"
    
    # Calculate new size
    local file_size=$(stat -c%s "$file")
    local new_size=$((file_size * (100 - percent) / 100))
    
    # Truncate file
    truncate -s $new_size "$output"
    
    echo "Truncation complete: $output"
    echo "  Original size: $file_size bytes"
    echo "  New size: $new_size bytes"
    echo "Recovery command: --truncate-recovery"
}

# Scenario 4: Page Header Corruption (Specific Fields)
corrupt_page_header() {
    local file="$1"
    local page="$2"
    
    echo "=== Scenario: Page Header Corruption ==="
    echo "Original: $file"
    echo "Target page: $page"
    
    local output="$OUTPUT_DIR/header_corrupt_p${page}.ibd"
    cp "$file" "$output"
    
    local offset=$((page * PAGE_SIZE))
    
    # Corrupt specific header fields:
    # - FIL_PAGE_PREV (offset 0, 4 bytes)
    # - FIL_PAGE_NEXT (offset 4, 4 bytes)
    # - FIL_PAGE_LSN (offset 8, 8 bytes)
    # - FIL_PAGE_TYPE (offset 16, 2 bytes)
    # - FIL_PAGE_OFFSET (offset 20, 4 bytes)
    # - FIL_PAGE_CHECKSUM (offset 24, 8 bytes double write)
    
    # Corrupt page type
    dd if=/dev/urandom of="$output" bs=1 count=2 seek=$((offset + 16)) conv=notrunc 2>/dev/null
    
    # Corrupt page number
    dd if=/dev/urandom of="$output" bs=1 count=4 seek=$((offset + 20)) conv=notrunc 2>/dev/null
    
    # Corrupt LSN
    dd if=/dev/urandom of="$output" bs=1 count=8 seek=$((offset + 8)) conv=notrunc 2>/dev/null
    
    echo "Page header corruption complete: $output"
    echo "Recovery command: --header-repair"
}

# Scenario 5: B+Tree Pointer Corruption
corrupt_btree_pointers() {
    local file="$1"
    local num_pages="${2:-50}"
    
    echo "=== Scenario: B+Tree Pointer Corruption ==="
    echo "Original: $file"
    echo "Targeting B+Tree pages"
    
    local output="$OUTPUT_DIR/btree_pointers.ibd"
    cp "$file" "$output"
    
    local file_size=$(stat -c%s "$file")
    local total_pages=$((file_size / PAGE_SIZE))
    
    # Corrupt page chain pointers for B+Tree index pages
    # In MySQL 5.6, index pages are linked via FIL_PAGE_PREV and FIL_PAGE_NEXT
    
    local corrupted=0
    for i in $(seq 1 $num_pages); do
        local page=$((RANDOM % total_pages))
        local offset=$((page * PAGE_SIZE))
        
        # Corrupt previous page pointer
        dd if=/dev/urandom of="$output" bs=1 count=4 seek=$((offset + 0)) conv=notrunc 2>/dev/null
        
        # Corrupt next page pointer
        dd if=/dev/urandom of="$output" bs=1 count=4 seek=$((offset + 4)) conv=notrunc 2>/dev/null
        
        corrupted=$((corrupted + 1))
    done
    
    echo "B+Tree pointer corruption complete: $output ($corrupted pages)"
    echo "Recovery command: --rebuild-chain"
}

# Scenario 6: BLOB Chain Corruption
corrupt_blob_chain() {
    local file="$1"
    local num_breaks="${2:-10}"
    
    echo "=== Scenario: BLOB Chain Breakage ==="
    echo "Original: $file"
    
    local output="$OUTPUT_DIR/blob_chain.ibd"
    cp "$file" "$output"
    
    local file_size=$(stat -c%s "$file")
    local total_pages=$((file_size / PAGE_SIZE))
    
    # In MySQL 5.6, BLOB data spans multiple pages
    # Corrupt intermediate BLOB page pointers to break the chain
    
    for i in $(seq 1 $num_breaks); do
        # Pick a page in the middle (not first or last)
        local page=$((1 + RANDOM % (total_pages - 2)))
        local offset=$((page * PAGE_SIZE))
        
        # Corrupt page type to make it look like a different type
        dd if=/dev/urandom of="$output" bs=1 count=2 seek=$((offset + 16)) conv=notrunc 2>/dev/null
        
        # Corrupt link pointers
        dd if=/dev/urandom of="$output" bs=1 count=8 seek=$((offset + 0)) conv=notrunc 2>/dev/null
    done
    
    echo "BLOB chain corruption complete: $output"
    echo "Recovery command: --blob-recovery"
}

# Scenario 7: Checksum Corruption
corrupt_checksum() {
    local file="$1"
    local num_pages="${2:-100}"
    
    echo "=== Scenario: Checksum Corruption ==="
    echo "Original: $file"
    
    local output="$OUTPUT_DIR/checksum_corrupt.ibd"
    cp "$file" "$output"
    
    local file_size=$(stat -c%s "$file")
    local total_pages=$((file_size / PAGE_SIZE))
    
    # Write wrong checksums (checksum fields remain valid pattern but wrong value)
    for i in $(seq 1 $num_pages); do
        local page=$((RANDOM % total_pages))
        local offset=$((page * PAGE_SIZE + 24))  # FIL_PAGE_CHECKSUM offset
        
        # Write a fixed wrong checksum
        local wrong_checksum=$((RANDOM * RANDOM))
        printf "\\x$(printf '%02x' $((wrong_checksum & 0xFF)))\\" | dd of="$output" bs=1 count=4 seek=$offset conv=notrunc 2>/dev/null
        printf "\\x$(printf '%02x' $(((wrong_checksum >> 8) & 0xFF)))\\" | dd of="$output" bs=1 count=4 seek=$((offset + 4)) conv=notrunc 2>/dev/null
    done
    
    echo "Checksum corruption complete: $output"
    echo "Recovery command: --header-repair"
}

# Scenario 8: Record Structure Corruption
corrupt_record_structure() {
    local file="$1"
    local num_records="${2:-100}"
    
    echo "=== Scenario: Record Structure Corruption ==="
    echo "Original: $file"
    
    local output="$OUTPUT_DIR/record_struct.ibd"
    cp "$file" "$output"
    
    # Record headers are in page data area (offset 38+)
    # Record header format (compact):
    # - 2 bytes: next record offset
    # - 1 byte: info bits
    # - 2 bytes: heap number
    # - 1 byte: number of fields
    
    for i in $(seq 1 $num_records); do
        # Pick a random page
        local page=$((RANDOM % (total_pages - 1) + 1))
        local page_offset=$((page * PAGE_SIZE))
        
        # Skip FIL header (38 bytes), go to page data
        # Skip Infimum record (13 bytes), go to actual records
        # Record starts around offset 38 + 13 + 13 = 64
        
        local record_start=$((page_offset + 64 + RANDOM % (PAGE_SIZE - 128)))
        
        # Corrupt record header fields
        dd if=/dev/urandom of="$output" bs=1 count=2 seek=$record_start conv=notrunc 2>/dev/null  # next_record
        dd if=/dev/urandom of="$output" bs=1 count=1 seek=$((record_start + 2)) conv=notrunc 2>/dev/null  # info_bits
    done
    
    echo "Record structure corruption complete: $output"
    echo "Recovery command: --default-recovery"
}

# Scenario 9: Mixed Corruption (Complex Real-world Scenario)
corrupt_mixed() {
    local file="$1"
    local intensity="${2:-medium}"  # low, medium, high
    
    echo "=== Scenario: Mixed Corruption ==="
    echo "Original: $file"
    echo "Intensity: $intensity"
    
    local output="$OUTPUT_DIR/mixed_corruption.ibd"
    cp "$file" "$output"
    
    # Scale corruption based on intensity
    case "$intensity" in
        low)
            torn_pages=50
            header_corrupt=20
            btree_corrupt=10
            blob_break=5
            ;;
        medium)
            torn_pages=100
            header_corrupt=50
            btree_corrupt=50
            blob_break=20
            ;;
        high)
            torn_pages=200
            header_corrupt=100
            btree_corrupt=100
            blob_break=50
            ;;
    esac
    
    # Apply multiple corruption types
    echo "Applying torn pages corruption..."
    corrupt_torn_pages "$output" $torn_pages 2>/dev/null
    cp "${output}.torn" "$output" 2>/dev/null || true
    
    echo "Applying B+Tree corruption..."
    corrupt_btree_pointers "$output" $btree_corrupt 2>/dev/null
    cp "${output}.btree" "$output" 2>/dev/null || true
    
    echo "Applying BLOB chain corruption..."
    corrupt_blob_chain "$output" $blob_break 2>/dev/null
    cp "${output}.blob" "$output" 2>/dev/null || true
    
    echo "Mixed corruption complete: $output"
    echo "Recovery command: Multiple modes may be needed"
}

# Scenario 10: Fragmented Recovery Test (rm -rf Simulation)
corrupt_fragmented() {
    local file="$1"
    local num_fragments="${2:-100}"
    
    echo "=== Scenario: File Fragmentation (rm -rf) ==="
    echo "Original: $file"
    
    local output="$OUTPUT_DIR/fragmented.ibd"
    
    # Create a fragmented file by extracting and reassembling
    local file_size=$(stat -c%s "$file")
    local fragment_size=$((file_size / num_fragments))
    
    # Create temp directory for fragments
    local temp_dir="$CORRUPTION_DIR/fragments_$$"
    mkdir -p "$temp_dir"
    
    # Extract fragments
    for i in $(seq 0 $((num_fragments - 1))); do
        dd if="$file" of="$temp_dir/frag_$i" bs=$fragment_size count=1 skip=$i 2>/dev/null
    done
    
    # Shuffle fragments
    ls "$temp_dir"/frag_* | shuf | while read frag; do
        basename=$(basename "$frag")
        mv "$frag" "$temp_dir/${basename}.shuffled"
    done
    
    # Reassemble in shuffled order
    cat "$temp_dir"/frag_*.shuffled > "$output" 2>/dev/null || {
        # Fallback if shuf not available
        ls "$temp_dir"/frag_* | sort -R | xargs cat > "$output"
    }
    
    # Clean up
    rm -rf "$temp_dir"
    
    echo "Fragmentation complete: $output"
    echo "Recovery command: --rmrf-recovery"
}

# ==============================================================================
# Main Entry Point
# ==============================================================================

main() {
    create_output_dir
    
    local command="${1:-}"
    shift || true
    
    case "$command" in
        torn)
            corrupt_torn_pages "$@"
            ;;
        dd)
            corrupt_dd_style "$@"
            ;;
        truncate)
            corrupt_truncate "$@"
            ;;
        header)
            corrupt_page_header "$@"
            ;;
        btree)
            corrupt_btree_pointers "$@"
            ;;
        blob)
            corrupt_blob_chain "$@"
            ;;
        checksum)
            corrupt_checksum "$@"
            ;;
        record)
            corrupt_record_structure "$@"
            ;;
        mixed)
            corrupt_mixed "$@"
            ;;
        fragmented)
            corrupt_fragmented "$@"
            ;;
        all)
            echo "Running all corruption scenarios..."
            local original="$1"
            copy_original "$original"
            
            echo ""
            echo "=== Running All Scenarios ==="
            echo ""
            
            # Run each scenario
            corrupt_torn_pages "$original" 100
            corrupt_fragmented "$original" 100
            corrupt_blob_chain "$original" 50
            corrupt_btree_pointers "$original" 50
            corrupt_page_header "$original" 100
            corrupt_checksum "$original" 100
            corrupt_truncate "$original" 10
            corrupt_mixed "$original" medium
            
            echo ""
            echo "=== All Scenarios Complete ==="
            echo "Output directory: $OUTPUT_DIR"
            ;;
        *)
            usage
            ;;
    esac
}

# Run main if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
