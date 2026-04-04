# MySQL Recovery Pro - Build Scripts

This directory contains build scripts for different platforms.

## Available Scripts

### Linux
- `build_linux.sh` - Build script for Linux

### macOS
- `build_macos.sh` - Build script for macOS

### Windows
- `build_windows.sh` - Build script for Windows

## Usage

### Linux/macOS

```bash
# Basic build (Release mode)
./build_linux.sh

# Build with debug symbols
./build_linux.sh --debug

# Build with GUI support
./build_linux.sh --enable-gui

# Build with AddressSanitizer
./build_linux.sh --enable-sanitizer

# Custom build directory
./build_linux.sh --build-dir my_build

# Install to custom location
./build_linux.sh --install-dir /opt/mysql_recovery
```

### Windows

```bash
# Basic build (Release mode)
./build_windows.sh

# Build with debug symbols
./build_windows.sh --debug

# Build with GUI support
./build_windows.sh --enable-gui

# Build with AddressSanitizer
./build_windows.sh --enable-sanitizer
```

## Build Options

| Option | Description |
|--------|-------------|
| `--debug` | Build with debug symbols |
| `--release` | Build in release mode (default) |
| `--build-dir DIR` | Build directory |
| `--install-dir DIR` | Installation prefix |
| `--threads N` | Number of parallel jobs |
| `--prefix DIR` | Installation prefix (alias for --install-dir) |
| `--enable-gui` | Enable GUI support (Qt) |
| `--enable-sanitizer` | Enable AddressSanitizer |
| `--help` | Show help message |

## Prerequisites

### All Platforms
- CMake 3.20 or higher
- C++20 compatible compiler:
  - GCC 10+ (Linux/macOS)
  - Clang 12+ (Linux/macOS)
  - MSVC 2019+ (Windows)

### Linux
- nlohmann/json library
- zlib library
- liburing (optional, for io_uring support)
- GTest (optional, for tests)

### macOS
- Xcode Command Line Tools
- Homebrew (recommended)
- nlohmann/json library
- zlib library
- Qt5/Qt6 (optional, for GUI)

### Windows
- Visual Studio 2019 or later
- nlohmann/json library
- zlib library
- Qt5/Qt6 (optional, for GUI)

## Platform-Specific Notes

### Linux
- The build script will automatically install missing dependencies via apt if possible
- For ARM64 architecture, ARM-specific optimizations are enabled automatically

### macOS
- Homebrew is recommended for dependency management
- For Apple Silicon (M1/M2), ARM64 optimizations are enabled automatically
- Qt can be installed via `brew install qt`

### Windows
- Visual Studio 2019 or later is required
- For ARM64 Windows, ARM-specific optimizations are enabled automatically
- Qt can be installed from https://www.qt.io/download

## Cross-Compilation

### Cross-compile for ARM64 Linux on x86_64

```bash
# Create a toolchain file
cat > arm64-toolchain.cmake << EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
EOF

# Build
./build_linux.sh \
    --build-dir build_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=arm64-toolchain.cmake
```

### Cross-compile for Windows on Linux

```bash
# Install MinGW-w64
sudo apt-get install gcc-mingw-w64 g++-mingw-w64

# Create a toolchain file
cat > windows-toolchain.cmake << EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
EOF

# Build
./build_linux.sh \
    --build-dir build_windows \
    -DCMAKE_TOOLCHAIN_FILE=windows-toolchain.cmake
```

## Continuous Integration

The project uses GitHub Actions for CI/CD. See `.github/workflows/ci.yml` for details.

## Troubleshooting

### Common Issues

1. **CMake not found**
   - Install CMake from https://cmake.org/download/

2. **Compiler not found**
   - Linux: `sudo apt-get install build-essential`
   - macOS: `xcode-select --install`
   - Windows: Install Visual Studio

3. **Library not found**
   - Install the missing library via package manager or download manually

4. **Permission denied**
   - Run with `sudo` for system-wide installation

## Support

For support, please:
1. Check the documentation
2. Run with verbose logging
3. Submit an issue with detailed information
