/**
 * @file cmake/cross_compilation.cmake
 * @brief 跨平台编译配置
 *
 * 提供跨平台编译的支持和配置
 */

# ==================== 跨平台编译支持 ====================

# 检测目标平台
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(TARGET_WINDOWS 1)
    message(STATUS "Target: Windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(TARGET_MACOS 1)
    message(STATUS "Target: macOS")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(TARGET_LINUX 1)
    message(STATUS "Target: Linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(TARGET_FREEBSD 1)
    message(STATUS "Target: FreeBSD")
endif()

# 检测目标架构
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
    set(TARGET_ARCH_X86_64 1)
    message(STATUS "Target Architecture: x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(TARGET_ARCH_ARM64 1)
    message(STATUS "Target Architecture: ARM64")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "i386" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "x86")
    set(TARGET_ARCH_X86 1)
    message(STATUS "Target Architecture: x86")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    set(TARGET_ARCH_ARM 1)
    message(STATUS "Target Architecture: ARM")
endif()

# ==================== 编译器特定配置 ====================

# GCC/Clang配置
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # 通用警告选项
    add_compile_options(-Wall -Wextra -Wpedantic)
    
    # 优化选项
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -march=native)
    endif()
    
    # C++20支持
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    
    # 位置无关代码
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    
    # 符号可见性
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
    
    # 静态链接libstdc++
    if(STATIC_STDCXX)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++")
    endif()
endif()

# MSVC配置
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # 通用警告选项
    add_compile_options(/W4)
    
    # 优化选项
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(/O2 /GL)
    endif()
    
    # C++20支持
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    
    # 动态运行时
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# ==================== 架构特定优化 ====================

# x86_64优化
if(TARGET_ARCH_X86_64)
    # 启用SSE4.2
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-msse4.2)
    endif()
    
    # 启用AVX2（如果可用）
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
        if(COMPILER_SUPPORTS_AVX2)
            add_compile_options(-mavx2)
        endif()
    endif()
    
    # 启用AVX-512（如果可用）
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        check_cxx_compiler_flag("-mavx512f" COMPILER_SUPPORTS_AVX512)
        if(COMPILER_SUPPORTS_AVX512)
            add_compile_options(-mavx512f)
        endif()
    endif()
endif()

# ARM64优化
if(TARGET_ARCH_ARM64)
    # 启用NEON
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-mfpu=neon)
    endif()
    
    # 启用CRC32
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-march=armv8-a+crc)
    endif()
endif()

# ==================== 静态链接配置 ====================

option(STATIC_LINK "Static link libraries" OFF)

if(STATIC_LINK)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    set(BUILD_SHARED_LIBS OFF)
    
    # 静态链接libgcc和libstdc++
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
    endif()
endif()

# ==================== 交叉编译配置 ====================

# ARM64交叉编译示例
# cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-arm64.cmake ..

# x86_64交叉编译示例
# cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-x86_64.cmake ..

# ==================== 工具链文件示例 ====================

# toolchain-arm64.cmake:
# set(CMAKE_SYSTEM_NAME Linux)
# set(CMAKE_SYSTEM_PROCESSOR aarch64)
# set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
# set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# toolchain-x86_64.cmake:
# set(CMAKE_SYSTEM_NAME Linux)
# set(CMAKE_SYSTEM_PROCESSOR x86_64)
# set(CMAKE_C_COMPILER x86_64-linux-gnu-gcc)
# set(CMAKE_CXX_COMPILER x86_64-linux-gnu-g++)

# ==================== 架构检测宏 ====================

# 检测x86_64
macro(check_x86_64)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
        set(HAVE_X86_64 1)
    endif()
endmacro()

# 检测ARM64
macro(check_arm64)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(HAVE_ARM64 1)
    endif()
endmacro()

# 检测SSE4.2
macro(check_sse42)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        check_cxx_compiler_flag("-msse4.2" COMPILER_SUPPORTS_SSE42)
        if(COMPILER_SUPPORTS_SSE42)
            set(HAVE_SSE42 1)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(HAVE_SSE42 1)  # MSVC always has SSE4.2 on x86_64
    endif()
endmacro()

# 检测AVX2
macro(check_avx2)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
        if(COMPILER_SUPPORTS_AVX2)
            set(HAVE_AVX2 1)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        check_cxx_compiler_flag("/arch:AVX2" COMPILER_SUPPORTS_AVX2_MSVC)
        if(COMPILER_SUPPORTS_AVX2_MSVC)
            set(HAVE_AVX2 1)
        endif()
    endif()
endmacro()

# 检测NEON
macro(check_neon)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        check_cxx_compiler_flag("-mfpu=neon" COMPILER_SUPPORTS_NEON)
        if(COMPILER_SUPPORTS_NEON)
            set(HAVE_NEON 1)
        endif()
    endif()
endmacro()
