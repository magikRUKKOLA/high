#ifndef CONVERSATION_MANAGER_HPP
#define CONVERSATION_MANAGER_HPP

#include "message.hpp"
#include <vector>
#include <string>
#include <chrono>

class ConversationManager {
public:
    struct ConversationInfo {
        std::string title;
        bool interrupted = false;
        std::chrono::system_clock::time_point timestamp;
        std::string model;
    };
    
    // Pagination result structure
    struct ConversationPage {
        std::vector<ConversationInfo> conversations;
        size_t total_count = 0;
        size_t page = 0;
        size_t page_size = 0;
        bool has_more = false;
    };
    
    static void ensure_config_dir();
    
    // NEW: Fast count without loading metadata
    static size_t count_conversations();
    
    // NEW: Paginated listing - only loads metadata for visible items
    static ConversationPage list_conversations_page(size_t page, size_t page_size);
    
    // Legacy: Load all (for backward compatibility, but deprecated)
    static std::vector<ConversationInfo> list_conversations_info();
    
    static ConversationHistory load_conversation(const std::string& title, 
                                                 std::string& out_model);
    
    static void save_conversation(const std::string& title, 
                                 const ConversationHistory& messages,
                                 const std::string& model,
                                 bool interrupted = false);
    
    static std::string generate_title();
};

#endif
