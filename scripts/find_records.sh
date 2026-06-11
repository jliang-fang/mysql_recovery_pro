#!/bin/bash

echo "Finding pages with more than 2 records..."
for i in $(seq 0 1023); do
  n_recs_hex=$(xxd -s $((i*16384 + 36)) -l 2 /tmp/test_users.ibd 2>/dev/null | awk '{print $2}')
  if [ -n "$n_recs_hex" ]; then
    n_recs=$((0x$n_recs_hex))
    if [ $n_recs -gt 2 ]; then
      echo "Page $i: n_recs=$n_recs"
    fi
  fi
done | head -20

echo ""
echo "=== Examining a page with many records (if any) ==="
# Check page 0
echo "Page 0 contents:"
xxd -s 0 -l 128 /tmp/test_users.ibd
