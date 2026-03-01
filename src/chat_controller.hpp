#ifndef CHAT_CONTROLLER_HPP
#define CHAT_CONTROLLER_HPP

#include "message.hpp"
#include "output_formatter.hpp"
#include <string>
#include <atomic>
#include <vector>

extern std::atomic<bool> g_running;
extern std::atomic<bool> g_interrupted;
extern std::atomic<bool> g_terminate;

class ChatController {
public:
    static void process_single_message(const std::string& model,
                                       const std::string& input,
                                       const ConversationHistory& history,
                                       const std::string& save_title = "",
                                       size_t stream_delay_ms = 0,
                                       size_t stream_chunk_size = 1);
    
    static std::string match_model(const std::vector<std::string>& models, const std::string& query);
};

struct TerminalColorGuard {
    ~TerminalColorGuard();
};

struct CursorGuard {
    CursorGuard();
    ~CursorGuard();
};

#endif
