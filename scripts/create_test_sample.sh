#!/bin/
dd if=/dev/zero of=/app/mysql8_recovery/test_data/sample.ibd bs=16384 count=1 2>/dev/null

# 写入页类型（B+Tree Index = 9969）
printf '\xF1\x26' | dd of=/app/mysql8_recovery/test_data/sample.ibd bs=1 seek=0 count=2 conv=notrunc 2>/dev/null

# 写入 PAGE_HEADER
# PAGE_N_DIR_SLOTS = 3 (至少需要主键槽位 + 空槽)
printf '\x03\x00' | dd of=/app/mysql8_recovery/test_data/sample.ibd bs=1 seek=52 count=2 conv=notrunc 2>/dev/null

# PAGE_HEAP_TOP = 100 (记录从偏移100开始)
printf '\x64\x00' | dd of=/app/mysql8_recovery/test_data/sample.ibd bs=1 seek=58 count=2 conv=notrunc 2>/dev/null

# 写入第一个 slot（偏移100）
printf '\x64\x00' | dd of=/app/mysql8_recovery/test_data/sample.ibd bs=1 seek=38+16 count=2 conv=notrunc 2>/dev/null

# 写入第二个 slot（空槽，偏移0）
printf '\x00\x00' | dd of=/app/mysql8_recovery/test_data/sample.ibd bs=1 seek=38+18 count=2 conv=notrunc 2>/dev/null

# 写入第三个 slot（偏移50，另一个记录）
printf '\x32\x00' | dd of=/app/mysql8_recovery/test_data/sample.ibd bs=1 seek=38+20 count=2 conv=notrunc 2>/dev/null

echo "✅ 测试样本已创建"
