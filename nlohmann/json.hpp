#ifndef NLOHMANN_JSON_HPP
#define NLOHMANN_JSON_HPP

// This is a placeholder for the nlohmann/json library header.
// In practice, this file should be replaced with the actual content of json.hpp
// from https://github.com/nlohmann/json/blob/develop/single_include/nlohmann/json.hpp
// or installed via package manager (e.g., vcpkg, Conan, CMake FetchContent).

// For demonstration purposes only — do NOT use this placeholder in production.
#include <string>
#include <vector>
#include <map>
#include <iostream>

class json {
public:
    enum class value_t { null, boolean, number_integer, number_unsigned, number_float, string, array, object };

    json() : m_type(value_t::null) {}
    explicit json(bool b) : m_type(value_t::boolean), m_value_boolean(b) {}
    explicit json(std::nullptr_t) : m_type(value_t::null) {}
    explicit json(const std::string& s) : m_type(value_t::string), m_value_string(new std::string(s)) {}

    ~json() {
        if (m_type == value_t::string && m_value_string) delete m_value_string;
    }

    json(const json& other) : m_type(other.m_type) {
        switch (m_type) {
            case value_t::string: m_value_string = new std::string(*other.m_value_string); break;
            default: m_value = other.m_value; break;
        }
    }

    json& operator=(const json& other) {
        if (this != &other) {
            if (m_type == value_t::string && m_value_string) delete m_value_string;
            m_type = other.m_type;
            switch (m_type) {
                case value_t::string: m_value_string = new std::string(*other.m_value_string); break;
                default: m_value = other.m_value; break;
            }
        }
        return *this;
    }

    json(json&& other) noexcept : m_type(other.m_type), m_value(other.m_value) {
        if (m_type == value_t::string) {
            m_value_string = other.m_value_string;
            other.m_value_string = nullptr;
        }
    }

    json& operator=(json&& other) noexcept {
        if (this != &other) {
            if (m_type == value_t::string && m_value_string) delete m_value_string;
            m_type = other.m_type;
            m_value = other.m_value;
            if (m_type == value_t::string) {
                m_value_string = other.m_value_string;
                other.m_value_string = nullptr;
            }
        }
        return *this;
    }

    value_t type() const { return m_type; }

private:
    value_t m_type;
    union {
        bool m_value_boolean;
        std::int64_t m_value_integer;
        std::uint64_t m_value_unsigned;
        double m_value_float;
        std::string* m_value_string;
    } m_value;
};

#endif // NLOHMANN_JSON_HPP
