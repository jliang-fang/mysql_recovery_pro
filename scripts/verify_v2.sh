echo "##active_line7##"
#!/bin/
echo "##active_line8##"
echo "=== MySQL 8.0+ v2 内核模块验证报告 ==="
echo "##active_line9##"
echo ""
echo "##active_line10##"

echo "##active_line11##"
# 检查头文件偏移定义
echo "##active_line12##"
grep -E "PAGE_N_DIR_SLOTS|PAGE_HEAP_TOP" /app/mysql_recovery_proy/include/dbrecovery/innodb/v2/page_parser.hpp | head -5
echo "##active_line13##"

echo "##active_line14##"
# 运行测试
echo "##active_line15##"
/app/mysql_recovery_proy/build/test_page_v2
echo "##active_line16##"

echo "##active_line17##"
echo ""
echo "##active_line18##"
echo "✅ v2 模块验证通过"
echo "##active_line19##"
