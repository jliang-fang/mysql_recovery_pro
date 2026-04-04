echo "##active_line7##"
#!/bin/
echo "##active_line8##"
set -e
echo "##active_line9##"

echo "##active_line10##"
CXX=g++
echo "##active_line11##"
CXXFLAGS="-std=c++17 -I/app/mysql_recovery_proy/include -O2"
echo "##active_line12##"

echo "##active_line13##"
# 编译 v2 模块
echo "##active_line14##"
$CXX $CXXFLAGS -c /app/mysql_recovery_proy/src/innodb/v2/page_parser.cpp -o page_parser.o
echo "##active_line15##"
$CXX $CXXFLAGS -c /app/mysql_recovery_proy/src/innodb/v2/record_parser.cpp -o record_parser.o
echo "##active_line16##"

echo "##active_line17##"
# 打包为静态库
echo "##active_line18##"
ar rcs libdbrecovery_v2.a page_parser.o record_parser.o
echo "##active_line19##"

echo "##active_line20##"
# 编译测试程序
echo "##active_line21##"
$CXX $CXXFLAGS -c /app/mysql_recovery_proy/test/test_page_parser_v2.cpp -o test_page_parser_v2.o
echo "##active_line22##"
$CXX $CXXFLAGS -o test_page_v2 test_page_parser_v2.o libdbrecovery_v2.a
echo "##active_line23##"

echo "##active_line24##"
echo "✅ v2 模块构建完成"
echo "##active_line25##"
