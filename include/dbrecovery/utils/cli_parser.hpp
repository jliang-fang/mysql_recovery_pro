#ifndef DBRECOVERY_UTILS_CLI_PARSER_HPP
#define DBRECOVERY_UTILS_CLI_PARSER_HPP

#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <optional>

namespace dbrecovery {
namespace utils {

/**
 * @brief CliParser: 命令行参数解析器
 *
 * 支持的参数格式：
 * - --key=value
 * - --key value
 * - -k value
 * - --flag (布尔标志)
 */
class CliParser {
public:
    explicit CliParser(int argc, char** argv);
    ~CliParser() = default;

    // 禁止拷贝
    CliParser(const CliParser&) = delete;
    CliParser& operator=(const CliParser&) = delete;

    /**
     * @brief 检查选项是否存在
     *
     * @param name 选项名
     * @return true 存在
     * @return false 不存在
     */
    bool has_option(const std::string& name) const;

    /**
     * @brief 获取选项值
     *
     * @param name 选项名
     * @param default_val 默认值
     * @return std::string 选项值
     */
    std::string get_option(const std::string& name, const std::string& default_val = "") const;

    /**
     * @brief 获取整数选项值
     *
     * @param name 选项名
     * @param default_val 默认值
     * @return std::optional<int> 选项值
     */
    std::optional<int> get_int_option(const std::string& name, int default_val = 0) const;

    /**
     * @brief 获取布尔选项值
     *
     * @param name 选项名
     * @param default_val 默认值
     * @return bool 选项值
     */
    bool get_bool_option(const std::string& name, bool default_val = false) const;

    /**
     * @brief 获取双精度浮点选项值
     *
     * @param name 选项名
     * @param default_val 默认值
     * @return std::optional<double> 选项值
     */
    std::optional<double> get_double_option(const std::string& name, double default_val = 0.0) const;

    /**
     * @brief 获取所有选项
     *
     * @return std::unordered_map<std::string, std::string> 所有选项
     */
    const std::unordered_map<std::string, std::string>& get_all_options() const;

    /**
     * @brief 要求必须存在的选项
     *
     * @param required 必须存在的选项列表
     * @throws std::runtime_error 如果缺少必选选项
     */
    void require_options(const std::vector<std::string>& required) const;

    /**
     * @brief 验证参数有效性
     *
     * @return true 参数有效
     * @return false 参数无效
     */
    bool validate() const;

    /**
     * @brief 获取帮助信息
     *
     * @param program_name 程序名
     * @param usage 使用说明
     * @return std::string 帮助信息
     */
    static std::string get_help_message(const std::string& program_name,
                                        const std::vector<std::string>& usage);

    /**
     * @brief 打印帮助信息
     *
     * @param program_name 程序名
     * @param usage 使用说明
     */
    static void print_help(const std::string& program_name,
                          const std::vector<std::string>& usage);

    /**
     * @brief 获取完整的帮助信息（包含所有选项和详细示例）
     *
     * @param program_name 程序名
     * @return std::string 完整的帮助信息
     */
    static std::string get_full_help(const std::string& program_name);

private:
    std::unordered_map<std::string, std::string> options_;
};

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_UTILS_CLI_PARSER_HPP
