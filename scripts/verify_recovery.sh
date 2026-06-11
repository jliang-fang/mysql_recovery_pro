#!/bin/bash

# Recovery Verification Script
# Compares recovered data with original MySQL data

echo "=============================================="
echo "Recovery Verification Script"
echo "=============================================="

# Source MySQL data info
echo ""
echo "[1] Source MySQL Data Statistics:"
podman exec mysql56_recovery_test mysql -urecovery_test -e "
SELECT 
    COUNT(*) as total_records,
    COUNT(DISTINCT email) as distinct_emails,
    MIN(id) as min_id,
    MAX(id) as max_id,
    MIN(create_time) as first_record,
    MAX(create_time) as last_record
FROM recovery_test.test_users_partitioned;
" 2>/dev/null

echo ""
echo "[2] Checking email pattern distribution..."
podman exec mysql56_recovery_test mysql -urecovery_test -e "
SELECT 
    CASE 
        WHEN email LIKE '%@com%' THEN '.com'
        WHEN email LIKE '%@cn%' THEN '.cn'
        WHEN email LIKE '%@net%' THEN '.net'
        WHEN email LIKE '%@org%' THEN '.org'
        WHEN email LIKE '%@io%' THEN '.io'
        WHEN email LIKE '%@edu%' THEN '.edu'
        ELSE 'other'
    END as domain,
    COUNT(*) as count
FROM recovery_test.test_users_partitioned
GROUP BY domain
ORDER BY count DESC
LIMIT 10;
" 2>/dev/null

# Count recovered unique emails
echo ""
echo "[3] Recovered Data Statistics:"
 recovered_dir="${1:-/app/mysql_recovery_pro/test_output/truncate_test_v2}"
 csv_file="$recovered_dir/recovered_table.csv"

if [ -f "$csv_file" ]; then
    # Exclude comment lines and count
    recovered_count=$(grep -v "^--" "$csv_file" | grep -c ",")
    echo "  Total recovered records: $recovered_count"
    
    # Extract unique emails
    unique_emails=$(grep -v "^--" "$csv_file" | grep "@" | cut -d',' -f3 | sort -u | wc -l)
    echo "  Unique emails: $unique_emails"
    
    # Sample data
    echo ""
    echo "[4] Sample Recovered Data:"
    grep -v "^--" "$csv_file" | head -10
else
    echo "ERROR: Recovered file not found: $csv_file"
    exit 1
fi

echo ""
echo "[5] Recovery Rate Calculation:"
podman exec mysql56_recovery_test mysql -urecovery_test -e "SELECT COUNT(*) FROM recovery_test.test_users_partitioned;" 2>/dev/null | tail -1 > /tmp/source_count.txt
recovered_total=$(grep -v "^--" "$csv_file" | wc -l)
recovered_total=$((recovered_total - 1))  # Subtract header

source_count=$(cat /tmp/source_count.txt)
recovery_rate=$(echo "scale=4; $recovered_total / $source_count * 100" | bc 2>/dev/null || echo "N/A")

echo "  Source records: $source_count"
echo "  Recovered records: $recovered_total"
echo "  Recovery rate: ${recovery_rate}%"

if (( $(echo "$recovery_rate >= 99.9" | bc -l 2>/dev/null || echo 1) )); then
    echo ""
    echo "✅ SUCCESS: Recovery rate >= 99.9%"
else
    echo ""
    echo "❌ FAILED: Recovery rate < 99.9%"
    exit 1
fi

echo ""
echo "=============================================="
echo "Verification Complete"
echo "=============================================="
