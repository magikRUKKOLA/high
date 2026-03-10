#ifndef UI_MANAGER_HPP
#define UI_MANAGER_HPP

#include "conversation_manager.hpp"
#include <vector>
#include <string>

class UIManager {
public:
    // NEW: Interactive selection with pagination support
    static std::string select_conversation_interactive(
        const std::vector<ConversationManager::ConversationInfo>& conv_infos,
        size_t total_count,
        size_t current_page,
        size_t page_size,
        bool& page_changed,
        size_t& new_page);
    
    static bool prompt_save_interrupted();
};

#endif
