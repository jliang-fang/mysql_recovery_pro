"""Version capabilities and explicit matrix expansion for disaster validation."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable


@dataclass(frozen=True)
class VersionCapability:
    version: str
    image: str
    image_digest: str
    page_sizes: frozenset[int]
    row_formats: frozenset[str]
    schema_sources: frozenset[str]
    supports_json: bool
    supports_generated_columns: bool
    supports_compression: bool
    supports_tde: bool
    connection_collation: str


@dataclass(frozen=True)
class MatrixItem:
    version: str
    filesystem: str
    scenario: str
    dataset_profile: str
    page_size: int
    row_format: str
    status: str
    reason: str = ""


CAPABILITIES = {
    "5.6": VersionCapability(
        version="5.6", image="localhost/mysql:5.6",
        image_digest="sha256:897086d07d1efa876224b147397ea8d3147e61dd84dce963aace1d5e9dc2802d",
        page_sizes=frozenset({4096, 8192, 16384}),
        row_formats=frozenset({"COMPACT", "REDUNDANT", "DYNAMIC", "COMPRESSED"}),
        schema_sources=frozenset({"FRM", "EXTERNAL"}), supports_json=False,
        supports_generated_columns=False, supports_compression=True, supports_tde=True,
        connection_collation="utf8mb4_unicode_ci",
    ),
    "5.7": VersionCapability(
        version="5.7", image="localhost/mysql:5.7",
        image_digest="sha256:dab0a802b44617303694fb17d166501de279c3031ddeb28c56ecf7fcab5ef0da",
        page_sizes=frozenset({4096, 8192, 16384, 32768, 65536}),
        row_formats=frozenset({"COMPACT", "REDUNDANT", "DYNAMIC", "COMPRESSED"}),
        schema_sources=frozenset({"FRM", "EXTERNAL"}), supports_json=True,
        supports_generated_columns=True, supports_compression=True, supports_tde=True,
        connection_collation="utf8mb4_unicode_ci",
    ),
    "8.0": VersionCapability(
        version="8.0", image="localhost/mysql:8.0",
        image_digest="sha256:62fb722c78b24245ddff1796a0fcee4a49cc5b87e0aaaf20c92d1da9e0a2497b",
        page_sizes=frozenset({4096, 8192, 16384, 32768, 65536}),
        row_formats=frozenset({"DYNAMIC", "COMPRESSED"}),
        schema_sources=frozenset({"SDI", "EXTERNAL"}), supports_json=True,
        supports_generated_columns=True, supports_compression=True, supports_tde=True,
        connection_collation="utf8mb4_0900_ai_ci",
    ),
    "8.4": VersionCapability(
        version="8.4", image="localhost/mysql:8.4",
        image_digest="sha256:80f4933e3835f9dc4d35a28ec500d7986cb4414e6c6821c5461239cb7beb8995",
        page_sizes=frozenset({4096, 8192, 16384, 32768, 65536}),
        row_formats=frozenset({"DYNAMIC", "COMPRESSED"}),
        schema_sources=frozenset({"SDI", "EXTERNAL"}), supports_json=True,
        supports_generated_columns=True, supports_compression=True, supports_tde=True,
        connection_collation="utf8mb4_0900_ai_ci",
    ),
    "9.0": VersionCapability(
        version="9.0", image="localhost/mysql:9.0",
        image_digest="sha256:0cf8b60f74e235c4dd4beb762b10ec6da3dce30265f5f1d40c2c11fa7872ebd9",
        page_sizes=frozenset({4096, 8192, 16384, 32768, 65536}),
        row_formats=frozenset({"DYNAMIC", "COMPRESSED"}),
        schema_sources=frozenset({"SDI", "EXTERNAL"}), supports_json=True,
        supports_generated_columns=True, supports_compression=True, supports_tde=True,
        connection_collation="utf8mb4_0900_ai_ci",
    ),
}


VALID_FILESYSTEMS = frozenset({"ext4", "xfs"})
VALID_SCENARIOS = frozenset({"baseline", "drop", "truncate", "rm-rf", "corruption-l1", "corruption-l2", "corruption-l3", "corruption-l4", "tde-unvalidated"})
VALID_DATASET_PROFILES = frozenset({"smoke", "100k", "1m"})


def matrix_item(version: str, filesystem: str, scenario: str, dataset_profile: str,
                page_size: int = 16384, row_format: str = "DYNAMIC") -> MatrixItem:
    capability = CAPABILITIES.get(version)
    if capability is None:
        return MatrixItem(version, filesystem, scenario, dataset_profile, page_size, row_format,
                          "UNSUPPORTED", "unknown_mysql_version")
    if filesystem not in VALID_FILESYSTEMS:
        return MatrixItem(version, filesystem, scenario, dataset_profile, page_size, row_format,
                          "UNSUPPORTED", "unknown_filesystem")
    if scenario not in VALID_SCENARIOS:
        return MatrixItem(version, filesystem, scenario, dataset_profile, page_size, row_format,
                          "UNSUPPORTED", "unknown_scenario")
    if dataset_profile not in VALID_DATASET_PROFILES:
        return MatrixItem(version, filesystem, scenario, dataset_profile, page_size, row_format,
                          "UNSUPPORTED", "unknown_dataset_profile")
    if page_size not in capability.page_sizes:
        return MatrixItem(version, filesystem, scenario, dataset_profile, page_size, row_format,
                          "UNSUPPORTED", "unsupported_page_size")
    if row_format not in capability.row_formats:
        return MatrixItem(version, filesystem, scenario, dataset_profile, page_size, row_format,
                          "UNSUPPORTED", "unsupported_row_format")
    if row_format == "COMPRESSED" and page_size > 16384:
        # A KEY_BLOCK_SIZE smaller than the page size is required, and InnoDB
        # refuses compressed tables entirely above 16 KiB pages ("Table storage
        # engine ... doesn't have this option"). The cell is unsupported, not a
        # skipped or failed case.
        return MatrixItem(version, filesystem, scenario, dataset_profile, page_size, row_format,
                          "UNSUPPORTED", "compressed_row_format_unsupported_above_16k_pages")
    if scenario == "tde-unvalidated" and not capability.supports_tde:
        return MatrixItem(version, filesystem, scenario, dataset_profile, page_size, row_format,
                          "UNSUPPORTED", "tde_not_supported")
    return MatrixItem(version, filesystem, scenario, dataset_profile, page_size, row_format,
                      "PLANNED")


def expand_matrix(versions: Iterable[str], filesystems: Iterable[str], scenarios: Iterable[str],
                  dataset_profiles: Iterable[str], page_size: int = 16384,
                  row_format: str = "DYNAMIC") -> list[MatrixItem]:
    return [
        matrix_item(version, filesystem, scenario, dataset_profile, page_size, row_format)
        for version in versions
        for filesystem in filesystems
        for scenario in scenarios
        for dataset_profile in dataset_profiles
    ]


def unsupported_cells(versions, filesystems, scenarios, dataset_profiles,
                      page_sizes, row_formats) -> list[MatrixItem]:
    """Every requested combination the version matrix rejects, with its reason."""
    cells = []
    for version in versions:
        for filesystem in filesystems:
            for scenario in scenarios:
                for profile in dataset_profiles:
                    for page_size in page_sizes:
                        for row_format in row_formats:
                            item = matrix_item(version, filesystem, scenario, profile,
                                               page_size, row_format)
                            if item.status == "UNSUPPORTED":
                                cells.append(item)
    return cells
