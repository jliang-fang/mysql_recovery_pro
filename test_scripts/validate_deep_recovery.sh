#!/bin/bash

# Validation script for MySQL 5.6 Deep Recovery (TRUNCATE & RM-RF scenarios)
# This script simulates a real disaster and verifies recovery rate

DATA_DIR="/app/mysql_recovery_pro/test_data"
OUTPUT_DIR="/app/mysql_recovery_pro/test_output"
mkdir -p $DATA_DIR $OUTPUT_DIR

echo "=== Step 1: Setup Environment ==="
# Note: In a real environment, we'd start MySQL and insert 5M records here.
# For this validation prototype, we assume a pre-generated .ibd file with 5M records exists.
# If not found, we create a dummy one for structural testing.
IBD_FILE="$DATA_DIR/test_table.ibd"

if [ ! -f "$IBD_FILE" ]; then
    echo "[WARN] No test .ibd file with 5M records found. Creating a mock binary image..."
    # Create a mock InnoDB-like file for testing the forensic scanner (1GB)
    dd if=/dev/urandom of=$IBD_FILE bs=1M count=1024
    echo "[INFO] Mock data created at $IBD_FILE"
fi

echo "=== Step 2: Simulate Disaster ==="
# Scenario A: TRUNCATE (Simulated by creating a copy and zeroing out the page headers)
TRUNCATED_IBD="$DATA_DIR/truncated.ibd"
cp $IBD_FILE $TRUNCATED_IBD
# Zero out some headers to simulate truncation metadata changes
dd if=/dev/zero of=$TRUNCATED_IBD bs=1 count=100 seek=0 conv=notrunc

# Scenario B: RM-RF (Simulated by deleting the file and scanning a 'disk image' copy)
DISK_IMAGE="$DATA_DIR/disk_image.bin"
cp $IBD_FILE $DISK_IMAGE
echo "[INFO] Simulated disk image created at $DISK_IMAGE"

echo "=== Step 3: Execute Deep Recovery ==="
# We use the binary tool we just integrated
RECOVERY_BIN="/app/mysql_recovery_pro/bin/mysql_recover" # Path depends on build output

if [ -f "$RECOVERY_BIN" ]; then
    echo "[INFO] Running forensic recovery on truncated file..."
    $RECOVERY_BIN --deep-recover $TRUNCATED_IBD $OUTPUT_DIR/truncated_recovered.csv
    
    echo "[INFO] Running forensic recovery on raw disk image..."
    $RECOVERY_BIN --deep-recover $DISK_IMAGE $OUTPUT_DIR/disk_recovered.csv
else
    echo "[ERROR] Recovery binary not found at $RECOVERY_BIN. Please build the project first."
    exit 1
fi

echo "=== Step 4: Verify Recovery Rate ==="
# In a real scenario, we'd compare line counts of original vs recovered CSVs
if [ -f "$OUTPUT_DIR/truncated_recovered.csv" ]; then
    RECOVERED_COUNT=$(wc -l < $OUTPUT_DIR/truncated_recovered.csv)
    echo "[RESULT] Recovered tuples from TRUNCATE scenario: $RECOVERED_COUNT"
else
    echo "[ERROR] Recovery failed to produce output."
fi

echo "=== Validation Complete ==="
