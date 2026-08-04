#ifndef DBRECOVERY_TOOLS_INTERACTIVE_SCHEMA_BUILDER_HPP
#define DBRECOVERY_TOOLS_INTERACTIVE_SCHEMA_BUILDER_HPP

#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include "dbrecovery/inference/statistical_classifier.hpp"

namespace dbrecovery {
namespace tools {

/**
 * @brief InteractiveSchemaBuilder — terminal UI for schema confirmation
 *
 * When --infer-schema mode detects a schema, this class provides a terminal
 * UI that lets users confirm, edit, or reject inferred column types before
 * recovery proceeds.
 *
 * Note: Implementations moved to src/tools/interactive_schema_builder.cpp
 */
class InteractiveSchemaBuilder {
public:
    struct UserAction {
        enum Type { ACCEPT, EDIT, REJECT, QUIT };
        Type type = ACCEPT;
        std::string edit_value;
    };

    void set_schema_result(const inference::SchemaInferenceResult& result);
    bool run_interactive();
    void display_summary();
    std::string generate_ddl();
    const inference::SchemaInferenceResult& get_accepted_schema() const;
    void display_column_candidates(const inference::InferredColumnV2& col);
    UserAction prompt_for_column(const inference::InferredColumnV2& col, int index, int total);
    bool handle_edit_column(int col_index, const std::string& new_name, int new_type_id);
    bool handle_reject_column(int col_index);

private:
    inference::SchemaInferenceResult original_result_;
    inference::SchemaInferenceResult accepted_result_;
    std::vector<bool> accepted_;
    std::set<int> rejected_columns_;
    bool interactive_mode_ = false;

    void rebuild_accepted_result();
};

} // namespace tools
} // namespace dbrecovery

#endif // DBRECOVERY_TOOLS_INTERACTIVE_SCHEMA_BUILDER_HPP