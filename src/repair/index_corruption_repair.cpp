#include "dbrecovery/repair/index_corruption_repair.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <queue>

namespace dbrecovery {
namespace repair {

// ==================== IndexCorruptionRepairer ====================

IndexCorruptionRepairer::IndexCorruptionRepairer()
    : page_size_(16384) {
}

std::vector<IndexCorruption> IndexCorruptionRepairer::analyze(const uint8_t* file_data, size_t file_size) {
    std::vector<IndexCorruption> corruptions;

    if (!file_data || file_size < page_size_ * 3) {
        return corruptions;
    }

    size_t page_count = file_size / page_size_;

    // 1. 收集所有页信息
    auto all_pages = get_all_page_info(file_data, file_size);

    // 2. 检测索引ID不一致
    std::map<uint32_t, std::vector<uint32_t>> pages_by_index_id;
    for (const auto& [page_no, info] : all_pages) {
        if (info.is_valid && info.index_id != 0 && info.index_id != 0xFFFFFFFF) {
            pages_by_index_id[info.index_id].push_back(page_no);
        }
    }

    // 找出主索引ID（页数最多的）
    uint32_t main_index_id = 0;
    size_t max_pages = 0;
    for (const auto& [index_id, pages] : pages_by_index_id) {
        if (pages.size() > max_pages) {
            max_pages = pages.size();
            main_index_id = index_id;
        }
    }

    // 检测索引ID不匹配的页
    for (const auto& [page_no, info] : all_pages) {
        if (info.is_valid && info.index_id != 0 && info.index_id != 0xFFFFFFFF) {
            if (info.index_id != main_index_id) {
                IndexCorruption c;
                c.type = IndexCorruptionType::INDEX_ID_MISMATCH;
                c.page_no = page_no;
                c.related_page_no = main_index_id;
                std::ostringstream oss;
                oss << "Page " << page_no << " has index_id " << info.index_id
                    << ", expected " << main_index_id;
                c.description = oss.str();
                c.confidence = 0.85f;
                c.repairable = true;
                corruptions.push_back(c);
            }
        }
    }

    // 3. 检测根页
    auto root_opt = detect_root_from_inode(file_data, file_size);
    if (!root_opt) {
        root_opt = infer_root_page(file_data, file_size);
    }

    if (!root_opt) {
        IndexCorruption c;
        c.type = IndexCorruptionType::ROOT_PAGE_NOT_FOUND;
        c.page_no = 0;
        c.description = "Cannot detect root page from INODE or infer from structure";
        c.confidence = 0.90f;
        c.repairable = true;
        corruptions.push_back(c);
    }

    // 4. 检测层级问题
    if (root_opt) {
        uint32_t root_page = *root_opt;

        // 使用 BFS 遍历 B+Tree
        std::set<uint32_t> visited;
        std::queue<std::pair<uint32_t, uint8_t>> to_visit;  // (page_no, expected_level)
        to_visit.push({root_page, 255});  // 255 表示未知层级

        while (!to_visit.empty()) {
            auto [current_page, expected_level] = to_visit.front();
            to_visit.pop();

            if (visited.find(current_page) != visited.end()) {
                continue;  // 已访问
            }
            visited.insert(current_page);

            auto info_opt = get_page_info(file_data, file_size, current_page);
            if (!info_opt || !info_opt->is_valid) {
                IndexCorruption c;
                c.type = IndexCorruptionType::NODE_POINTER_INVALID;
                c.page_no = current_page;
                c.description = "Node points to invalid page";
                c.confidence = 0.90f;
                c.repairable = false;
                corruptions.push_back(c);
                continue;
            }

            const auto& info = *info_opt;

            // 检查层级
            if (expected_level != 255 && info.level != expected_level) {
                IndexCorruption c;
                c.type = IndexCorruptionType::LEVEL_MISMATCH;
                c.page_no = current_page;
                std::ostringstream oss;
                oss << "Page " << current_page << " has level " << (int)info.level
                    << ", expected " << (int)expected_level;
                c.description = oss.str();
                c.confidence = 0.85f;
                c.repairable = true;
                corruptions.push_back(c);
            }

            // 检查非叶节点没有子节点
            if (!info.is_leaf && info.child_pages.empty()) {
                IndexCorruption c;
                c.type = IndexCorruptionType::NON_LEAF_NO_CHILDREN;
                c.page_no = current_page;
                c.description = "Non-leaf page has no child pointers";
                c.confidence = 0.90f;
                c.repairable = false;
                corruptions.push_back(c);
            }

            // 检查叶节点有子节点
            if (info.is_leaf && !info.child_pages.empty()) {
                IndexCorruption c;
                c.type = IndexCorruptionType::LEAF_HAS_CHILDREN;
                c.page_no = current_page;
                c.description = "Leaf page has child pointers";
                c.confidence = 0.90f;
                c.repairable = true;
                corruptions.push_back(c);
            }

            // 添加子节点到队列
            if (!info.is_leaf) {
                for (uint32_t child : info.child_pages) {
                    if (child < page_count && child != 0xFFFFFFFF) {
                        to_visit.push({child, static_cast<uint8_t>(info.level - 1)});
                    }
                }
            }
        }
    }

    // 5. 检测孤儿页
    auto orphans = detect_orphan_pages(file_data, file_size);
    for (uint32_t orphan : orphans) {
        IndexCorruption c;
        c.type = IndexCorruptionType::PAGE_CHAIN_ORPHAN;
        c.page_no = orphan;
        c.description = "Page is not reachable from root";
        c.confidence = 0.80f;
        c.repairable = false;
        corruptions.push_back(c);
    }

    // 6. 检测页链循环
    auto loops = detect_page_chain_loops(file_data, file_size);
    for (const auto& loop : loops) {
        IndexCorruption c;
        c.type = IndexCorruptionType::PAGE_CHAIN_LOOP;
        c.page_no = loop.front();
        std::ostringstream oss;
        oss << "Circular reference detected: ";
        for (size_t i = 0; i < loop.size() && i < 5; ++i) {
            oss << loop[i] << " -> ";
        }
        if (loop.size() > 5) oss << "...";
        oss << loop.front();
        c.description = oss.str();
        c.confidence = 0.95f;
        c.repairable = true;
        corruptions.push_back(c);
    }

    return corruptions;
}

size_t IndexCorruptionRepairer::repair(uint8_t* file_data, size_t file_size,
                                        const std::vector<IndexCorruption>& corruptions) {
    size_t repaired = 0;

    for (const auto& c : corruptions) {
        if (!c.repairable) {
            continue;
        }

        bool success = false;

        switch (c.type) {
            case IndexCorruptionType::INDEX_ID_MISMATCH:
                success = repair_index_id_mismatch(file_data, c.page_no, c.related_page_no);
                break;

            case IndexCorruptionType::LEVEL_MISMATCH:
                // 需要额外的逻辑确定正确的层级
                break;

            case IndexCorruptionType::PAGE_CHAIN_LOOP:
                success = rebuild_page_chain(file_data, file_size);
                break;

            default:
                break;
        }

        if (success) {
            repaired++;
            repaired_count_++;
        }
    }

    return repaired;
}

std::optional<uint32_t> IndexCorruptionRepairer::detect_root_from_inode(const uint8_t* file_data, size_t file_size) {
    InodePageRepairer inode_repairer;
    inode_repairer.set_page_size(page_size_);

    if (file_size < page_size_ * 3) {
        return std::nullopt;
    }

    const uint8_t* inode_page = file_data + 2 * page_size_;
    return inode_repairer.extract_root_page(inode_page, page_size_);
}

std::optional<uint32_t> IndexCorruptionRepairer::infer_root_page(const uint8_t* file_data, size_t file_size) {
    if (!file_data || file_size < page_size_) {
        return std::nullopt;
    }

    // 收集所有页的层级信息
    auto all_pages = get_all_page_info(file_data, file_size);

    // 找出层级最大的页（应该是根页）
    uint32_t max_level_page = 0;
    uint8_t max_level = 0;

    for (const auto& [page_no, info] : all_pages) {
        if (info.is_valid && !info.is_leaf && info.level > max_level) {
            max_level = info.level;
            max_level_page = page_no;
        }
    }

    if (max_level > 0) {
        return max_level_page;
    }

    // 如果没有非叶节点，返回第一个有效的 INDEX 页
    for (const auto& [page_no, info] : all_pages) {
        if (info.is_valid) {
            return page_no;
        }
    }

    return std::nullopt;
}

bool IndexCorruptionRepairer::repair_node_pointer(uint8_t* file_data, uint32_t page_no, uint32_t expected_child) {
    if (!file_data || page_no == 0xFFFFFFFF) {
        return false;
    }

    uint8_t* page = file_data + page_no * page_size_;

    // 非叶节点的子节点指针存储在记录中
    // 这需要解析记录格式，暂时简化处理
    last_error_ = "Node pointer repair not fully implemented";
    return false;
}

bool IndexCorruptionRepairer::repair_level_mismatch(uint8_t* file_data, uint32_t page_no, uint8_t correct_level) {
    if (!file_data) {
        return false;
    }

    uint8_t* page = file_data + page_no * page_size_;

    // level 存储在 PAGE_HEADER 的 offset 38 + 4 = 42 (BTR_N_LEVELS_ABS)
    dbrecovery::innodb::write_be<uint16_t>(page, 42, correct_level);

    return true;
}

bool IndexCorruptionRepairer::repair_index_id_mismatch(uint8_t* file_data, uint32_t page_no, uint32_t correct_index_id) {
    if (!file_data) {
        return false;
    }

    uint8_t* page = file_data + page_no * page_size_;

    // index_id 存储在 offset 38 + 6 = 44 (BTR_IND_PAGE_OFFSET_ABS)
    dbrecovery::innodb::write_be<uint32_t>(page, 44, correct_index_id);

    return true;
}

bool IndexCorruptionRepairer::rebuild_page_chain(uint8_t* file_data, size_t file_size) {
    if (!file_data || file_size < page_size_) {
        return false;
    }

    auto all_pages = get_all_page_info(file_data, file_size);

    // 按层级分组
    std::map<uint8_t, std::vector<uint32_t>> pages_by_level;
    for (const auto& [page_no, info] : all_pages) {
        if (info.is_valid) {
            pages_by_level[info.level].push_back(page_no);
        }
    }

    // 对每个层级的页按某种顺序排序并重建链
    for (auto& [level, pages] : pages_by_level) {
        std::sort(pages.begin(), pages.end());

        for (size_t i = 0; i < pages.size(); ++i) {
            uint8_t* page = file_data + pages[i] * page_size_;

            // 设置 prev_page
            if (i == 0) {
                dbrecovery::innodb::write_be<uint32_t>(page, 8, 0xFFFFFFFF);
            } else {
                dbrecovery::innodb::write_be<uint32_t>(page, 8, pages[i - 1]);
            }

            // 设置 next_page
            if (i == pages.size() - 1) {
                dbrecovery::innodb::write_be<uint32_t>(page, 12, 0xFFFFFFFF);
            } else {
                dbrecovery::innodb::write_be<uint32_t>(page, 12, pages[i + 1]);
            }
        }
    }

    return true;
}

std::vector<uint32_t> IndexCorruptionRepairer::detect_orphan_pages(const uint8_t* file_data, size_t file_size) {
    std::vector<uint32_t> orphans;

    if (!file_data || file_size < page_size_) {
        return orphans;
    }

    size_t page_count = file_size / page_size_;
    auto all_pages = get_all_page_info(file_data, file_size);

    // 找到根页
    auto root_opt = detect_root_from_inode(file_data, file_size);
    if (!root_opt) {
        root_opt = infer_root_page(file_data, file_size);
    }

    if (!root_opt) {
        return orphans;
    }

    // BFS 遍历找到所有可达页
    std::set<uint32_t> reachable;
    std::queue<uint32_t> to_visit;
    to_visit.push(*root_opt);

    while (!to_visit.empty()) {
        uint32_t current = to_visit.front();
        to_visit.pop();

        if (reachable.find(current) != reachable.end()) {
            continue;
        }
        reachable.insert(current);

        auto info_opt = get_page_info(file_data, file_size, current);
        if (!info_opt || !info_opt->is_valid) {
            continue;
        }

        // 添加子节点
        for (uint32_t child : info_opt->child_pages) {
            if (child < page_count && child != 0xFFFFFFFF) {
                to_visit.push(child);
            }
        }

        // 添加链中的邻居
        if (info_opt->prev_page < page_count && info_opt->prev_page != 0xFFFFFFFF) {
            to_visit.push(info_opt->prev_page);
        }
        if (info_opt->next_page < page_count && info_opt->next_page != 0xFFFFFFFF) {
            to_visit.push(info_opt->next_page);
        }
    }

    // 找出不可达的页
    for (const auto& [page_no, info] : all_pages) {
        if (info.is_valid && reachable.find(page_no) == reachable.end()) {
            orphans.push_back(page_no);
        }
    }

    return orphans;
}

std::vector<std::vector<uint32_t>> IndexCorruptionRepairer::detect_page_chain_loops(const uint8_t* file_data, size_t file_size) {
    std::vector<std::vector<uint32_t>> loops;

    if (!file_data || file_size < page_size_) {
        return loops;
    }

    auto all_pages = get_all_page_info(file_data, file_size);

    // 使用 Floyd's cycle detection 检测每个链
    for (const auto& [start_page, info] : all_pages) {
        if (!info.is_valid || info.next_page == 0xFFFFFFFF) {
            continue;
        }

        std::vector<uint32_t> path;
        std::set<uint32_t> visited;
        uint32_t current = start_page;

        while (current != 0xFFFFFFFF && visited.find(current) == visited.end()) {
            visited.insert(current);
            path.push_back(current);

            auto current_info_opt = get_page_info(file_data, file_size, current);
            if (!current_info_opt || !current_info_opt->is_valid) {
                break;
            }
            current = current_info_opt->next_page;
        }

        if (current != 0xFFFFFFFF && visited.find(current) != visited.end()) {
            // 找到循环
            auto it = std::find(path.begin(), path.end(), current);
            if (it != path.end()) {
                std::vector<uint32_t> loop(it, path.end());
                loops.push_back(loop);
            }
        }
    }

    return loops;
}

std::optional<BTreePageInfo> IndexCorruptionRepairer::get_page_info(const uint8_t* file_data, size_t file_size, uint32_t page_no) const {
    if (!file_data || file_size < (page_no + 1) * page_size_) {
        return std::nullopt;
    }

    const uint8_t* page = file_data + page_no * page_size_;
    return parse_page_header(page);
}

std::map<uint32_t, BTreePageInfo> IndexCorruptionRepairer::get_all_page_info(const uint8_t* file_data, size_t file_size) const {
    std::map<uint32_t, BTreePageInfo> result;

    if (!file_data || file_size < page_size_) {
        return result;
    }

    size_t page_count = file_size / page_size_;

    for (size_t i = 0; i < page_count; ++i) {
        auto info_opt = get_page_info(file_data, file_size, static_cast<uint32_t>(i));
        if (info_opt) {
            result[static_cast<uint32_t>(i)] = *info_opt;
        }
    }

    return result;
}

bool IndexCorruptionRepairer::validate_btree_structure(const uint8_t* file_data, size_t file_size) {
    auto corruptions = analyze(file_data, file_size);
    return corruptions.empty();
}

BTreePageInfo IndexCorruptionRepairer::parse_page_header(const uint8_t* page_data) const {
    BTreePageInfo info = {};

    if (!page_data) {
        info.is_valid = false;
        return info;
    }

    // 读取页类型
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX) {
        info.is_valid = false;
        return info;
    }

    info.page_type = page_type;
    info.is_valid = true;

    // 读取页号
    info.page_no = dbrecovery::innodb::read_be<uint32_t>(page_data, 4);

    // 读取 prev/next
    info.prev_page = dbrecovery::innodb::read_be<uint32_t>(page_data, 8);
    info.next_page = dbrecovery::innodb::read_be<uint32_t>(page_data, 12);

    // 读取 BTR header
    // level 在 offset 42 (FIL_HEADER_SIZE + 4)
    info.level = dbrecovery::innodb::read_be<uint16_t>(page_data, 42);
    info.is_leaf = (info.level == 0);

    // index_id 在 offset 44
    info.index_id = dbrecovery::innodb::read_be<uint32_t>(page_data, 44);

    // 读取 n_dir_slots 和 n_recs
    info.n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data, 38);
    info.n_records = dbrecovery::innodb::read_le<uint16_t>(page_data, 54);

    // 提取子节点指针（非叶节点）
    if (!info.is_leaf) {
        info.child_pages = extract_child_pointers(page_data, page_size_);
    }

    return info;
}

std::vector<uint32_t> IndexCorruptionRepairer::extract_child_pointers(const uint8_t* page_data, size_t page_size) const {
    std::vector<uint32_t> children;

    if (!page_data || page_size < 100) {
        return children;
    }

    // 非叶节点的记录格式：[key] [child_page_no]
    // 子页号存储在每条记录的末尾（4字节）
    // 需要遍历记录链来提取

    // 简化处理：从 INFIMUM 开始遍历
    uint16_t current = 54;
    std::set<uint16_t> visited;

    while (current != 0 && current < page_size - 10) {
        if (visited.find(current) != visited.end()) {
            break;
        }
        visited.insert(current);

        uint8_t rec_type = page_data[current] & 0x07;

        if (rec_type == dbrecovery::innodb::REC_STATUS_SUPREMUM) {
            break;
        }

        // 对于节点指针记录，最后4字节是子页号
        // 需要知道记录长度，这里简化处理
        // 实际实现需要解析记录格式

        // 读取下一条记录
        int16_t next_offset = static_cast<int16_t>(
            dbrecovery::innodb::read_le<uint16_t>(page_data, current + 1));
        if (next_offset == 0) {
            break;
        }
        current = current + next_offset;
    }

    return children;
}

// ==================== InodePageRepairer ====================

InodePageRepairer::InodePageRepairer()
    : page_size_(16384) {
}

std::vector<IndexCorruption> InodePageRepairer::detect_corruptions(const uint8_t* page_data, size_t page_size) {
    std::vector<IndexCorruption> corruptions;

    if (!page_data || page_size < 100) {
        return corruptions;
    }

    // 检查 INODE 页的页类型
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

    if (page_type != 0x45BB) {  // FIL_PAGE_INODE
        IndexCorruption c;
        c.type = IndexCorruptionType::ROOT_PAGE_NOT_FOUND;
        c.page_no = 2;
        c.description = "Page 2 is not an INODE page";
        c.confidence = 0.95f;
        c.repairable = true;
        corruptions.push_back(c);
    }

    return corruptions;
}

bool InodePageRepairer::repair(uint8_t* page_data, size_t page_size) {
    if (!page_data) {
        return false;
    }

    // 设置页类型为 INODE
    dbrecovery::innodb::write_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS, 0x45BB);

    return true;
}

std::optional<uint32_t> InodePageRepairer::extract_root_page(const uint8_t* page_data, size_t page_size) const {
    if (!page_data || page_size < 100) {
        return std::nullopt;
    }

    // INODE 结构：
    // - List node (12 bytes) for full node list
    // - List node (12 bytes) for free node list
    // - Inode entries (each 12 bytes)
    //
    // 每个 Inode entry 包含：
    // - segment_id (8 bytes)
    // - 2 list nodes (12 bytes each)
    // - fseg header (10 bytes)
    //
    // 简化处理：直接从特定偏移读取

    // 实际的根页信息存储在 FSP_HDR 页（page 0）或 INODE 页
    // 这里返回一个占位值
    return std::nullopt;
}

bool InodePageRepairer::write_root_page(uint8_t* page_data, size_t page_size, uint32_t root_page_no) {
    if (!page_data) {
        return false;
    }

    // 写入根页信息到 INODE
    // 实际实现需要更复杂的 INODE 结构处理
    return false;
}

bool InodePageRepairer::rebuild_inode(uint8_t* page_data, size_t page_size,
                                       const std::vector<uint32_t>& root_pages) {
    if (!page_data || root_pages.empty()) {
        return false;
    }

    // 重建 INODE 页
    // 设置页类型
    dbrecovery::innodb::write_be<uint16_t>(page_data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS, 0x45BB);

    // 清零其他部分
    std::memset(page_data + 38, 0, page_size - 38 - 8);

    // 写入根页信息
    // 实际实现需要完整重建 INODE 结构

    return true;
}

// ==================== IndexStructureHandler ====================

size_t IndexStructureHandler::analyze_and_repair(uint8_t* file_data, size_t file_size) {
    last_corruptions_ = repairer_.analyze(file_data, file_size);
    if (last_corruptions_.empty()) {
        return 0;
    }
    return repairer_.repair(file_data, file_size, last_corruptions_);
}

std::vector<IndexCorruption> IndexStructureHandler::analyze(const uint8_t* file_data, size_t file_size) {
    last_corruptions_ = repairer_.analyze(file_data, file_size);
    return last_corruptions_;
}

std::string IndexStructureHandler::get_report() const {
    std::ostringstream oss;
    oss << "Index Structure Corruption Report\n";
    oss << "==================================\n\n";

    if (last_corruptions_.empty()) {
        oss << "No index structure corruption detected.\n";
        return oss.str();
    }

    oss << "Detected " << last_corruptions_.size() << " corruption(s):\n\n";

    for (size_t i = 0; i < last_corruptions_.size(); ++i) {
        const auto& c = last_corruptions_[i];
        oss << (i + 1) << ". ";

        switch (c.type) {
            case IndexCorruptionType::NODE_POINTER_INVALID:
                oss << "Node Pointer Invalid";
                break;
            case IndexCorruptionType::ROOT_PAGE_NOT_FOUND:
                oss << "Root Page Not Found";
                break;
            case IndexCorruptionType::LEVEL_MISMATCH:
                oss << "Level Mismatch";
                break;
            case IndexCorruptionType::INDEX_ID_MISMATCH:
                oss << "Index ID Mismatch";
                break;
            case IndexCorruptionType::PAGE_CHAIN_ORPHAN:
                oss << "Orphan Page";
                break;
            case IndexCorruptionType::PAGE_CHAIN_LOOP:
                oss << "Page Chain Loop";
                break;
            case IndexCorruptionType::NON_LEAF_NO_CHILDREN:
                oss << "Non-leaf Has No Children";
                break;
            case IndexCorruptionType::LEAF_HAS_CHILDREN:
                oss << "Leaf Has Children";
                break;
            default:
                oss << "Unknown";
                break;
        }

        oss << "\n   Page: " << c.page_no;
        if (c.related_page_no != 0) {
            oss << " (related: " << c.related_page_no << ")";
        }
        oss << "\n";
        oss << "   Confidence: " << std::fixed << std::setprecision(1) << (c.confidence * 100) << "%\n";
        oss << "   Repairable: " << (c.repairable ? "Yes" : "No") << "\n";
        oss << "   Description: " << c.description << "\n\n";
    }

    oss << "Repair Statistics:\n";
    oss << "  Repaired: " << repairer_.get_repaired_count() << "\n";

    return oss.str();
}

} // namespace repair
} // namespace dbrecovery
