# vcpkg CMake toolchain file
# This file is used by vcpkg to integrate with CMake builds.
# It sets up the necessary variables for finding packages installed via vcpkg.

if(NOT DEFINED ENV{VCPKG_ROOT})
    message(FATAL_ERROR "vcpkg root directory not found. Please set VCPKG_ROOT environment variable.")
endif()

set(VCPKG_INSTALLED_DIR "$ENV{VCPKG_ROOT}/installed")
set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "vcpkg target triplet")

# Set CMake prefix path to include vcpkg-installed packages
list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")

# Enable vcpkg manifest mode if vcpkg.json exists in source root
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg.json")
    set(CMAKE_FORCE_MANIFEST_MODE ON)
endif()

# Set default toolchain variables for cross-compilation support (if needed)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/toolchains/${VCPKG_TARGET_TRIPLET}.cmake" CACHE FILEPATH "vcpkg chainload toolchain")

# Include vcpkg's CMake functions and macros
include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
