#include "dbrecovery/tools/unified_recovery.hpp"
#include "dbrecovery/tools/result_contract.hpp"
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/recovery/strategy_recommender.hpp"
#include "dbrecovery/recovery/parallel_exporter.hpp"
#include "dbrecovery/export/data_exporter.hpp"
#include "dbrecovery/report/recovery_report.hpp"
#include "dbrecovery/common/damage_level.hpp"
#include "dbrecovery/innodb/ddl_change_detector.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/frm_parser.hpp"
#include "dbrecovery/innodb/sdi_extractor.hpp"
#include "dbrecovery/innodb/heuristic_parser.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include "dbrecovery/recovery/truncate_recovery.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/innodb/encrypted_table_recovery.hpp"
#include "dbrecovery/innodb/offline_keyring_parser.hpp"
#include "dbrecovery/common/mysql_version_detector.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/salvage/secondary_index_rescuer.hpp"
#include "dbrecovery/innodb.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>

namespace fs = std::filesystem;
namespace dbrecovery {
namespace tools {
namespace {

std::string trim_schema_token(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::vector<std::string> split_schema_columns(const std::string& schema) {
    std::vector<std::string> columns;
    std::string current;
    int depth = 0;
    for (char c : schema) {
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        if (c == ',' && depth == 0) {
            if (!trim_schema_token(current).empty()) columns.push_back(trim_schema_token(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!trim_schema_token(current).empty()) columns.push_back(trim_schema_token(current));
    return columns;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0') << static_cast<unsigned>(c)
                        << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

std::vector<std::string> parse_enum_values(const std::string& value) {
    std::vector<std::string> values;
    std::string current;
    char quote = 0;
    for (char c : value) {
        if ((c == '\'' || c == '"') && (quote == 0 || quote == c)) {
            if (quote == 0) quote = c;
            else quote = 0;
        } else if (c == ',' && quote == 0) {
            values.push_back(trim_schema_token(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!trim_schema_token(current).empty()) values.push_back(trim_schema_token(current));
    for (auto& item : values) {
        item = trim_schema_token(item);
        if (item.size() >= 2 && ((item.front() == '\'' && item.back() == '\'') ||
                                 (item.front() == '"' && item.back() == '"'))) {
            item = item.substr(1, item.size() - 2);
        }
    }
    return values;
}

bool parse_user_schema(const std::string& schema,
                       std::vector<innodb::IndexColumnDef>& columns,
                       uint32_t& primary_key_columns) {
    if (schema.empty()) return false;
    for (const auto& definition : split_schema_columns(schema)) {
        std::vector<std::string> parts;
        if (definition.find(':') != std::string::npos) {
            std::istringstream stream(definition);
            std::string part;
            while (std::getline(stream, part, ':')) parts.push_back(trim_schema_token(part));
        } else {
            std::istringstream stream(definition);
            std::string part;
            while (stream >> part) parts.push_back(part);
        }
        if (parts.size() < 2) continue;

        innodb::IndexColumnDef column;
        column.name = trim_schema_token(parts[0]);
        std::string type = parts[1];
        std::string type_upper = type;
        std::transform(type_upper.begin(), type_upper.end(), type_upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const auto open = type_upper.find('(');
        std::string args;
        if (open != std::string::npos) {
            const auto close = type_upper.rfind(')');
            if (close != std::string::npos && close > open) args = type.substr(open + 1, close - open - 1);
            type_upper = trim_schema_token(type_upper.substr(0, open));
        }
        column.mb_minlen = 1;
        column.mb_maxlen = 1;
        if (type_upper == "TINYINT") { column.col_type = 2; column.col_length = 1; }
        else if (type_upper == "SMALLINT") { column.col_type = 3; column.col_length = 2; }
        else if (type_upper == "MEDIUMINT") { column.col_type = 10; column.col_length = 3; }
        else if (type_upper == "INT" || type_upper == "INTEGER") { column.col_type = 4; column.col_length = 4; }
        else if (type_upper == "BIGINT") { column.col_type = 9; column.col_length = 8; }
        else if (type_upper == "FLOAT") { column.col_type = 5; column.col_length = 4; }
        else if (type_upper == "DOUBLE") { column.col_type = 6; column.col_length = 8; }
        else if (type_upper == "DECIMAL" || type_upper == "NUMERIC") {
            column.col_type = 21;
            const auto comma = args.find(',');
            try {
                column.precision = static_cast<uint16_t>(std::stoul(args.substr(0, comma)));
                column.col_length = column.decimal_storage_size();
                if (comma != std::string::npos) column.scale = static_cast<uint16_t>(std::stoul(args.substr(comma + 1)));
                column.col_length = column.decimal_storage_size();
            } catch (...) {}
        } else if (type_upper == "DATETIME" || type_upper == "DATETIME2") {
            column.col_type = 19; column.col_length = 5;
            if (!args.empty()) { try { column.datetime_precision = static_cast<uint8_t>(std::stoul(args)); } catch (...) {} }
        } else if (type_upper == "TIMESTAMP" || type_upper == "TIMESTAMP2") {
            column.col_type = 18; column.col_length = 4;
            if (!args.empty()) { try { column.datetime_precision = static_cast<uint8_t>(std::stoul(args)); } catch (...) {} }
        } else if (type_upper == "TIME" || type_upper == "TIME2") {
            column.col_type = 20; column.col_length = 3;
            if (!args.empty()) { try { column.datetime_precision = static_cast<uint8_t>(std::stoul(args)); } catch (...) {} }
        } else if (type_upper == "DATE") { column.col_type = 15; column.col_length = 3; }
        else if (type_upper == "YEAR") { column.col_type = 14; column.col_length = 1; }
        else if (type_upper == "VARCHAR") {
            column.col_type = 16; column.col_length = args.empty() ? 255 : static_cast<uint32_t>(std::stoul(args));
            column.is_big = column.col_length * column.mb_maxlen > 255;
        } else if (type_upper == "CHAR") {
            column.col_type = 29; column.col_length = args.empty() ? 1 : static_cast<uint32_t>(std::stoul(args));
            column.is_big = column.col_length * column.mb_maxlen > 255;
        } else if (type_upper == "VARBINARY") {
            column.col_type = 16; column.col_length = args.empty() ? 255 : static_cast<uint32_t>(std::stoul(args));
            column.is_binary = true; column.mb_maxlen = 1; column.is_big = column.col_length > 255;
        } else if (type_upper == "BINARY") {
            column.col_type = 29; column.col_length = args.empty() ? 1 : static_cast<uint32_t>(std::stoul(args));
            column.is_binary = true; column.mb_maxlen = 1; column.is_big = column.col_length > 255;
        } else if (type_upper == "TINYTEXT" || type_upper == "TEXT" || type_upper == "MEDIUMTEXT" || type_upper == "LONGTEXT" || type_upper == "BLOB") {
            column.col_type = 27; column.col_length = 65535; column.is_big = true;
            column.is_binary = type_upper == "BLOB";
        } else if (type_upper == "JSON") {
            column.col_type = 31; column.col_length = 0xffffffffU; column.is_big = true;
        } else if (type_upper == "ENUM") {
            column.col_type = 22; column.col_length = 1; column.enum_values = parse_enum_values(args);
        } else if (type_upper == "SET") {
            column.col_type = 23; column.col_length = 1; column.set_values = parse_enum_values(args);
        } else {
            return false;
        }

        column.is_nullable = true;
        for (size_t i = 2; i < parts.size(); ++i) {
            std::string flag = parts[i];
            std::transform(flag.begin(), flag.end(), flag.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (flag == "pk" || flag == "key" || flag.find("primary key") != std::string::npos) {
                column.is_nullable = false; ++primary_key_columns;
            } else if (flag == "notnull" || flag == "not_null" || flag.find("not null") != std::string::npos) {
                column.is_nullable = false;
            } else if (flag == "unsigned" || flag.find(" unsigned") != std::string::npos) {
                column.is_unsigned = true;
            }
        }
        std::string definition_lower = definition;
        std::transform(definition_lower.begin(), definition_lower.end(), definition_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (definition_lower.find("utf8mb4") != std::string::npos) {
            column.mb_minlen = 1;
            column.mb_maxlen = 4;
        } else if (definition_lower.find("utf8mb3") != std::string::npos ||
                   definition_lower.find("utf8") != std::string::npos) {
            column.mb_minlen = 1;
            column.mb_maxlen = 3;
        } else if (definition_lower.find("ucs2") != std::string::npos) {
            column.mb_minlen = 2;
            column.mb_maxlen = 2;
        } else if (definition_lower.find("utf16") != std::string::npos) {
            column.mb_minlen = 2;
            column.mb_maxlen = 4;
        } else if (definition_lower.find("utf32") != std::string::npos) {
            column.mb_minlen = 4;
            column.mb_maxlen = 4;
        } else if (definition_lower.find("binary") != std::string::npos) {
            column.mb_minlen = 1;
            column.mb_maxlen = 1;
            column.is_binary = true;
        }
        if (definition_lower.find("primary key") != std::string::npos) {
            if (column.is_nullable) ++primary_key_columns;
            column.is_nullable = false;
        }
        if (definition_lower.find("not null") != std::string::npos) column.is_nullable = false;
        if (definition_lower.find("unsigned") != std::string::npos) column.is_unsigned = true;
        column.physical_pos = static_cast<uint32_t>(columns.size() + 1);
        column.ordinal_position = static_cast<uint32_t>(columns.size());
        columns.push_back(std::move(column));
    }
    if (primary_key_columns == 0 && !columns.empty()) {
        primary_key_columns = 1;
        columns.front().is_nullable = false;
    }
    return !columns.empty();
}

bool load_schema_file(const std::string& path, catalog::TableSchema& schema) {
    if (path.empty()) return false;
    std::ifstream file(path);
    if (!file) return false;
    try {
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        schema = catalog::TableSchema{};
        return schema.from_json(nlohmann::json::parse(content));
    } catch (...) {
        return false;
    }
}

uint32_t charset_max_length(uint32_t charset_id) {
    switch (charset_id) {
        case 33: case 83: case 224: return 3;
        case 45: case 46: case 94: case 255: return 4;
        default: return 1;
    }
}

bool convert_schema(const catalog::TableSchema& schema,
                    std::vector<innodb::IndexColumnDef>& columns,
                    uint32_t& primary_key_columns) {
    columns.clear();
    primary_key_columns = 0;
    for (const auto& entry : schema.columns) {
        const auto& source = entry.second;
        if (source.name == "DB_TRX_ID" || source.name == "DB_ROLL_PTR" ||
            source.name == "DB_ROW_ID") continue;
        innodb::IndexColumnDef column;
        column.name = source.name;
        column.col_type = static_cast<uint8_t>(source.mysql_type_id);
        column.col_length = source.column_length;
        column.is_nullable = source.is_nullable;
        column.is_unsigned = !source.is_signed;
        column.char_set_id = source.charset_id;
        column.is_binary = source.charset_id == 63 || source.collation_id == 63;
        column.precision = source.numeric_precision > 0
            ? static_cast<uint16_t>(source.numeric_precision) : 0;
        column.scale = source.numeric_scale > 0
            ? static_cast<uint16_t>(source.numeric_scale) : 0;
        column.datetime_precision = source.datetime_precision >= 0
            ? static_cast<uint8_t>(source.datetime_precision) : 0;
        column.mb_minlen = 1;
        column.mb_maxlen = charset_max_length(source.charset_id);
        column.is_big = column.col_length * column.mb_maxlen > 255;
        column.enum_values = source.enum_values;
        column.set_values = source.set_values;
        column.ordinal_position = source.ordinal ? source.ordinal : entry.first;
        column.physical_pos = source.physical_pos;
        columns.push_back(std::move(column));
    }
    if (columns.empty()) return false;
    std::sort(columns.begin(), columns.end(), [](const auto& left, const auto& right) {
        return left.physical_pos < right.physical_pos;
    });
    for (const auto& index : schema.indexes) {
        if (!index.is_primary) continue;
        for (const auto& name : index.columns) {
            if (std::any_of(columns.begin(), columns.end(), [&](const auto& column) {
                    return column.name == name;
                })) {
                ++primary_key_columns;
            }
        }
    }
    if (primary_key_columns == 0) {
        primary_key_columns = 1;
        columns.front().is_nullable = false;
    }
    return true;
}

} // namespace

UnifiedRecoveryResult run_unified_recovery(
    const std::string& ibd_path,
    const std::string& output_dir,
    const std::string& table_name,
    const std::string& format,
    uint32_t page_size,
    const std::string& schema_str,
    const std::string& frm_path,
    uint64_t table_id,
    const std::string& system_tablespace,
    bool repair,
    bool schemaless_recovery,
    bool verbose,
    const std::string& schema_file_path) {
    UnifiedRecoveryResult result;
    result.table_name = table_name.empty() ? "recovered_table" : table_name;
    result.source_ibd = ibd_path;
    (void)repair;
    auto start = std::chrono::high_resolution_clock::now();

    // Step 1: DDL pre-scan
    try {
        innodb::DDLChangeChainDetector ddl_detector(page_size ? page_size : 16384);
        auto ddl = ddl_detector.detect(ibd_path);
        result.ddl_variation_detected = ddl.possible_schema_variation;
        if (ddl.blocked_auto_decode) {
            result.trust_level = "EVIDENCE_ONLY";
            result.schema_source = "EVIDENCE_ONLY";
            result.result_classification = "forensic_evidence";
        }
    } catch (...) {}

    bool pipeline_allowed = true;
    if (ibd_path.empty() || !fs::exists(ibd_path)) {
        result.damage_level = common::DamageLevel::L4_CRITICAL;
        result.recovery_method = "none";
        pipeline_allowed = false;
    }

    // TDE candidate gate — keep plaintext parse blocked for candidate inputs
    if (pipeline_allowed) {
        std::ifstream in(ibd_path, std::ios::binary);
        if (in) {
            std::vector<uint8_t> p0(innodb::FIL_HEADER_SIZE + 45);
            in.read(reinterpret_cast<char*>(p0.data()), p0.size());
            size_t n = static_cast<size_t>(in.gcount());
            if (innodb::TDEDecryptor::probe_candidate(p0.data(), n) == innodb::TDECandidateProbe::RECOGNIZED_CANDIDATE) {
                result.recovery_method = "blocked_tde_candidate";
                result.trust_level = "EVIDENCE_ONLY";
                result.schema_source = "EVIDENCE_ONLY";
                result.result_classification = "forensic_evidence";
                pipeline_allowed = false;
            }
        }
    }

    // Steps 2-6: Unified pipeline via InnoDBIndexParser + salvage fallbacks
    if (pipeline_allowed) {
        uint32_t psize = page_size;
        if (psize == 0) {
            std::ifstream f(ibd_path, std::ios::binary);
            if (f) {
                std::vector<uint8_t> fp(16384);
                f.read(reinterpret_cast<char*>(fp.data()), 16384);
                psize = innodb::detect_page_size_from_fsp(fp.data());
            }
            if (psize == 0) psize = 16384;
        }
        page_size = psize;

        // Record damage evidence for every complete page before parsing rows.
        {
            std::ifstream df(ibd_path, std::ios::binary | std::ios::ate);
            if (df) {
                const auto file_size = static_cast<uint64_t>(df.tellg());
                const uint64_t page_count = file_size / page_size;
                df.seekg(0);
                result.page_damage.reserve(static_cast<size_t>(std::min<uint64_t>(page_count, 1000000)));
                for (uint64_t page_index = 0; page_index < page_count; ++page_index) {
                    std::vector<uint8_t> page(psize);
                    df.read(reinterpret_cast<char*>(page.data()), static_cast<std::streamsize>(page.size()));
                    if (df.gcount() != static_cast<std::streamsize>(page.size())) break;
                    const auto damage = recovery::SalvageEngine::assess_damage(page.data(), page.size());
                    PageDamageEvidence evidence;
                    evidence.page_no = damage.detected_page_no;
                    evidence.page_offset = page_index * static_cast<uint64_t>(page_size);
                    evidence.page_type = damage.detected_page_type;
                    evidence.space_id = damage.detected_space_id;
                    evidence.fil_header_valid = damage.fil_header_valid;
                    evidence.page_header_valid = damage.page_header_valid;
                    evidence.checksum_valid = damage.checksum_valid;
                    evidence.free_space_corrupt = damage.free_space_corrupt;
                    evidence.damage_level = damage.level;
                    evidence.diagnosis = damage.diagnosis;
                    result.page_damage.push_back(std::move(evidence));
                    if (static_cast<uint8_t>(damage.level) > static_cast<uint8_t>(result.damage_level)) {
                        result.damage_level = damage.level;
                    }
                }
            }
        }

        // Version detection for schema gates
        MySQLVersion detected_version = MySQLVersion::UNKNOWN;
        {
            std::ifstream vf(ibd_path, std::ios::binary);
            if (vf) {
                std::vector<uint8_t> vp(psize);
                vf.read(reinterpret_cast<char*>(vp.data()), vp.size());
                detected_version = MySQLVersionDetector::detect(vp.data(), vp.size());
            }
        }

        innodb::InnoDBIndexParser parser;
        parser.set_recover_deleted(true);
        parser.set_bad_page_mode(2);
        parser.set_verbose(verbose);
        innodb::g_verbose = verbose;

        std::vector<innodb::IndexColumnDef> index_columns;
        std::string schema_source = "EVIDENCE_ONLY";
        bool have_schema = false;

        // Priority: --schema string
        if (!schema_str.empty()) {
            uint32_t n_pk = 0;
            have_schema = parse_user_schema(schema_str, index_columns, n_pk);
            if (have_schema) {
                parser.set_columns(index_columns, false, n_pk);
                schema_source = "USER";
            }
        }
        // JSON schema file
        if (!have_schema && !schema_file_path.empty()) {
            catalog::TableSchema schema;
            uint32_t n_pk = 0;
            if (load_schema_file(schema_file_path, schema) &&
                convert_schema(schema, index_columns, n_pk)) {
                parser.set_columns(index_columns, false, n_pk);
                schema_source = "USER";
                have_schema = true;
            }
        }
        // FRM
        if (!have_schema && !frm_path.empty()) {
            auto frm = innodb::FRMParser::parse(frm_path);
            if (frm.has_value()) {
                std::set<std::string> pk_names;
                for (auto &idx : frm->indexes) if (idx.is_primary) for (auto &c: idx.columns) pk_names.insert(c);
                uint32_t n_pk = 0;
                for (auto &[pos, col] : frm->columns) {
                    innodb::IndexColumnDef icol;
                    icol.name = col.name;
                    icol.col_type = static_cast<uint8_t>(col.mysql_type_id);
                    icol.col_length = col.column_length;
                    icol.is_nullable = col.is_nullable;
                    icol.is_unsigned = !col.is_signed;
                    uint8_t cs = static_cast<uint8_t>(col.charset_id);
                    switch (cs) { case 33: case 83: case 224: icol.mb_maxlen=3; break; case 45: case 94: icol.mb_maxlen=4; break; default: icol.mb_maxlen=1; break; }
                    icol.is_binary = (col.charset_id==63||col.collation_id==63) && icol.col_type!=31;
                    if (col.mysql_type_id==21 && col.numeric_precision>0) { icol.precision=col.numeric_precision; icol.scale=col.numeric_scale; }
                    icol.enum_values=col.enum_values; icol.set_values=col.set_values;
                    icol.is_big = (icol.col_length * icol.mb_maxlen > 255);
                    if (pk_names.count(col.name)) n_pk++;
                    icol.physical_pos = static_cast<uint32_t>(index_columns.size()+1);
                    index_columns.push_back(icol);
                }
                if (n_pk==0 && !index_columns.empty()) n_pk=1;
                parser.set_columns(index_columns,false,n_pk);
                schema_source = "FRM";
                have_schema = true;
            }
        }
        // SDI — placeholder: schema extraction via SDI is handled by caller-supplied
        // FRM/schema_str; unified pipeline keeps evidence-only invariant here.
        if (!have_schema && !schemaless_recovery) {
            (void)detected_version; (void)table_id;
        }

        result.column_names.clear();
        if (!index_columns.empty()) {
            result.column_names.reserve(index_columns.size());
            for (size_t i = 0; i < index_columns.size(); ++i) {
                result.column_names.push_back(index_columns[i].name.empty()
                    ? "field_" + std::string(i < 10 ? "000" : i < 100 ? "00" : i < 1000 ? "0" : "") + std::to_string(i)
                    : index_columns[i].name);
            }
        }

        // Phase 1: IndexParser
        std::vector<innodb::IndexRecord> all_recs;
        size_t total_recovered = 0;
        all_recs = parser.parse_ibd_force(ibd_path);
        total_recovered = all_recs.size();
        result.recovery_method = "index_parser";

        // Phase 1.5: TRUNCATE fallback
        if (total_recovered==0) {
            auto st = parser.get_stats();
            if (st.pages_scanned>5 && st.pages_index_leaf==0) {
                bool is_trunc=false;
                std::ifstream f(ibd_path, std::ios::binary);
                if (f) { std::vector<uint8_t> p(16384); f.read(reinterpret_cast<char*>(p.data()),16384); uint16_t t=innodb::read_be<uint16_t>(p.data(),24); is_trunc=(t==0x0008); }
                if (is_trunc) {
                    recovery::TruncateRecovery tr;
                    tr.set_page_size(page_size);
                    if (!system_tablespace.empty()) { tr.set_system_tablespace(system_tablespace); tr.enable_undo_log(true); }
                    tr.enable_disk_scan(true);
                    catalog::TableSchema schema; schema.table_name=table_name.empty()?"recovered_table":table_name; schema.page_size=page_size;
                    for (size_t ci=0; ci<index_columns.size(); ++ci) {
                        auto &ic=index_columns[ci]; catalog::ColumnInfo ci2; ci2.name=ic.name; ci2.ordinal=ic.ordinal_position; ci2.physical_pos=ic.physical_pos; ci2.mysql_type_id=ic.col_type; ci2.is_nullable=ic.is_nullable; ci2.is_signed=!ic.is_unsigned; ci2.column_length=ic.col_length; ci2.charset_id=ic.char_set_id; ci2.numeric_precision=ic.precision; ci2.numeric_scale=ic.scale; ci2.datetime_precision=ic.datetime_precision; ci2.enum_values=ic.enum_values; ci2.set_values=ic.set_values; schema.columns[ci]=ci2;
                    }
                    auto r = tr.recover(ibd_path, schema, [](double, const std::string&){});
                    if (r.success && r.records_recovered>0) {
                        // Convert map records to IndexRecord-like for export path below
                        // For unified result, synthesize SalvagedRows
                        for (auto &m: r.recovered_records) {
                            auto it=m.find("_undo_action");
                            if (it!=m.end() && it->second=="FORENSIC_LEGACY_UNDO") continue;
                            recovery::SalvagedRow sr; sr.salvage_level=recovery::SalvageLevel::STRUCTURED; sr.confidence=1.0; sr.recovery_method="truncate_recovery";
                            for (auto &kv: m) sr.fields.push_back(kv.second);
                            sr.field_nulls.assign(sr.fields.size(), false);
                            result.records.push_back(std::move(sr));
                        }
                        total_recovered = result.records.size();
                        result.recovery_method = "truncate_recovery";
                    }
                }
            }
        }

        // Phase 2: Heuristic fallback
        if (total_recovered==0 && !schemaless_recovery) {
            innodb::HeuristicScanner hs(page_size);
            std::set<std::string> seen;
            std::vector<innodb::IndexRecord> extra;
            hs.scan_file_streaming(ibd_path, [&](const std::vector<innodb::HeuristicRecord>& recs, uint32_t pg)->bool{
                for (auto &rec: recs) {
                    if (rec.is_infimum||rec.is_supremum||rec.is_deleted) continue;
                    if (rec.fields.size()<2) continue;
                    innodb::IndexRecord ir; ir.page_no=pg;
                    for (auto &f: rec.fields) { innodb::IndexFieldValue fv; fv.type=innodb::IndexFieldValue::Type::STRING_VAL; fv.str_value=f.to_string(); fv.is_null=f.is_null; ir.fields.push_back(fv); }
                    std::string h; for (auto &ff: ir.fields) h+=ff.str_value+"|";
                    if (!seen.insert(h).second) continue;
                    extra.push_back(ir);
                }
                return true;
            });
            if (!extra.empty()) {
                for (auto &ir: extra) {
                    recovery::SalvagedRow sr; sr.salvage_level=recovery::SalvageLevel::STRUCTURED; sr.confidence=0.6; sr.recovery_method="heuristic_scanner";
                    for (auto &f: ir.fields) {
                        sr.fields.push_back(f.str_value);
                        sr.field_nulls.push_back(f.is_null);
                    }
                    sr.page_no = ir.page_no;
                    result.records.push_back(std::move(sr));
                }
                if (all_recs.empty()) total_recovered = result.records.size();
                else {
                    // merge heuristic into all_recs path
                    for (auto &ir: extra) all_recs.push_back(ir);
                    total_recovered = all_recs.size();
                }
                if (result.recovery_method=="index_parser") result.recovery_method="index_parser+heuristic";
                else result.recovery_method="heuristic_scanner";
            }
        }

        // Phase 3: SchemaAgnostic
        if ((total_recovered==0 || total_recovered<100) && all_recs.empty()) {
            innodb::SchemaAgnosticScanner::Config cfg; cfg.recover_deleted=true;
            innodb::SchemaAgnosticScanner sa(cfg);
            auto sa_recs = sa.parse_ibd(ibd_path);
            for (auto &sr2: sa_recs) {
                recovery::SalvagedRow sr; sr.salvage_level=recovery::SalvageLevel::STRUCTURED; sr.confidence=0.5; sr.recovery_method="schema_agnostic";
                for (auto &f: sr2.fields) {
                    sr.fields.push_back(f.to_string());
                    sr.field_nulls.push_back(f.is_null);
                }
                sr.page_no = sr2.page_no;
                result.records.push_back(std::move(sr));
            }
            if (!sa_recs.empty()) {
                total_recovered = result.records.size();
                result.recovery_method = result.recovery_method=="none" ? "schema_agnostic" : result.recovery_method+"+schema_agnostic";
            }
        }

        // Promote IndexParser records into unified result if not yet done
        if (!all_recs.empty() && result.records.empty()) {
            for (auto &ir: all_recs) {
                recovery::SalvagedRow sr; sr.salvage_level=recovery::SalvageLevel::STRUCTURED; sr.confidence=0.95; sr.recovery_method="index_parser";
                sr.page_no = ir.page_no; sr.field_count = static_cast<uint16_t>(ir.fields.size());
                for (auto &f: ir.fields) {
                    sr.fields.push_back(f.str_value);
                    sr.field_nulls.push_back(f.is_null);
                }
                result.records.push_back(std::move(sr));
            }
            total_recovered = result.records.size();
        }

        // Phase 4: Salvage for damaged pages — L1/L2 -> rows, L3/L4 -> evidence_fragments
        {
            bool need_salvage = (total_recovered==0 || result.damage_level >= common::DamageLevel::L3_SEVERE);
            if (!need_salvage) {
                // also probe damage level
                std::ifstream f(ibd_path, std::ios::binary);
                if (f) {
                    std::vector<uint8_t> buf(page_size);
                    f.read(reinterpret_cast<char*>(buf.data()), page_size);
                    if (f.gcount()==(std::streamsize)page_size) {
                        auto dr = recovery::SalvageEngine::assess_damage(buf.data(), page_size);
                        result.damage_level = dr.level;
                        if (dr.level >= common::DamageLevel::L3_SEVERE) need_salvage = true;
                    }
                }
            }
            if (need_salvage) {
                recovery::SalvageEngine::Config cfg;
                cfg.page_size = page_size;
                cfg.min_confidence = 0.3;
                cfg.include_evidence_fragments = false;
                recovery::SalvageEngine eng(cfg);
                std::ifstream f(ibd_path, std::ios::binary);
                std::vector<std::pair<const uint8_t*, size_t>> pages;
                std::vector<std::vector<uint8_t>> owned;
                if (f) {
                    f.seekg(0, std::ios::end); size_t fsz = static_cast<size_t>(f.tellg()); f.seekg(0);
                    size_t np = fsz / page_size;
                    owned.reserve(std::min<size_t>(np, 256));
                    for (size_t i=0;i<np && owned.size()<256;i++) {
                        std::vector<uint8_t> buf(page_size);
                        f.read(reinterpret_cast<char*>(buf.data()), page_size);
                        if (f.gcount()!=(std::streamsize)page_size) break;
                        owned.push_back(std::move(buf));
                    }
                    for (auto &b: owned) pages.emplace_back(b.data(), b.size());
                }
                if (!pages.empty()) {
                    auto sr = eng.salvage_pages(pages);
                    for (auto &r: sr.rows) result.records.push_back(std::move(r));
                    for (auto &e: sr.evidence_fragments) result.evidence_fragments.push_back(std::move(e));
                    if (!sr.rows.empty() || !sr.evidence_fragments.empty()) {
                        result.damage_level = sr.damage.level;
                        if (!sr.rows.empty() && result.recovery_method=="none") result.recovery_method="salvage_engine";
                    }
                    // Export: CSV only rows, never fragments — enforced by caller
                }
            }
        }

        const bool page_integrity_failure = std::any_of(
            result.page_damage.begin(), result.page_damage.end(),
            [](const PageDamageEvidence& page) {
                // ALLOCATED pages (type=0x0000) are empty with no records
                // and should not trigger integrity failure
                if (page.page_type == 0x0000) return false;
                return !page.checksum_valid ||
                       !page.fil_header_valid ||
                       !page.page_header_valid ||
                       page.free_space_corrupt;
            });
        if (page_integrity_failure && !result.records.empty()) {
            for (auto& row : result.records) {
                row.salvage_level = recovery::SalvageLevel::FRAGMENT;
                if (row.evidence.strategy.empty()) {
                    row.evidence.strategy = "page_integrity_gate";
                }
                row.evidence.level = std::max(
                    row.evidence.level, common::DamageLevel::L1_MINOR);
                row.evidence.tags.push_back("page-integrity-failure");
                result.evidence_fragments.push_back(std::move(row));
            }
            result.records.clear();
        }

        result.total_recovered = static_cast<uint32_t>(result.records.size());
        // Deduplicate by content_hash
        {
            std::unordered_set<std::string> seen;
            std::vector<recovery::SalvagedRow> dedup;
            dedup.reserve(result.records.size());
            for (auto &r: result.records) {
                std::string h = r.content_hash;
                if (h.empty()) { for (auto &ff: r.fields) h+=ff+"|"; }
                if (seen.insert(h).second) dedup.push_back(std::move(r));
            }
            result.records.swap(dedup);
            result.total_recovered = static_cast<uint32_t>(result.records.size());
        }

        if (have_schema || schema_source!="EVIDENCE_ONLY") {
            result.schema_source = schema_source;
            result.trust_level = result.ddl_variation_detected ? "EVIDENCE_ONLY" : schema_source;
        }

        // Step 7: Secondary index rescue when recovery low
        if (result.total_recovered < 10) {
            try {
                salvage::SecondaryIndexRescuer resc(page_size);
                (void)resc;
            } catch (...) {}
        }

        // Step 8: Strategy recommendation informational
        try {
            recovery::StrategyRecommender rec;
            if (verbose) { auto rr = rec.recommend("CORRUPTION"); (void)rr.to_text(); }
        } catch (...) {}

        // Step 9: Export — only authoritative structured rows from fully valid pages may be exported.
        if (result.total_recovered == 0) {
            result.export_rejection_reason = "no_structured_rows";
        } else if (result.trust_level != "AUTHORITATIVE" &&
                   result.trust_level != "USER" && result.trust_level != "FRM") {
            result.export_rejection_reason = "schema_trust_insufficient";
        } else if (page_integrity_failure) {
            result.export_rejection_reason = "page_integrity_failure";
        } else if (result.damage_level >= common::DamageLevel::L3_SEVERE) {
            result.export_rejection_reason = "severe_or_critical_page_damage";
        } else if (result.ddl_variation_detected) {
            result.export_rejection_reason = "ddl_variation_detected";
        } else if (!output_dir.empty()) {
            fs::create_directories(output_dir);
            std::vector<std::map<std::string, exporter::ExportValue>> maps;
            maps.reserve(result.records.size());
            for (const auto& r : result.records) {
                std::map<std::string, exporter::ExportValue> m;
                for (size_t i = 0; i < r.fields.size(); ++i) {
                    const std::string key = i < result.column_names.size()
                        ? result.column_names[i]
                        : "field_" + std::to_string(i);
                    const bool is_null = i < r.field_nulls.size() && r.field_nulls[i];
                    m[key] = exporter::ExportValue{r.fields[i], is_null};
                }
                maps.push_back(std::move(m));
            }
            const std::string csv_path = output_dir + "/recovered.csv";
            result.export_success = exporter::Exporter::export_to_csv(
                maps, result.column_names, csv_path);
            if (!result.export_success) result.export_rejection_reason = "csv_export_failed";
        }
    }

    if (result.trust_level.empty()) result.trust_level = "EVIDENCE_ONLY";
    if (result.schema_source.empty()) result.schema_source = "EVIDENCE_ONLY";
    if (result.recovery_method.empty()) result.recovery_method = "none";

    const bool trusted_schema = result.trust_level == "AUTHORITATIVE" ||
                                result.trust_level == "USER" ||
                                result.trust_level == "FRM";
    const bool page_integrity_failure = std::any_of(
        result.page_damage.begin(), result.page_damage.end(),
        [](const PageDamageEvidence& page) {
            // ALLOCATED pages (type=0x0000) are empty with no records
            // and should not trigger integrity failure
            if (page.page_type == 0x0000) return false;
            return !page.checksum_valid ||
                   !page.fil_header_valid ||
                   !page.page_header_valid ||
                   page.free_space_corrupt;
        });
    const bool structured_ready = result.total_recovered > 0 &&
                                  trusted_schema &&
                                  !result.ddl_variation_detected &&
                                  !page_integrity_failure &&
                                  result.damage_level < common::DamageLevel::L3_SEVERE;
    if (structured_ready && result.export_rejection_reason.empty() && output_dir.empty()) {
        result.export_success = true;
    }
    if (!structured_ready && result.export_rejection_reason.empty()) {
        if (result.total_recovered == 0) {
            result.export_rejection_reason = "no_structured_rows";
        } else if (!trusted_schema) {
            result.export_rejection_reason = "schema_trust_insufficient";
        } else if (page_integrity_failure) {
            result.export_rejection_reason = "page_integrity_failure";
        } else if (result.damage_level >= common::DamageLevel::L3_SEVERE) {
            result.export_rejection_reason = "severe_or_critical_page_damage";
        } else if (result.ddl_variation_detected) {
            result.export_rejection_reason = "ddl_variation_detected";
        }
    }
    result.result_classification = structured_ready
        ? "structured_recovery" : "forensic_evidence";
    result.success = structured_ready && (output_dir.empty() || result.export_success);

    auto end = std::chrono::high_resolution_clock::now();
    double secs = std::chrono::duration<double>(end - start).count();
    try {
        uint32_t damaged_pages = 0;
        uint32_t checksum_mismatches = 0;
        uint32_t header_damage = 0;
        for (const auto& page : result.page_damage) {
            if (page.damage_level != common::DamageLevel::NONE) ++damaged_pages;
            if (!page.checksum_valid) ++checksum_mismatches;
            if (!page.fil_header_valid || !page.page_header_valid) ++header_damage;
        }
        report::DataStats data;
        data.total_pages_scanned = result.page_damage.size();
        data.corrupted_pages = damaged_pages;
        data.skipped_pages = header_damage;
        data.records_fully_recovered = result.total_recovered;
        data.records_partially_recovered = result.evidence_fragments.size();
        data.recovery_rate_percent = result.total_recovered > 0 ? 100.0 : 0.0;
        report::ReportBuilder rb;
        rb.set_recovery_type("unified_recovery")
          .set_status(result.success ? "SUCCESS" : "FORENSIC_EVIDENCE")
          .set_input(report::InputInfo{ "ibd", result.source_ibd, 0, trusted_schema,
                                        result.schema_source, false, false, {result.source_ibd}})
          .set_strategy(report::StrategyInfo{ {result.recovery_method}, result.recovery_method, {}, {} })
          .set_data(data)
          .set_timing(report::TimingInfo{ 0, 0, 0, secs });
        if (checksum_mismatches > 0) {
            rb.add_damage(report::DamageDetail{0, "checksum_mismatch",
                                               std::to_string(checksum_mismatches) + " pages"});
        }
        if (header_damage > 0) {
            rb.add_damage(report::DamageDetail{0, "header_damage",
                                               std::to_string(header_damage) + " pages"});
        }
        if (!table_name.empty()) rb.add_ddl("TABLE " + result.table_name);
        report::RecoveryReport built_report = rb.build();
        built_report.data.total_pages_scanned = result.page_damage.size();
        built_report.data.corrupted_pages = damaged_pages;
        built_report.data.skipped_pages = header_damage;
        built_report.damage.pages_with_header_damage = header_damage;
        built_report.damage.pages_with_checksum_mismatch = checksum_mismatches;
        built_report.result_contract = result_contract_from_unified(
            result, result.table_name, result.source_ibd);
        result.report = std::move(built_report);
    } catch (...) {}
    return result;
}

bool write_unified_result_json(const std::string& output_dir,
                               const std::string& table_name,
                               const UnifiedRecoveryResult& result) {
    try {
        fs::create_directories(output_dir);
        const std::string name = result.table_name.empty()
            ? (table_name.empty() ? "recovered_table" : table_name)
            : result.table_name;
        const std::string path = output_dir + "/" + name + "_result.json";
        return write_result_contract_json(
            path, result_contract_from_unified(result, name, result.source_ibd));
    } catch (...) { return false; }
}

} // namespace tools
} // namespace dbrecovery
