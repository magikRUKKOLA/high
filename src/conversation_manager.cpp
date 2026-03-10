#include "conversation_manager.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "json_utils.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

void ConversationManager::ensure_config_dir() {
    const Config& config = Config::instance();
    std::filesystem::create_directories(config.get_config_dir() + "/conversations");
}

// NEW: Fast count - only counts files, doesn't parse JSON
size_t ConversationManager::count_conversations() {
    ensure_config_dir();
    const Config& config = Config::instance();
    std::string conv_dir = config.get_config_dir() + "/conversations";
    
    if (!std::filesystem::exists(conv_dir)) return 0;
    
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(conv_dir)) {
        if (entry.path().extension() == ".json" || 
            entry.path().extension() == ".bak") {
            count++;
        }
    }
    
    return count;
}

// NEW: Paginated listing - only loads metadata for visible items
ConversationManager::ConversationPage ConversationManager::list_conversations_page(
    size_t page, size_t page_size) {
    
    ensure_config_dir();
    const Config& config = Config::instance();
    std::string conv_dir = config.get_config_dir() + "/conversations";
    
    ConversationPage result;
    result.page = page;
    result.page_size = page_size;
    
    if (!std::filesystem::exists(conv_dir)) {
        return result;
    }
    
    // First pass: collect all file info (fast - no JSON parsing)
    struct FileInfo {
        std::string title;
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
    };
    
    std::vector<FileInfo> files;
    files.reserve(1024);  // Pre-allocate for performance
    
    for (const auto& entry : std::filesystem::directory_iterator(conv_dir)) {
        if (entry.path().extension() != ".json") continue;
        
        try {
            FileInfo info;
            info.title = entry.path().stem().string();
            info.path = entry.path();
            info.mtime = std::filesystem::last_write_time(entry.path());
            files.push_back(info);
        } catch (...) {
            Logger::warn("Skipping invalid conversation file: %s", 
                        entry.path().filename().c_str());
        }
    }
    
    result.total_count = files.size();
    
    // Sort by modification time (newest first)
    std::sort(files.begin(), files.end(),
              [](const FileInfo& a, const FileInfo& b) {
                  return a.mtime > b.mtime;
              });
    
    // Calculate page boundaries
    size_t start_idx = page * page_size;
    size_t end_idx = std::min(start_idx + page_size, files.size());
    
    if (start_idx >= files.size()) {
        return result;  // Empty page
    }
    
    // Second pass: load metadata ONLY for visible items
    result.conversations.reserve(end_idx - start_idx);
    
    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& file = files[i];
        
        ConversationInfo info;
        info.title = file.title;
        
        // Convert file_time to system_clock
        try {
            info.timestamp = std::chrono::clock_cast<std::chrono::system_clock>(file.mtime);
        } catch (...) {
            info.timestamp = std::chrono::system_clock::now();
        }
        
        // Load JSON metadata (model, interrupted flag)
        try {
            std::ifstream ifs(file.path);
            if (ifs.is_open()) {
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>());
                
                json_error_t error;
                JsonPtr root(json_loads(content.c_str(), 0, &error));
                
                if (root) {
                    json_t* interrupted_obj = json_object_get(root.get(), "interrupted");
                    info.interrupted = json_is_true(interrupted_obj);
                    
                    json_t* model_obj = json_object_get(root.get(), "model");
                    if (json_is_string(model_obj)) {
                        info.model = json_string_value(model_obj);
                    }
                }
            }
        } catch (...) {
            Logger::warn("Failed to load metadata for: %s", file.title.c_str());
        }
        
        result.conversations.push_back(info);
    }
    
    result.has_more = (end_idx < files.size());
    
    return result;
}

// Legacy function - loads ALL conversations (deprecated, kept for compatibility)
std::vector<ConversationManager::ConversationInfo> 
ConversationManager::list_conversations_info() {
    
    std::vector<ConversationInfo> all_results;
    size_t page = 0;
    const size_t page_size = 100;  // Internal page size for bulk loading
    
    while (true) {
        auto page_result = list_conversations_page(page, page_size);
        
        if (page_result.conversations.empty()) break;
        
        all_results.insert(all_results.end(),
                          page_result.conversations.begin(),
                          page_result.conversations.end());
        
        if (!page_result.has_more) break;
        page++;
    }
    
    return all_results;
}

ConversationHistory ConversationManager::load_conversation(const std::string& title,
                                                           std::string& out_model) {
    const Config& config = Config::instance();
    ConversationHistory messages;
    std::string filename = config.get_config_dir() + "/conversations/" + title + ".json";
    
    out_model.clear();
    
    Logger::debug("Attempting to load conversation from: %s", filename.c_str());
    
    if (!std::filesystem::exists(filename)) {
        Logger::warn("Conversation file does not exist: %s", filename.c_str());
        return messages;
    }
    
    std::ifstream file(filename);
    if (!file.is_open()) {
        Logger::error("Failed to open conversation file: %s (permission issue?)", 
                     filename.c_str());
        return messages;
    }
    
    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    
    json_error_t error;
    JsonPtr root(json_loads(content.c_str(), 0, &error));
    
    if (!root) {
        Logger::error("Failed to parse JSON in %s: %s", filename.c_str(), error.text);
        return messages;
    }
    
    json_t* model_obj = json_object_get(root.get(), "model");
    if (json_is_string(model_obj)) {
        out_model = json_string_value(model_obj);
        Logger::info("Restored conversation model: %s", out_model.c_str());
    }
    
    json_t* msg_array = json_object_get(root.get(), "messages");
    if (!json_is_array(msg_array)) {
        Logger::error("Invalid conversation format: no messages array in %s", 
                     filename.c_str());
        return messages;
    }
    
    size_t idx;
    json_t* value;
    json_array_foreach(msg_array, idx, value) {
        json_t* role_obj = json_object_get(value, "role");
        json_t* content_obj = json_object_get(value, "content");
        
        if (json_is_string(role_obj) && json_is_string(content_obj)) {
            Message msg;
            msg.role = json_string_value(role_obj);
            msg.content = json_string_value(content_obj);
            messages.push_back(msg);
        }
    }
    
    return messages;
}

void ConversationManager::save_conversation(const std::string& title,
                                           const ConversationHistory& messages,
                                           const std::string& model,
                                           bool interrupted) {
    ensure_config_dir();
    const Config& config = Config::instance();
    std::string filename = config.get_config_dir() + "/conversations/" + title + ".json";

    JsonPtr root(json_object());
    if (!root) {
        Logger::error("Failed to create JSON object for conversation save");
        return;
    }

    JsonPtr msg_array(json_array());
    if (!msg_array) {
        Logger::error("Failed to create messages array for conversation save");
        return;
    }

    for (const auto& msg : messages) {
        json_t* j_msg = json_object();
        if (!j_msg) continue;

        json_object_set_new(j_msg, "role", json_string(msg.role.c_str()));
        json_object_set_new(j_msg, "content", json_string(msg.content.c_str()));

        if (json_array_append_new(msg_array.get(), j_msg) != 0) {
            Logger::error("Failed to append message to conversation array");
            json_decref(j_msg);
        }
    }

    if (json_object_set_new(root.get(), "messages", msg_array.get()) != 0) {
        Logger::error("Failed to attach messages to conversation JSON");
        return;
    }
    msg_array.release();

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&time_t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    json_object_set_new(root.get(), "timestamp", json_string(oss.str().c_str()));

    if (!model.empty()) {
        json_object_set_new(root.get(), "model", json_string(model.c_str()));
    }

    json_object_set_new(root.get(), "interrupted", interrupted ? json_true() : json_false());

    char* json_str = json_dumps(root.get(), JSON_INDENT(2));
    if (!json_str) {
        Logger::error("Failed to serialize conversation JSON for %s", title.c_str());
        return;
    }

    JsonStrPtr json_guard(json_str);

    std::string temp_filename = filename + ".tmp";
    {
        std::ofstream file(temp_filename);
        if (!file.is_open()) {
            Logger::error("Failed to open conversation file for writing: %s", 
                         temp_filename.c_str());
            return;
        }

        file << json_guard.get();
        file.close();
        if (file.fail()) {
            Logger::error("Failed to write conversation data: %s", temp_filename.c_str());
            std::filesystem::remove(temp_filename);
            return;
        }
    }

    try {
        if (std::filesystem::exists(filename)) {
            std::filesystem::rename(filename, filename + ".bak");
        }
        std::filesystem::rename(temp_filename, filename);
        Logger::debug("Successfully saved conversation: %s (model: %s, interrupted: %s)",
                     title.c_str(), model.c_str(), interrupted ? "yes" : "no");
    } catch (const std::exception& e) {
        Logger::error("Failed to replace conversation file %s: %s", 
                     filename.c_str(), e.what());
        try {
            if (std::filesystem::exists(filename + ".bak")) {
                std::filesystem::rename(filename + ".bak", filename);
            }
        } catch (...) {
            Logger::error("Failed to restore conversation backup: %s", title.c_str());
        }
    }
}

std::string ConversationManager::generate_title() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return "conv_" + std::to_string(timestamp);
}
