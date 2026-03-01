#include "logger.hpp"
#include <chrono>
#include <iomanip>

std::atomic<LogLevel> Logger::g_log_level(LogLevel::INFO);
std::ofstream Logger::log_file_stream;
std::mutex Logger::log_mutex;
bool Logger::use_stderr_ = true;

void Logger::init(LogLevel level) {
    g_log_level = level;
    use_stderr_ = true;
}

void Logger::init(const std::string& log_file_path, LogLevel level) {
    g_log_level = level;
    use_stderr_ = false;
    
    std::lock_guard<std::mutex> lock(log_mutex);
    
    if (log_file_stream.is_open()) {
        log_file_stream.close();
    }
    
    log_file_stream.open(log_file_path, std::ios::app);
    if (!log_file_stream.is_open()) {
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    log_file_stream << "\n=== Log started at "
                    << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
                    << " ===\n";
    log_file_stream.flush();
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(log_mutex);
    
    if (log_file_stream.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        log_file_stream << "=== Log ended at "
                        << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
                        << " ===\n\n";
        log_file_stream.close();
    }
}
