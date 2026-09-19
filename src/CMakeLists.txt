# src/CMakeLists.txt - MySQL 8.x Recovery Engine Source Files
# Enhanced for multi-version and multi-environment compatibility

include(CheckIncludeFileCXX)

# ==================== Common Sources ====================
set(COMMON_SOURCES
    common/version_compat.cpp
    common/environment_compat.cpp
    common/environment_compat_platform.cpp
    common/mysql_version_detector.cpp
)

# Add recovery module sources
set(RECOVERY_SOURCES
    recovery/engine.cpp
    recovery/blob_reconstructor.cpp
    recovery/rmrf_recovery.cpp
    recovery/blob_chain_rebuilder.cpp
    recovery/disaster_recovery.cpp
    recovery/truncate_recovery.cpp
    recovery/parallel_engine.cpp
)

# Add innodb module sources
set(INNODB_SOURCES
    innodb/compressed_table_recovery.cpp
    innodb/data_validator.cpp
    innodb/encrypted_table_recovery.cpp
    innodb/offline_keyring_parser.cpp
    innodb/shared_tablespace_recovery.cpp
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
    engine/blob_recovery.cpp
)

# Add utils module sources
set(UTILS_SOURCES
    utils/cli_parser.cpp
    utils/file_mapper.cpp
    utils/innodb_checksum.cpp
)

# Add tools module sources
set(TOOLS_SOURCES
    tools/heuristic_inference.cpp
    tools/interactive_schema_builder.cpp
    tools/progress_cli.cpp
    tools/recovery_cli.cpp
    tools/unified_recovery.cpp
    tools/result_contract.cpp
)

# Add salvage module sources (Phase 2: Row-level salvage for damaged pages)
set(SALVAGE_SOURCES
    salvage/damaged_page_handler.cpp
    salvage/row_salvage_scanner.cpp
    salvage/salvage_engine.cpp
    salvage/secondary_index_rescuer.cpp
    salvage/data_correlation.cpp
    salvage/evidence_serializer.cpp
)

# Add carve module sources (Phase 2: Disk carving page reassembly)
set(CARVE_SOURCES
    carve/page_reassembler.cpp
    carve/fragment_reassembler.cpp
    carve/page_identity_engine.cpp
    carve/tablespace_composer.cpp
)

# Add enterprise module sources (Phase 2: Partition + shared tablespace recovery)
set(ENTERPRISE_SOURCES
    enterprise/partition_recovery.cpp
    enterprise/offline_keyring.cpp
    enterprise/shared_tablespace.cpp
)

# Add report module sources (Phase 2: Recovery report generation)
set(REPORT_SOURCES
    report/recovery_report.cpp
)

# Add catalog module sources (Phase 2: fail-closed schema type conversion)
set(CATALOG_SOURCES
    catalog/schema_type_conversion.cpp
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

# Add inference module sources
set(INFERENCE_SOURCES
    inference/physical_layout_analyzer.cpp
    inference/statistical_classifier.cpp
    inference/feature_extractor.cpp
    inference/interactive_builder.cpp
    inference/layout_analyzer.cpp
    inference/ddl_change_detector.cpp
)

# Add safety module sources (Phase 2: Bare Device Safety Gate)
set(SAFETY_SOURCES
    safety/device_identity.cpp
    safety/loopback_safety.cpp
    safety/write_protect.cpp
    safety/safety_gate.cpp
)

# Add undo module sources (Phase 2: Undo log analysis & flashback)
set(UNDO_SOURCES
    undo/flashback_engine.cpp
    undo/undo_page_manager.cpp
)

# Add validate module sources (Phase 7: Integrated validation + capability report)
set(VALIDATE_SOURCES
    validate/unified_validator.cpp
)

# Add all sources to dbrecovery_innodb library
target_sources(dbrecovery_innodb PRIVATE
    ${COMMON_SOURCES}
    ${RECOVERY_SOURCES}
    ${INNODB_SOURCES}
    ${REPAIR_SOURCES}
    ${EXPORT_SOURCES}
    ${ENGINE_SOURCES}
    ${UTILS_SOURCES}
    ${TOOLS_SOURCES}
    ${PAGE_SOURCES}
    ${DISK_SOURCES}
    ${INFERENCE_SOURCES}
    ${SAFETY_SOURCES}
    ${UNDO_SOURCES}
    ${SALVAGE_SOURCES}
    ${CARVE_SOURCES}
    ${ENTERPRISE_SOURCES}
    ${REPORT_SOURCES}
    ${VALIDATE_SOURCES}
    ${CATALOG_SOURCES}
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
    # Check for linux/nvme_ioctl.h availability
    check_include_file_cxx("linux/nvme_ioctl.h" HAVE_LINUX_NVME_IOCTL_H)
    if(HAVE_LINUX_NVME_IOCTL_H)
        target_compile_definitions(dbrecovery_innodb PRIVATE
            HAVE_LINUX_NVME_IOCTL_H=1
        )
        message(STATUS "Found linux/nvme_ioctl.h - NVMe identity ioctl enabled")
    else()
        message(STATUS "linux/nvme_ioctl.h not found - NVMe identity will use sysfs fallback")
    endif()
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
target_link_libraries(dbrecovery_innodb PUBLIC dbrecovery_core dbrecovery_innodb_sources)

# ==================== GUI Support ====================
if(GUI_ENABLED)
    add_subdirectory(gui)
endif()

# ==================== CLI Executable ====================
# 主可执行文件在根目录 CMakeLists.txt 中定义
# 这里不再重复定义 mysql_recover_cli
