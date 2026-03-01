#ifndef API_CLIENT_HPP
#define API_CLIENT_HPP

#include "message.hpp"
#include "sse_parser.hpp"
#include <vector>
#include <string>
#include <atomic>

class APIClient {
public:
    static std::vector<std::string> fetch_models();
    static bool send_chat_request(const std::string& model, 
                                 const ConversationHistory& history,
                                 SSEParser& parser,
                                 std::atomic<bool>& running);
};

#endif
