#ifndef MESSAGE_HPP
#define MESSAGE_HPP

#include <string>
#include <deque>  // Essential: explicit include required

struct Message {
    std::string role;
    std::string content;
};

using ConversationHistory = std::deque<Message>;

#endif
