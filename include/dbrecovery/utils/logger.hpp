#pragma once

#include <string>
#include <iostream>
#include <fstream>
#include <memory>
#include <mutex>
#include <atomic>

namespace dbrecovery {
namespace utils {

enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    FATAL = 5
};

class Logger {
public:
    static Logger& instance() {
        static Logger instance;
        return instance;
    }
    
    void set_level(LogLevel level) {
        level_ = level;
    }
    
    void set_output_file(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ofs_.is_open()) {
            ofs_.close();
        }
        ofs_.open(filename, std::ios::app);
        output_to_file_ = ofs_.is_open();
    }
    
    void set_console_output(bool enabled) {
        console_output_ = enabled;
    }
    
    template<typename... Args>
    void log(LogLevel level, const std::string& format, Args&&... args) {
        if (level < level_) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string prefix = "[" + level_to_string(level) + "] ";
        std::string message = format + "\n";
        
        if (console_output_) {
            std::cout << prefix << message;
        }
        
        if (output_to_file_ && ofs_.is_open()) {
            ofs_ << prefix << message;
        }
    }
    
    template<typename... Args>
    void trace(const std::string& format, Args&&... args) {
        log(LogLevel::TRACE, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void debug(const std::string& format, Args&&... args) {
        log(LogLevel::DEBUG, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void info(const std::string& format, Args&&... args) {
        log(LogLevel::INFO, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void warn(const std::string& format, Args&&... args) {
        log(LogLevel::WARN, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void error(const std::string& format, Args&&... args) {
        log(LogLevel::ERROR, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void fatal(const std::string& format, Args&&... args) {
        log(LogLevel::FATAL, format, std::forward<Args>(args)...);
    }

private:
    Logger() : level_(LogLevel::INFO), console_output_(true), output_to_file_(false) {}
    
    ~Logger() {
        if (ofs_.is_open()) {
            ofs_.close();
        }
    }
    
    std::string level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::TRACE: return "TRACE";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default:              return "UNKNOWN";
        }
    }
    
    LogLevel level_;
    bool console_output_;
    bool output_to_file_;
    std::ofstream ofs_;
    std::mutex mutex_;
};

// 便捷宏
#define LOG_TRACE(...) dbrecovery::utils::Logger::instance().trace(__VA_ARGS__)
#define LOG_DEBUG(...) dbrecovery::utils::Logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...)  dbrecovery::utils::Logger::instance().info(__VA_ARGS__)
#define LOG_WARN(...)  dbrecovery::utils::Logger::instance().warn(__VA_ARGS__)
#define LOG_ERROR(...) dbrecovery::utils::Logger::instance().error(__VA_ARGS__)
#define LOG_FATAL(...) dbrecovery::utils::Logger::instance().fatal(__VA_ARGS__)

} // namespace utils
} // namespace dbrecovery
