cmake_minimum_required(VERSION 3.16)

project(GUI VERSION 1.0.0 LANGUAGES CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找Qt6（优先）或Qt5
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
if(NOT Qt6_FOUND)
    find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)
endif()

# 收集源文件
set(GUI_SOURCES
    main.cpp
    MainWindow.cpp
)

# 创建可执行目标
add_executable(${PROJECT_NAME} ${GUI_SOURCES})

# 链接Qt库
if(TARGET Qt6::Widgets)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
endif()

# 设置包含目录（如果需要）
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
