#ifndef SSE_PARSER_HPP
#define SSE_PARSER_HPP

#include <functional>
#include <mutex>
#include <string>
#include <vector>

class SSEParser {
public:
    enum class EventType { CONTENT, REASONING, TOOL_CALL, DONE, UNKNOWN };
    
    struct Event {
        EventType type = EventType::UNKNOWN;
        std::string data;
        std::string tool_id;
        std::string tool_name;
        int tool_index = -1;
    };

    using EventCallback = std::function<void(const Event&)>;

    void set_callback(EventCallback cb);
    void feed(const char* ptr, size_t size);
    void flush();  // ← NEW: Flush remaining buffer
    
    // NEW: Get/set reasoning state for tag insertion
    bool is_in_reasoning() const { return in_reasoning_; }
    void set_in_reasoning(bool val) { in_reasoning_ = val; }

private:
    std::string buffer;
    EventCallback event_cb;
    mutable std::mutex cb_mutex;
    
    struct {
        std::vector<std::string> arguments;
        std::vector<std::string> names;
        std::vector<std::string> ids;
    } tool_state;
    
    // NEW: Track if we're currently in reasoning mode
    bool in_reasoning_ = false;

    Event parse_event(const std::string& line);
};

#endif
