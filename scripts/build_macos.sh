#!/bin/bash
# build_macos.sh - macOS构建脚本
# 用于在macOS环境下构建MySQL Recovery Pro

set -e

echo "=========================================="
echo "MySQL Recovery Pro - macOS Build Script"
echo "=========================================="

# 配置选项
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_DIR="${BUILD_DIR:-build}"
INSTALL_DIR="${INSTALL_DIR:-/usr/local}"
THREADS="${THREADS:-$(sysctl -n hw.ncpu)}"

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
    echo "  brew install cmake"
    exit 1
fi

CMAKE_VERSION=$(cmake --version | head -n1 | grep -oP '\d+\.\d+\.\d+')
echo "  CMake version: $CMAKE_VERSION"

# 检查Xcode命令行工具
if ! xcode-select -p &> /dev/null; then
    echo "ERROR: Xcode command line tools not installed"
    echo "Please run: xcode-select --install"
    exit 1
fi

# 检查编译器
if ! command -v clang++ &> /dev/null; then
    echo "ERROR: Clang++ is not installed"
    echo "Please install Xcode or Command Line Tools"
    exit 1
fi

CLANG_VERSION=$(clang++ --version | head -n1 | grep -oP '\d+\.\d+\.\d+')
echo "  Compiler: Clang $CLANG_VERSION"

# 检查必要的库
echo ""
echo "Checking required libraries..."

# 检查Homebrew
if ! command -v brew &> /dev/null; then
    echo "WARNING: Homebrew not found"
    echo "  Consider installing Homebrew for easier dependency management"
    echo "  https://brew.sh"
fi

# nlohmann/json
if ! pkg-config --exists nlohmann-json 2>/dev/null; then
    if [ -n "$(command -v brew)" ]; then
        echo "  Installing nlohmann/json via Homebrew..."
        brew install nlohmann-json
    elif [ ! -d "nlohmann" ]; then
        echo "  nlohmann/json not found, downloading..."
        mkdir -p nlohmann
        curl -L https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp -o nlohmann/json.hpp
    fi
fi
echo "  nlohmann/json: OK"

# zlib
if ! pkg-config --exists zlib; then
    if [ -n "$(command -v brew)" ]; then
        echo "  Installing zlib via Homebrew..."
        brew install zlib
    fi
fi
echo "  zlib: OK"

# liburing (macOS不支持，但保持兼容性)
echo "  liburing: N/A (macOS does not support io_uring)"

# GTest (可选)
if pkg-config --exists gtest; then
    echo "  GTest: OK (tests enabled)"
elif [ -n "$(command -v brew)" ]; then
    echo "  Installing GTest via Homebrew..."
    brew install google-test
    echo "  GTest: OK"
else
    echo "  GTest: Not found (tests disabled)"
fi

# Qt5/Qt6 (可选)
if [ "${GUI_ENABLED:-OFF}" = "ON" ]; then
    if [ -n "$(command -v brew)" ]; then
        echo "  Installing Qt via Homebrew..."
        brew install qt
        echo "  Qt: OK"
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

# 对于Apple Silicon，添加特定的优化
if [ "$ARCH" = "arm64" ]; then
    CMAKE_OPTIONS+=("-DARM64_OPTIMIZATIONS=ON")
    # 设置架构标志
    CMAKE_OPTIONS+=("-DCMAKE_OSX_ARCHITECTURES=arm64")
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
