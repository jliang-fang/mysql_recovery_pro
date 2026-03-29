# src/CMakeLists.txt - MySQL 8.x Recovery Engine Source Files

# Add recovery module sources
set(RECOVERY_SOURCES
    recovery/engine.cpp
    recovery/blob_reconstructor.cpp
    recovery/rmrf_recovery.cpp
)

# Add repair module sources
set(REPAIR_SOURCES
    repair/repair.cpp
    repair/header_repairer.cpp
    repair/disk_scanner.cpp
    repair/crypt_scanner.cpp
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
)

# Add utils module sources
set(UTILS_SOURCES
    utils/cli_parser.cpp
    utils/file_mapper.cpp
    utils/innodb_checksum.cpp
    utils/zlib_helper.cpp
)

# Add all sources to dbrecovery_innodb library
target_sources(dbrecovery_innodb PRIVATE
    ${RECOVERY_SOURCES}
    ${REPAIR_SOURCES}
    ${EXPORT_SOURCES}
    ${ENGINE_SOURCES}
    ${UTILS_SOURCES}
)

# Add GUI sources
if(GUI_ENABLED)
    add_subdirectory(gui)
endif()
