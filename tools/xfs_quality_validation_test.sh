#!/bin/bash
# XFS Recovery Quality Validation Test
# Tests schema-aware recovery with correct type mapping

set -e

DEVICE="${1:-/dev/nvme0n2p1}"
OUTPUT_DIR="${2:-/tmp/xfs_validation_test}"
BIN="./build/mysql_recovery_pro"

echo "=== XFS Recovery Quality Validation Test ==="
echo "Device: $DEVICE"
echo "Output: $OUTPUT_DIR"
echo ""

# Clean output directory
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# Test schema with multiple types
SCHEMA="id INT,name VARCHAR:100,email VARCHAR:100,age INT,salary DECIMAL:10:2,created_at DATETIME"

echo "Running schema-aware XFS recovery..."
echo "Schema: $SCHEMA"
echo ""

# Run recovery with timeout
timeout 180 $BIN --disk-carving \
    --device-path "$DEVICE" \
    --filesystem xfs \
    --schema="$SCHEMA" \
    --output "$OUTPUT_DIR" \
    --verbose 2>&1 | tail -50

echo ""
echo "=== Checking Output Files ==="

# Count output files
CSV_COUNT=$(ls -1 "$OUTPUT_DIR"/*.csv 2>/dev/null | wc -l)
echo "CSV files generated: $CSV_COUNT"

if [ "$CSV_COUNT" -eq 0 ]; then
    echo "ERROR: No CSV files generated!"
    exit 1
fi

# Check first few files
echo ""
echo "=== Sample CSV Content ==="
for f in $(ls -1 "$OUTPUT_DIR"/*.csv 2>/dev/null | head -3); do
    echo ""
    echo "File: $f"
    echo "---"
    head -5 "$f"
    echo ""
    # Check for integer values (should not look like dates)
    if head -10 "$f" | grep -q "^[0-9]\+,"; then
        echo "✓ Integer values detected in first column"
    else
        echo "⚠ First column may not be integers"
    fi
done

# Check for proper column headers
echo ""
echo "=== Column Header Analysis ==="
FIRST_CSV=$(ls -1 "$OUTPUT_DIR"/*.csv 2>/dev/null | head -1)
if [ -n "$FIRST_CSV" ]; then
    HEADERS=$(head -1 "$FIRST_CSV")
    echo "Headers: $HEADERS"

    # Check if schema column names are present
    if echo "$HEADERS" | grep -q "id INT"; then
        echo "✓ Schema column names present"
    else
        echo "⚠ Schema column names not found, may be using field_0 format"
    fi
fi

# Run quality validator if available
if [ -f "./tools/data_quality_validator.py" ]; then
    echo ""
    echo "=== Running Quality Validator ==="
    python3 ./tools/data_quality_validator.py "$OUTPUT_DIR" 2>/dev/null || true
fi

echo ""
echo "=== Test Complete ==="
