#!/bin/bash
# build_windows.sh - Windows构建脚本
# 用于在Windows环境下构建MySQL Recovery Pro

set -e

echo "=========================================="
echo "MySQL Recovery Pro - Windows Build Script"
echo "=========================================="

# 配置选项
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_DIR="${BUILD_DIR:-build}"
INSTALL_DIR="${INSTALL_DIR:-C:/Program Files/mysql_recovery}"
THREADS="${THREADS:-$(nproc 2>/dev/null || echo 4)}"

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --install-dir)
            INSTALL_DIR="$2"
            shift 2
            ;;
        --threads)
            THREADS="$2"
            shift 2
            ;;
        --prefix)
            INSTALL_DIR="$2"
            shift 2
            ;;
        --enable-gui)
            GUI_ENABLED="ON"
            shift
            ;;
        --enable-sanitizer)
            ENABLE_SANITIZER="ON"
            shift
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --debug              Build with debug symbols"
            echo "  --release            Build in release mode (default)"
            echo "  --build-dir DIR      Build directory (default: build)"
            echo "  --install-dir DIR    Installation prefix (default: C:/Program Files/mysql_recovery)"
            echo "  --threads N          Number of parallel jobs (default: auto)"
            echo "  --prefix DIR         Installation prefix (alias for --install-dir)"
            echo "  --enable-gui         Enable GUI support (Qt)"
            echo "  --enable-sanitizer   Enable AddressSanitizer"
            echo "  --help               Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

echo ""
echo "Build Configuration:"
echo "  Build Type:     $BUILD_TYPE"
echo "  Build Dir:      $BUILD_DIR"
echo "  Install Dir:    $INSTALL_DIR"
echo "  Threads:        $THREADS"
echo "  GUI Enabled:    ${GUI_ENABLED:-OFF}"
echo "  Sanitizer:      ${ENABLE_SANITIZER:-OFF}"
echo ""

# 检查依赖
echo "Checking dependencies..."

# 检查CMake
if ! command -v cmake &> /dev/null; then
    echo "ERROR: CMake is not installed"
    echo "Please install CMake 3.20 or higher"
    echo "  Download from: https://cmake.org/download/"
    exit 1
fi

CMAKE_VERSION=$(cmake --version | head -n1 | grep -oP '\d+\.\d+\.\d+')
echo "  CMake version: $CMAKE_VERSION"

# 检查Visual Studio
if command -v cl.exe &> /dev/null; then
    CL_VERSION=$(cl.exe 2>&1 | head -n1 | grep -oP '\d+\.\d+\.\d+')
    echo "  Compiler: MSVC $CL_VERSION"
elif [ -d "C:/Program Files/Microsoft Visual Studio" ]; then
    echo "  Visual Studio: Found"
else
    echo "WARNING: Visual Studio not found"
    echo "  Please install Visual Studio 2019 or later"
    echo "  Download from: https://visualstudio.microsoft.com/"
fi

# 检查必要的库
echo ""
echo "Checking required libraries..."

# nlohmann/json
if [ ! -d "nlohmann" ]; then
    echo "  nlohmann/json not found, downloading..."
    mkdir -p nlohmann
    curl -L https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp -o nlohmann/json.hpp
fi
echo "  nlohmann/json: OK"

# zlib
if [ ! -d "zlib" ]; then
    echo "  zlib not found, downloading..."
    mkdir -p zlib
    cd zlib
    curl -L https://github.com/madler/zlib/archive/refs/tags/v1.2.13.tar.gz -o zlib.tar.gz
    tar -xzf zlib.tar.gz
    cd ..
fi
echo "  zlib: OK"

# liburing (Windows不支持，但保持兼容性)
echo "  liburing: N/A (Windows does not support io_uring)"

# GTest (可选)
if [ ! -d "googletest" ]; then
    echo "  GTest not found, downloading..."
    git clone --depth 1 https://github.com/google/googletest.git googletest
fi
echo "  GTest: OK"

# Qt5/Qt6 (可选)
if [ "${GUI_ENABLED:-OFF}" = "ON" ]; then
    if [ -d "C:/Qt" ]; then
        echo "  Qt: Found at C:/Qt"
    else
        echo "  Qt: Not found (GUI disabled)"
        GUI_ENABLED="OFF"
    fi
fi

# 创建构建目录
echo ""
echo "Creating build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 配置CMake
echo ""
echo "Configuring CMake..."

CMAKE_OPTIONS=(
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DCMAKE_INSTALL_PREFIX=$INSTALL_DIR"
    "-DGUI_ENABLED=${GUI_ENABLED:-OFF}"
)

if [ "${ENABLE_SANITIZER:-OFF}" = "ON" ]; then
    CMAKE_OPTIONS+=("-DENABLE_ASAN=ON")
fi

# 检测架构
ARCH=$(uname -m 2>/dev/null || echo "x86_64")
echo "  Architecture: $ARCH"

# 运行CMake
cmake .. "${CMAKE_OPTIONS[@]}"

# 构建
echo ""
echo "Building..."
cmake --build . --config $BUILD_TYPE --parallel $THREADS

# 运行测试（如果启用）
if [ "${ENABLE_TESTS:-ON}" = "ON" ]; then
    echo ""
    echo "Running tests..."
    ctest --output-on-failure -C $BUILD_TYPE
fi

# 安装（如果指定）
if [ "${INSTALL:-OFF}" = "ON" ]; then
    echo ""
    echo "Installing to $INSTALL_DIR..."
    cmake --build . --config $BUILD_TYPE --target install
fi

echo ""
echo "=========================================="
echo "Build completed successfully!"
echo "=========================================="
echo ""
echo "To install, run: cmake --build . --config $BUILD_TYPE --target install"
echo "To run tests:   ctest -C $BUILD_TYPE"
echo ""
