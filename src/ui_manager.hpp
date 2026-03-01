#ifndef UI_MANAGER_HPP
#define UI_MANAGER_HPP

#include "conversation_manager.hpp"
#include <vector>
#include <string>

class UIManager {
public:
    static std::string select_conversation_interactive(const std::vector<ConversationManager::ConversationInfo>& conv_infos);
    static bool prompt_save_interrupted();
};

#endif
