#ifndef NLOHMANN_JSON_HPP
#define NLOHMANN_JSON_HPP

// 临时修复：提供基本功能以支持编译，但强烈建议使用官方库替代
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <memory>

namespace nlohmann {

class json {
public:
    enum class value_t { null, boolean, number_integer, number_unsigned, number_float, string, array, object };

    json() : m_type(value_t::null), m_value_integer(0) {}
    explicit json(bool b) : m_type(value_t::boolean), m_value_boolean(b) {}
    explicit json(std::nullptr_t) : m_type(value_t::null), m_value_integer(0) {}
    explicit json(int64_t i) : m_type(value_t::number_integer), m_value_integer(i) {}
    explicit json(uint64_t u) : m_type(value_t::number_unsigned), m_value_unsigned(u) {}
    explicit json(double d) : m_type(value_t::number_float), m_value_float(d) {}
    explicit json(const std::string& s) : m_type(value_t::string), m_value_string(std::make_shared<std::string>(s)) {}
    explicit json(std::string&& s) : m_type(value_t::string), m_value_string(std::make_shared<std::string>(std::move(s))) {}

    ~json() = default;

    json(const json& other) : m_type(other.m_type) {
        copy_from(other);
    }

    json& operator=(const json& other) {
        if (this != &other) {
            m_type = other.m_type;
            copy_from(other);
        }
        return *this;
    }

    json(json&& other) noexcept : m_type(other.m_type) {
        move_from(std::move(other));
    }

    json& operator=(json&& other) noexcept {
        if (this != &other) {
            m_type = other.m_type;
            move_from(std::move(other));
        }
        return *this;
    }

    value_t type() const { return m_type; }

    // 基本访问器（简化实现）
    std::string get<std::string>() const {
        if (m_type == value_t::string) return *m_value_string;
        return "";
    }

    int64_t get<int64_t>() const {
        if (m_type == value_t::number_integer) return m_value_integer;
        if (m_type == value_t::number_unsigned) return static_cast<int64_t>(m_value_unsigned);
        return 0;
    }

    bool get<bool>() const {
        if (m_type == value_t::boolean) return m_value_boolean;
        return false;
    }

    // 模拟 operator[]（仅用于对象）
    json& operator[](const std::string& key) {
        if (!m_object) m_object = std::make_shared<std::map<std::string, json>>();
        return (*m_object)[key];
    }

    const json& operator[](const std::string& key) const {
        static json null_json;
        if (m_object && m_object->find(key) != m_object->end()) {
            return (*m_object)[key];
        }
        return null_json;
    }

    bool contains(const std::string& key) const {
        return m_object && m_object->find(key) != m_object->end();
    }

private:
    void copy_from(const json& other) {
        switch (other.m_type) {
            case value_t::string:
                if (other.m_value_string) {
                    m_value_string = std::make_shared<std::string>(*other.m_value_string);
                }
                break;
            case value_t::object:
                if (other.m_object) {
                    m_object = std::make_shared<std::map<std::string, json>>(*other.m_object);
                }
                break;
            default:
                m_value = other.m_value;
                break;
        }
    }

    void move_from(json&& other) noexcept {
        switch (other.m_type) {
            case value_t::string:
                m_value_string = std::move(other.m_value_string);
                other.m_value_string = nullptr;
                break;
            case value_t::object:
                m_object = std::move(other.m_object);
                other.m_object = nullptr;
                break;
            default:
                m_value = other.m_value;
                break;
        }
    }

    value_t m_type;
    union {
        bool m_value_boolean;
        int64_t m_value_integer;
        uint64_t m_value_unsigned;
        double m_value_float;
    } m_value;

    std::shared_ptr<std::string> m_value_string;
    std::shared_ptr<std::map<std::string, json>> m_object;
};

} // namespace nlohmann

#endif // NLOHMANN_JSON_HPP
