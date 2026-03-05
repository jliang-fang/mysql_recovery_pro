echo "##active_line11##"
cmake_minimum_required(VERSION 3.10)
echo "##active_line12##"

echo "##active_line13##"
project(MyProject VERSION 1.0.0 LANGUAGES CXX)
echo "##active_line14##"

echo "##active_line15##"
set(CMAKE_CXX_STANDARD 17)
echo "##active_line16##"
set(CMAKE_CXX_STANDARD_REQUIRED ON)
echo "##active_line17##"

echo "##active_line18##"
# 添加 innodb 模块（含 v2）
echo "##active_line19##"
add_subdirectory(src/innodb)
echo "##active_line20##"

echo "##active_line21##"
# 主 CLI 构建
echo "##active_line22##"
add_executable(mysql8_recovery_cli main.cpp)
echo "##active_line23##"
target_link_libraries(mysql8_recovery_cli dbrecovery_core )
echo "##active_line24##"
