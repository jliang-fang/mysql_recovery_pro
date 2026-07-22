#ifndef DBRECOVERY_TOOLS_INTERACTIVE_SCHEMA_BUILDER_HPP
#define DBRECOVERY_TOOLS_INTERACTIVE_SCHEMA_BUILDER_HPP

#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

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
 * Commands:
 *   [Enter]  — accept column as-is
 *   e        — edit column name/type
 *   r        — reject column (exclude from schema)
 *   q        — quit interactive mode
 */
class InteractiveSchemaBuilder {
public:
    struct UserAction {
        enum Type { ACCEPT, EDIT, REJECT, QUIT };
        Type type = ACCEPT;
        std::string edit_value;
    };

    /**
     * @brief Set the v2 schema inference result to work with
     */
    void set_schema_result(const inference::SchemaInferenceResult& result) {
        original_result_ = result;
        accepted_result_ = result;
        accepted_.assign(result.columns.size(), true);
        interactive_mode_ = false;
        rejected_columns_.clear();
    }

    /**
     * @brief Run the interactive schema builder loop
     * @return true if user accepted (not quit), false if quit
     */
    bool run_interactive() {
        if (original_result_.columns.empty()) {
            std::cout << "[INFO] No columns to display (empty schema).\n";
            return false;
        }

        interactive_mode_ = true;
        std::cout << "\n=== Interactive Schema Builder ===\n";
        std::cout << "Table: " << original_result_.proposed_table_name << "\n";
        std::cout << "Columns: " << original_result_.detected_column_count << "\n";
        std::cout << "Confidence: " << std::fixed << std::setprecision(1)
                  << original_result_.overall_confidence * 100 << "%\n\n";

        for (size_t i = 0; i < original_result_.columns.size(); i++) {
            if (!interactive_mode_) break;

            const auto& col = original_result_.columns[i];
            display_column_candidates(col);

            UserAction action = prompt_for_column(col, static_cast<int>(i) + 1,
                                                  static_cast<int>(original_result_.columns.size()));

            switch (action.type) {
                case UserAction::ACCEPT:
                    accepted_[i] = true;
                    std::cout << "  [Accepted]\n\n";
                    break;
                case UserAction::EDIT:
                    accepted_[i] = true;
                    if (!action.edit_value.empty()) {
                        handle_edit_column(static_cast<int>(i), action.edit_value, col.best_guess.innodb_type_id);
                    }
                    std::cout << "  [Edited]\n\n";
                    break;
                case UserAction::REJECT:
                    accepted_[i] = false;
                    rejected_columns_.insert(static_cast<int>(i));
                    std::cout << "  [Rejected]\n\n";
                    break;
                case UserAction::QUIT:
                    interactive_mode_ = false;
                    std::cout << "  [Quit]\n\n";
                    break;
            }
        }

        // Rebuild accepted result
        rebuild_accepted_result();
        return interactive_mode_;  // true if we didn't quit at the very start
    }

    /**
     * @brief Display a summary of the final accepted schema
     */
    void display_summary() {
        std::cout << "\n=== Accepted Schema Summary ===\n";
        std::cout << "Table: " << accepted_result_.proposed_table_name << "\n";
        size_t accepted_count = 0;
        for (bool a : accepted_) {
            if (a) accepted_count++;
        }
        std::cout << "Accepted: " << accepted_count << "/" << original_result_.columns.size() << " columns\n";

        if (!rejected_columns_.empty()) {
            std::cout << "Rejected: ";
            bool first = true;
            for (int idx : rejected_columns_) {
                if (!first) std::cout << ", ";
                std::cout << original_result_.columns[static_cast<size_t>(idx)].suggested_name;
                first = false;
            }
            std::cout << "\n";
        }

        std::cout << "Confidence: " << std::fixed << std::setprecision(1)
                  << accepted_result_.overall_confidence * 100 << "%\n\n";

        if (!accepted_result_.columns.empty()) {
            std::cout << "Columns:\n";
            for (const auto& col : accepted_result_.columns) {
                std::cout << "  " << col.ordinal_position << ". " << col.suggested_name
                          << " (" << col.best_guess.type_name << ")"
                          << (col.is_nullable ? " NULL" : " NOT NULL")
                          << (col.is_auto_increment_candidate ? " AUTO_INCREMENT" : "")
                          << "\n";
            }
        }
    }

    /**
     * @brief Generate DDL for the accepted schema
     */
    std::string generate_ddl() {
        std::ostringstream oss;
        oss << "CREATE TABLE `" << accepted_result_.proposed_table_name << "` (\n";

        for (size_t i = 0; i < accepted_result_.columns.size(); i++) {
            const auto& col = accepted_result_.columns[i];
            oss << "  `" << col.suggested_name << "` " << col.best_guess.type_name;

            if (col.is_nullable) {
                oss << " NULL";
            } else {
                oss << " NOT NULL";
            }

            if (col.is_auto_increment_candidate) {
                oss << " AUTO_INCREMENT";
            }

            if (i < accepted_result_.columns.size() - 1) {
                oss << ",";
            }
            oss << "\n";
        }

        oss << ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n";
        return oss.str();
    }

    /**
     * @brief Get the accepted schema result
     */
    const inference::SchemaInferenceResult& get_accepted_schema() const {
        return accepted_result_;
    }

    /**
     * @brief Display type candidates for a column
     */
    void display_column_candidates(const inference::InferredColumnV2& col) {
        std::cout << "  Column " << col.ordinal_position << ": "
                  << col.suggested_name << "\n";
        std::cout << "    Best: " << col.best_guess.to_string() << "\n";
        for (size_t i = 1; i < col.candidates.size(); i++) {
            std::cout << "    Alt:  " << col.candidates[i].to_string() << "\n";
        }
        if (col.is_primary_key_candidate) std::cout << "    [PK candidate]\n";
        if (col.is_auto_increment_candidate) std::cout << "    [AUTO_INCREMENT candidate]\n";
    }

    /**
     * @brief Prompt user for action on a column
     */
    UserAction prompt_for_column(const inference::InferredColumnV2& col, int index, int total) {
        UserAction action;
        std::string input;

        std::cout << "  [" << index << "/" << total << "] "
                  << col.suggested_name
                  << " [Enter=accept, e=edit, r=reject, q=quit]: ";
        std::flush(std::cout);
        std::getline(std::cin, input);

        // Trim whitespace
        input.erase(0, input.find_first_not_of(" \t\n\r"));
        input.erase(input.find_last_not_of(" \t\n\r") + 1);

        if (input.empty() || input == "a") {
            action.type = UserAction::ACCEPT;
        } else if (input == "e") {
            action.type = UserAction::EDIT;
            std::cout << "    Enter new name (or press Enter to keep '" << col.suggested_name << "'): ";
            std::flush(std::cout);
            std::getline(std::cin, action.edit_value);
            // Trim
            action.edit_value.erase(0, action.edit_value.find_first_not_of(" \t\n\r"));
            action.edit_value.erase(action.edit_value.find_last_not_of(" \t\n\r") + 1);
        } else if (input == "r") {
            action.type = UserAction::REJECT;
        } else if (input == "q") {
            action.type = UserAction::QUIT;
        } else {
            // Default to accept for unrecognized input
            action.type = UserAction::ACCEPT;
        }

        return action;
    }

    /**
     * @brief Handle edit of a column's name (type_id preserved from best_guess)
     */
    bool handle_edit_column(int col_index, const std::string& new_name, int new_type_id) {
        size_t idx = static_cast<size_t>(col_index);
        if (idx >= original_result_.columns.size()) return false;

        if (!new_name.empty()) {
            original_result_.columns[idx].suggested_name = new_name;
            accepted_result_.columns[idx].suggested_name = new_name;
        }

        // Update type_id if different from best_guess
        if (new_type_id != original_result_.columns[idx].best_guess.innodb_type_id) {
            original_result_.columns[idx].best_guess.innodb_type_id = new_type_id;
            accepted_result_.columns[idx].best_guess.innodb_type_id = new_type_id;
        }

        return true;
    }

    /**
     * @brief Handle rejection of a column
     */
    bool handle_reject_column(int col_index) {
        size_t idx = static_cast<size_t>(col_index);
        if (idx >= original_result_.columns.size()) return false;

        accepted_[idx] = false;
        rejected_columns_.insert(col_index);
        rebuild_accepted_result();
        return true;
    }

private:
    inference::SchemaInferenceResult original_result_;
    inference::SchemaInferenceResult accepted_result_;
    std::vector<bool> accepted_;
    std::set<int> rejected_columns_;
    bool interactive_mode_ = false;

    /**
     * @brief Rebuild accepted_result_ from accepted_ flags
     */
    void rebuild_accepted_result() {
        inference::SchemaInferenceResult rebuilt;
        rebuilt.proposed_table_name = original_result_.proposed_table_name;
        rebuilt.overall_confidence = original_result_.overall_confidence;
        rebuilt.inference_method = original_result_.inference_method;
        rebuilt.warnings = original_result_.warnings;

        for (size_t i = 0; i < original_result_.columns.size(); i++) {
            if (accepted_[i]) {
                rebuilt.columns.push_back(original_result_.columns[i]);
            }
        }

        rebuilt.detected_column_count = static_cast<uint32_t>(rebuilt.columns.size());
        rebuilt.tentative_ddl = [&]() {
            std::ostringstream oss;
            oss << "CREATE TABLE `" << rebuilt.proposed_table_name << "` (\n";
            for (size_t i = 0; i < rebuilt.columns.size(); i++) {
                const auto& col = rebuilt.columns[i];
                oss << "  `" << col.suggested_name << "` " << col.best_guess.type_name;
                if (i < rebuilt.columns.size() - 1) oss << ",";
                oss << "\n";
            }
            oss << ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n";
            return oss.str();
        }();

        accepted_result_ = rebuilt;
    }
};

} // namespace tools
} // namespace dbrecovery

#endif // DBRECOVERY_TOOLS_INTERACTIVE_SCHEMA_BUILDER_HPP