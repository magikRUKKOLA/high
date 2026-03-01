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
    
    static void ensure_config_dir();
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
