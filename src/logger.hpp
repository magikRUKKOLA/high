#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <atomic>
#include <string>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <mutex>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <sys/time.h>

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    static void init(LogLevel level = LogLevel::INFO);
    static void init(const std::string& log_file_path, LogLevel level = LogLevel::INFO);
    static void shutdown();
    
    static std::string get_timestamp() {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        // FIX: Use thread-safe localtime_r
        struct tm tm_info;
        localtime_r(&tv.tv_sec, &tm_info);
        char buf[32];
        snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%06ld]", 
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, tv.tv_usec);
        return std::string(buf);
    }

    template<typename... Args>
    static void log(LogLevel level, const char* fmt, Args&&... args) {
        if (level < g_log_level.load()) return;
        
        std::lock_guard<std::mutex> lock(log_mutex);
        
        char buffer[4096];
        int n = snprintf(buffer, sizeof(buffer), fmt, std::forward<Args>(args)...);
        if (n < 0 || n >= static_cast<int>(sizeof(buffer))) {
            // Truncation
        }
        
        std::string log_line = buffer;
        if (log_line.empty()) return;
        
        if (log_line.back() != '\n') {
            log_line += '\n';
        }
        
        std::string ts = get_timestamp();
        log_line = ts + " " + log_line;
        
        // If log file is open, write only to file (no stderr)
        if (log_file_stream.is_open()) {
            log_file_stream << log_line;
            log_file_stream.flush();
        } else if (use_stderr_) {
            std::cerr << log_line;
        }
    }
    
    template<typename... Args>
    static void debug(const char* fmt, Args&&... args) {
        log(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void info(const char* fmt, Args&&... args) {
        log(LogLevel::INFO, fmt, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void warn(const char* fmt, Args&&... args) {
        log(LogLevel::WARN, fmt, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    static void error(const char* fmt, Args&&... args) {
        log(LogLevel::ERROR, fmt, std::forward<Args>(args)...);
    }

private:
    static std::atomic<LogLevel> g_log_level;
    static std::ofstream log_file_stream;
    static std::mutex log_mutex;
    static bool use_stderr_;
};

#endif
