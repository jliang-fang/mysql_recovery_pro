# src/CMakeLists.txt - MySQL 8.x Recovery Engine Source Files
# Enhanced for multi-version and multi-environment compatibility

# ==================== Common Sources ====================
set(COMMON_SOURCES
    common/version_compat.cpp
    common/environment_compat.cpp
    common/environment_compat_platform.cpp
)

# Add recovery module sources
set(RECOVERY_SOURCES
    recovery/engine.cpp
    recovery/blob_reconstructor.cpp
    recovery/rmrf_recovery.cpp
    recovery/blob_chain_rebuilder.cpp
    recovery/disaster_recovery.cpp
    recovery/truncate_recovery.cpp
)

# Add innodb module sources
set(INNODB_SOURCES
    innodb/data_validator.cpp
)

# Add repair module sources
set(REPAIR_SOURCES
    repair/repair.cpp
    repair/header_repairer.cpp
    repair/disk_scanner.cpp
    repair/crypt_scanner.cpp
    repair/corruption_detector.cpp
)

# Add export module sources
set(EXPORT_SOURCES
    export/csv_exporter.cpp
    export/sql_exporter.cpp
    export/ibd_exporter.cpp
)

# Add engine module sources
set(ENGINE_SOURCES
    engine/row_decoder.cpp
    engine/field_decoder.cpp
    engine/parallel_scanner.cpp
    engine/drop_table_recovery.cpp
)

# Add utils module sources
set(UTILS_SOURCES
    utils/cli_parser.cpp
    utils/file_mapper.cpp
    utils/innodb_checksum.cpp
    utils/zlib_helper.cpp
)

# Add tools module sources
set(TOOLS_SOURCES
    main_cli.cpp
    tools/heuristic_inference.cpp
)

# Add page module sources
set(PAGE_SOURCES
    page/page_v55.cpp
    page/page_v56.cpp
    page/page_v80.cpp
    page/page_v84.cpp
    page/page_factory.cpp
)

# Add disk module sources
set(DISK_SOURCES
    disk/raw_scanner.cpp
    disk/io_uring_scanner.cpp
    disk/ibd_file_carver.cpp
)

# Add all sources to dbrecovery_innodb library
target_sources(dbrecovery_innodb PRIVATE
    ${COMMON_SOURCES}
    ${RECOVERY_SOURCES}
    ${REPAIR_SOURCES}
    ${EXPORT_SOURCES}
    ${ENGINE_SOURCES}
    ${UTILS_SOURCES}
    ${PAGE_SOURCES}
    ${DISK_SOURCES}
    tools/heuristic_inference.cpp
)

# ==================== Platform-Specific Sources ====================
if(WIN32)
    # Windows-specific sources
    set(PLATFORM_SOURCES
        platform/windows/file_system_windows.cpp
        platform/windows/thread_windows.cpp
        platform/windows/memory_windows.cpp
    )
    target_compile_definitions(dbrecovery_innodb PRIVATE
        ENV_PLATFORM_WINDOWS
        _WIN32_WINNT=0x0601
    )
    target_link_libraries(dbrecovery_innodb PRIVATE
        psapi
        kernel32
        user32
        advapi32
    )
elseif(APPLE)
    # macOS-specific sources
    set(PLATFORM_SOURCES
        platform/macos/file_system_macos.cpp
        platform/macos/thread_macos.cpp
        platform/macos/memory_macos.cpp
    )
    target_compile_definitions(dbrecovery_innodb PRIVATE
        ENV_PLATFORM_MACOS
    )
    target_link_libraries(dbrecovery_innodb PRIVATE
        "-framework CoreFoundation"
        "-framework SystemConfiguration"
    )
else()
    # Linux/Unix sources
    set(PLATFORM_SOURCES
        platform/linux/file_system_linux.cpp
        platform/linux/thread_linux.cpp
        platform/linux/memory_linux.cpp
    )
    target_compile_definitions(dbrecovery_innodb PRIVATE
        ENV_PLATFORM_LINUX
    )
    target_link_libraries(dbrecovery_innodb PRIVATE
        pthread
        rt
    )
endif()

target_sources(dbrecovery_innodb PRIVATE ${PLATFORM_SOURCES})

# ==================== Architecture-Specific Optimizations ====================
if(CPU_FEATURES_FOUND)
    target_link_libraries(dbrecovery_innodb PRIVATE CPUFeatures::cpu_features)
    target_compile_definitions(dbrecovery_innodb PRIVATE
        HAVE_CPU_FEATURES=1
    )
endif()

# Enable CPU-specific optimizations for x86_64
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # Check for SSE4.2 support (CRC32C)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-msse4.2" COMPILER_SUPPORTS_SSE42)
        if(COMPILER_SUPPORTS_SSE42)
            target_compile_options(dbrecovery_innodb PRIVATE "-msse4.2")
            target_compile_definitions(dbrecovery_innodb PRIVATE
                ENV_CPU_HAS_SSE42=1
            )
        endif()
        
        # Check for AVX2 support
        check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
        if(COMPILER_SUPPORTS_AVX2)
            target_compile_options(dbrecovery_innodb PRIVATE "-mavx2")
            target_compile_definitions(dbrecovery_innodb PRIVATE
                ENV_CPU_HAS_AVX2=1
            )
        endif()
    endif()
endif()

# ==================== Endianness Detection ====================
include(TestBigEndian)
test_big_endian(ENV_IS_BIG_ENDIAN)
if(ENV_IS_BIG_ENDIAN)
    target_compile_definitions(dbrecovery_innodb PRIVATE ENV_IS_LITTLE_ENDIAN=0)
else()
    target_compile_definitions(dbrecovery_innodb PRIVATE ENV_IS_LITTLE_ENDIAN=1)
endif()

# ==================== C++ Standard Version ====================
if(CMAKE_CXX_STANDARD LESS 17)
    set(CMAKE_CXX_STANDARD 17)
endif()

# ==================== Build Type Specific Options ====================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(dbrecovery_innodb PRIVATE
        ENV_DEBUG_BUILD=1
        DEBUG=1
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(dbrecovery_innodb PRIVATE
            -g
            -O0
            -fno-omit-frame-pointer
        )
    endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(dbrecovery_innodb PRIVATE
        ENV_RELEASE_BUILD=1
        NDEBUG=1
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(dbrecovery_innodb PRIVATE
            -O3
            -DNDEBUG
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(dbrecovery_innodb PRIVATE
            /O2
            /DNDEBUG
        )
    endif()
endif()

# ==================== Sanitizer Support ====================
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_LSAN "Enable LeakSanitizer" OFF)

if(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_LSAN)
    set(SANITIZERS "")
    if(ENABLE_ASAN)
        list(APPEND SANITIZERS "address")
    endif()
    if(ENABLE_UBSAN)
        list(APPEND SANITIZERS "undefined")
    endif()
    if(ENABLE_LSAN)
        list(APPEND SANITIZERS "leak")
    endif()

    string(JOIN "," SANITIZER_LIST ${SANITIZERS})

    target_compile_options(dbrecovery_innodb PRIVATE
        -g
        -O0
        -fsanitize=${SANITIZER_LIST}
        -fno-omit-frame-pointer
    )
    target_link_libraries(dbrecovery_innodb PRIVATE
        -fsanitize=${SANITIZER_LIST}
        -fno-omit-frame-pointer
    )
endif()

# ==================== Link innodb module sources ====================
add_subdirectory(innodb)
target_link_libraries(dbrecovery_innodb PRIVATE dbrecovery_core dbrecovery_innodb_sources)

# ==================== GUI Support ====================
if(GUI_ENABLED)
    add_subdirectory(gui)
endif()

# ==================== CLI Executable ====================
# 主可执行文件在根目录 CMakeLists.txt 中定义
# 这里不再重复定义 mysql_recover_cli
