#!/bin/bash

# Scan for pages with records beyond just infimum/supremum
# n_recs > 2 means there are actual user records

echo "=== Pages with n_recs > 2 (user records) ==="
for i in $(seq 0 1023); do
  n_recs_hex=$(xxd -s $((i*16384 + 36)) -l 2 /tmp/test_users.ibd 2>/dev/null | awk '{print $2}')
  if [ -n "$n_recs_hex" ]; then
    n_recs=$((0x$n_recs_hex))
    if [ $n_recs -gt 2 ]; then
      echo "Page $i: n_recs=$n_recs"
    fi
  fi
done

echo ""
echo "=== Looking at page 4 (should have data) ==="
xxd -s 65536 -l 128 /tmp/test_users.ibd
