#include "sse_parser.hpp"
#include "logger.hpp"
#include "json_utils.hpp"
#include <cstring>
#include <memory>

void SSEParser::set_callback(EventCallback cb) {
    std::lock_guard lock(cb_mutex);
    event_cb = std::move(cb);
}

void SSEParser::feed(const char* ptr, size_t size) {
    if (!ptr || size == 0) return;
    
    // DEBUG: Log what we receive
    Logger::debug("[SSE] feed: received %zu bytes, buffer was %zu bytes", 
                  size, buffer.size());
    if (size > 0) {
        // FIX: Proper truncation with ellipsis
        size_t debug_size = std::min(size, (size_t)100);
        std::string debug_preview = std::string(ptr + (size - debug_size), debug_size);
        Logger::debug("[SSE] Last %zu bytes: '%s'",
                     debug_size, debug_preview.c_str());
    }
    
    buffer.append(ptr, size);
    
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (line.empty()) continue;
        
        if (line.compare(0, 5, "data:") == 0) {
            line = line.substr(5);
            size_t start = line.find_first_not_of(" \t");
            if (start != std::string::npos) line = line.substr(start);
            
            Logger::debug("[SSE] Processing line: '%s'", line.c_str());
            
            if (line == "[DONE]") {
                Event done_ev;
                done_ev.type = EventType::DONE;
                std::lock_guard lock(cb_mutex);
                if (event_cb) event_cb(done_ev);
                tool_state.arguments.clear();
                tool_state.names.clear();
                tool_state.ids.clear();
                continue;
            }
            
            auto ev = parse_event(line);
            if (ev.type != EventType::UNKNOWN) {
                std::lock_guard lock(cb_mutex);
                if (event_cb) event_cb(ev);
            }
        }
    }
    
    Logger::debug("[SSE] After feed: buffer has %zu bytes remaining", buffer.size());
}

// NEW: Flush remaining buffer content
void SSEParser::flush() {
    Logger::debug("[SSE] flush: buffer has %zu bytes", buffer.size());
    
    if (buffer.empty()) {
        Logger::debug("[SSE] flush: buffer empty, nothing to do");
        return;
    }
    
    // Process any remaining content as a final line
    std::string line = buffer;
    buffer.clear();
    
    if (line.empty()) return;
    
    Logger::debug("[SSE] flush: processing final line: '%s'", line.c_str());
    
    if (line.compare(0, 5, "data:") == 0) {
        line = line.substr(5);
        size_t start = line.find_first_not_of(" \t");
        if (start != std::string::npos) line = line.substr(start);
        
        if (line == "[DONE]") {
            Event done_ev;
            done_ev.type = EventType::DONE;
            std::lock_guard lock(cb_mutex);
            if (event_cb) event_cb(done_ev);
            tool_state.arguments.clear();
            tool_state.names.clear();
            tool_state.ids.clear();
            return;
        }
        
        auto ev = parse_event(line);
        if (ev.type != EventType::UNKNOWN) {
            Logger::debug("[SSE] flush: emitting final event, type=%d, data='%s'", 
                         static_cast<int>(ev.type), ev.data.c_str());
            std::lock_guard lock(cb_mutex);
            if (event_cb) event_cb(ev);
        }
    }
}

SSEParser::Event SSEParser::parse_event(const std::string& line) {
    Event ev;
    json_error_t error;
    json_t* root = json_loads(line.c_str(), 0, &error);
    if (!root) {
        Logger::debug("[SSE] parse_event: JSON parse failed: %s", error.text);
        return ev;
    }
    
    JsonPtr root_guard(root);
    
    json_t* choices = json_object_get(root, "choices");
    if (!json_is_array(choices) || json_array_size(choices) == 0) {
        Logger::debug("[SSE] parse_event: no choices in JSON");
        return ev;
    }
    json_t* choice = json_array_get(choices, 0);
    if (!choice) return ev;
    json_t* delta = json_object_get(choice, "delta");
    if (!delta) return ev;

    json_t* content_obj = json_object_get(delta, "content");
    if (json_is_string(content_obj)) {
        ev.type = EventType::CONTENT;
        ev.data = json_string_value(content_obj);
        Logger::debug("[SSE] parse_event: CONTENT, data='%s'", ev.data.c_str());
        return ev;
    }
    
    content_obj = json_object_get(delta, "reasoning_content");
    if (json_is_string(content_obj)) {
        ev.type = EventType::REASONING;
        ev.data = json_string_value(content_obj);
        Logger::debug("[SSE] parse_event: REASONING, data='%s'", ev.data.c_str());
        return ev;
    }

    json_t* tool_calls = json_object_get(delta, "tool_calls");
    if (json_is_array(tool_calls) && json_array_size(tool_calls) > 0) {
        ev.type = EventType::TOOL_CALL;
        json_t* tool_call = json_array_get(tool_calls, 0);
        if (!tool_call) return ev;
        
        json_t* index_obj = json_object_get(tool_call, "index");
        if (json_is_integer(index_obj)) {
            long idx = json_integer_value(index_obj);
            if (idx >= 0) {
                ev.tool_index = static_cast<int>(idx);
                if (static_cast<size_t>(ev.tool_index) >= tool_state.arguments.size()) {
                    size_t old_size = tool_state.arguments.size();
                    tool_state.arguments.resize(ev.tool_index + 1);
                    tool_state.names.resize(ev.tool_index + 1);
                    tool_state.ids.resize(ev.tool_index + 1);
                    for (size_t i = old_size; i < tool_state.arguments.size(); ++i) {
                        tool_state.arguments[i].clear();
                        tool_state.names[i].clear();
                        tool_state.ids[i].clear();
                    }
                }
            }
        }

        if (ev.tool_index >= 0 && ev.tool_index < static_cast<int>(tool_state.arguments.size())) {
            json_t* id_obj = json_object_get(tool_call, "id");
            if (json_is_string(id_obj)) {
                ev.tool_id = json_string_value(id_obj);
                tool_state.ids[ev.tool_index] = ev.tool_id;
            } else {
                ev.tool_id = tool_state.ids[ev.tool_index];
            }

            json_t* function = json_object_get(tool_call, "function");
            if (function) {
                json_t* name_obj = json_object_get(function, "name");
                if (json_is_string(name_obj)) {
                    ev.tool_name = json_string_value(name_obj);
                    tool_state.names[ev.tool_index] = ev.tool_name;
                } else {
                    ev.tool_name = tool_state.names[ev.tool_index];
                }

                json_t* args_obj = json_object_get(function, "arguments");
                if (json_is_string(args_obj)) {
                    const char* args_str = json_string_value(args_obj);
                    tool_state.arguments[ev.tool_index] += args_str;
                    ev.data = args_str;
                    Logger::debug("[SSE] parse_event: TOOL_CALL, data='%s'", ev.data.c_str());
                    return ev;
                }
            }
        }
        return ev;
    }
    return ev;
}
