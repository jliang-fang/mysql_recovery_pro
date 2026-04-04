#!/bin/bash
# build_linux.sh - Linux构建脚本
# 用于在Linux环境下构建MySQL Recovery Pro

set -e

echo "=========================================="
echo "MySQL Recovery Pro - Linux Build Script"
echo "=========================================="

# 配置选项
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_DIR="${BUILD_DIR:-build}"
INSTALL_DIR="${INSTALL_DIR:-/usr/local}"
THREADS="${THREADS:-$(nproc)}"

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
            echo "  --install-dir DIR    Installation prefix (default: /usr/local)"
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
    exit 1
fi

CMAKE_VERSION=$(cmake --version | head -n1 | grep -oP '\d+\.\d+\.\d+')
echo "  CMake version: $CMAKE_VERSION"

# 检查C++编译器
if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    echo "ERROR: No C++ compiler found"
    echo "Please install GCC 10+ or Clang 12+"
    exit 1
fi

if command -v g++ &> /dev/null; then
    GCC_VERSION=$(g++ --version | head -n1 | grep -oP '\d+\.\d+\.\d+')
    echo "  Compiler: GCC $GCC_VERSION"
elif command -v clang++ &> /dev/null; then
    CLANG_VERSION=$(clang++ --version | head -n1 | grep -oP '\d+\.\d+\.\d+')
    echo "  Compiler: Clang $CLANG_VERSION"
fi

# 检查必要的库
echo ""
echo "Checking required libraries..."

# nlohmann/json
if ! pkg-config --exists nlohmann-json 2>/dev/null; then
    if [ ! -d "nlohmann" ]; then
        echo "  nlohmann/json not found, downloading..."
        mkdir -p nlohmann
        curl -L https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp -o nlohmann/json.hpp
    fi
fi
echo "  nlohmann/json: OK"

# zlib
if ! pkg-config --exists zlib; then
    if ! dpkg -l | grep -q zlib1g-dev 2>/dev/null; then
        echo "  WARNING: zlib-dev not found, trying to install..."
        sudo apt-get update -qq && sudo apt-get install -y zlib1g-dev
    fi
fi
echo "  zlib: OK"

# liburing (可选)
if pkg-config --exists liburing; then
    echo "  liburing: OK (io_uring support enabled)"
else
    echo "  liburing: Not found (io_uring support disabled)"
fi

# GTest (可选)
if pkg-config --exists gtest; then
    echo "  GTest: OK (tests enabled)"
else
    echo "  GTest: Not found (tests disabled)"
fi

# Qt5/Qt6 (可选)
if [ "${GUI_ENABLED:-OFF}" = "ON" ]; then
    if pkg-config --exists Qt5Core; then
        echo "  Qt5: OK"
    elif pkg-config --exists Qt6Core; then
        echo "  Qt6: OK"
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
ARCH=$(uname -m)
echo "  Architecture: $ARCH"

# 对于ARM64，添加特定的优化
if [ "$ARCH" = "aarch64" ]; then
    CMAKE_OPTIONS+=("-DARM64_OPTIMIZATIONS=ON")
fi

# 运行CMake
cmake .. "${CMAKE_OPTIONS[@]}"

# 构建
echo ""
echo "Building..."
make -j"$THREADS"

# 运行测试（如果启用）
if [ "${ENABLE_TESTS:-ON}" = "ON" ]; then
    echo ""
    echo "Running tests..."
    ctest --output-on-failure -j"$THREADS"
fi

# 安装（如果指定）
if [ "${INSTALL:-OFF}" = "ON" ]; then
    echo ""
    echo "Installing to $INSTALL_DIR..."
    sudo make install
fi

echo ""
echo "=========================================="
echo "Build completed successfully!"
echo "=========================================="
echo ""
echo "To install, run: sudo make install"
echo "To run tests:   ctest"
echo ""
