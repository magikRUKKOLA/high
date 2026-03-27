#include "chat_controller.hpp"
#include "api_client.hpp"
#include "config.hpp"
#include "conversation_manager.hpp"
#include "loader.hpp"
#include "logger.hpp"
#include "sse_parser.hpp"
#include "ui_manager.hpp"
#include "output_formatter.hpp"
#include "syntax_highlighter.hpp"
#include "common.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <strings.h>
#include <cstring>
#include <unistd.h>
#include <regex>

std::atomic<bool> g_running{true};
std::atomic<bool> g_interrupted{false};
std::atomic<bool> g_terminate{false};

TerminalColorGuard::~TerminalColorGuard() {
    reset_terminal();
}

CursorGuard::CursorGuard() {
    hide_cursor();
}

CursorGuard::~CursorGuard() {
    show_cursor();
}

std::string ChatController::match_model(const std::vector<std::string>& models, const std::string& query) {
    if (models.empty()) return query;
    
    // 1. Exact match (case-sensitive)
    for (const auto& m : models) {
        if (m == query) {
            Logger::debug("[Model] Exact match found: '%s'", m.c_str());
            return m;
        }
    }
    
    // 2. Lowercase match (case-insensitive exact)
    std::string lower_q = query;
    for (char& c : lower_q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    
    for (const auto& m : models) {
        std::string lower_m = m;
        for (char& c : lower_m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower_m == lower_q) {
            Logger::debug("[Model] Case-insensitive match found: '%s'", m.c_str());
            return m;
        }
    }
    
    // 3. Lowercase prefix match (case-insensitive prefix)
    for (const auto& m : models) {
        std::string lower_m = m;
        for (char& c : lower_m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower_m.find(lower_q) == 0) {
            Logger::debug("[Model] Prefix match found: '%s'", m.c_str());
            return m;
        }
    }
    
    // 4. Regexp match (case-insensitive)
    try {
        std::regex pattern(query, std::regex::icase | std::regex::extended);
        for (const auto& m : models) {
            if (std::regex_search(m, pattern)) {
                Logger::debug("[Model] Regexp match found: '%s' (pattern: '%s')", m.c_str(), query.c_str());
                return m;
            }
        }
    } catch (const std::regex_error& e) {
        Logger::debug("[Model] Regexp compilation failed for '%s': %s", query.c_str(), e.what());
    }
    
    // 5. Fallback to first model
    Logger::warn("Model '%s' not found, using default: %s", query.c_str(), models[0].c_str());
    return models[0];
}

void ChatController::process_single_message(const std::string& model,
                                           const std::string& input,
                                           const ConversationHistory& history,
                                           const std::string& save_title,
                                           size_t stream_delay_ms,
                                           size_t stream_chunk_size) {
    if (input.empty() && history.empty()) { Logger::error("No input provided"); return; }

    ConversationHistory full_history = history;
    full_history.push_back({"user", input});
    std::string assistant_response;
    bool got_response = false;
    bool format_output = Config::instance().format_markdown_enabled();
    FormatContext format_ctx;
    TerminalColorGuard color_guard;
    CursorGuard cursor_guard;
    SSEParser parser;
    std::atomic<bool> done{false};
    std::string THINK_OPEN  = "\n<th";
    std::string THINK_CLOSE = "\n</th";
    THINK_OPEN.append("ink>\n");
    THINK_CLOSE.append("ink>\n");
    
    bool in_reasoning = false;
    
    Loader& loader = Loader::get_instance();

    parser.set_callback([&](const SSEParser::Event& ev) {
        loader.stop();
        if (ev.type == SSEParser::EventType::DONE) {
            if (in_reasoning) {
                if (format_output) process_format_buffer(THINK_CLOSE, format_ctx, Config::instance().get_highlight_theme(), true);
                else std::cout << THINK_CLOSE << std::flush;
                format_ctx.md_highlighter.reset();
                assistant_response += THINK_CLOSE;
                in_reasoning = false;
                // Reset codeblock parser state when exiting reasoning
                format_ctx.cb_state = CodeBlockParser::State{};
                format_ctx.cb_state.at_line_start = true;
                format_ctx.parse_buffer.clear();
            }
            else if (format_output) process_format_buffer("", format_ctx, Config::instance().get_highlight_theme(), true);
            done.store(true);
            return;
        }
        if (ev.type == SSEParser::EventType::CONTENT || ev.type == SSEParser::EventType::REASONING) {
            if (!ev.data.empty()) got_response = true;

            if (ev.type == SSEParser::EventType::REASONING) {
                if (!in_reasoning) {
                    std::cout << THINK_OPEN << std::flush;
                    assistant_response += THINK_OPEN;
                    in_reasoning = true;
                }
            } else if (ev.type == SSEParser::EventType::CONTENT) {
                if (in_reasoning) {
                    if (format_output) process_format_buffer(THINK_CLOSE, format_ctx, Config::instance().get_highlight_theme(), true);
                    else std::cout << THINK_CLOSE << std::flush;
                    format_ctx.md_highlighter.reset();
                    assistant_response += THINK_CLOSE;
                    in_reasoning = false;
                    format_ctx.cb_state = CodeBlockParser::State{};
                    format_ctx.cb_state.at_line_start = true;
                    format_ctx.parse_buffer.clear();
                }
            }

            assistant_response += ev.data;

            if (stream_chunk_size > 0 && ev.data.size() > stream_chunk_size) {
                for (size_t i = 0; i < ev.data.size(); i += stream_chunk_size) {
                    size_t chunk_len = std::min(stream_chunk_size, ev.data.size() - i);
                    std::string chunk = ev.data.substr(i, chunk_len);
                    if (!format_output) { 
                        std::cout << expand_tabs(chunk) << std::flush; 
                    }
                    else { process_format_buffer(chunk, format_ctx, Config::instance().get_highlight_theme(), false); }

                    loader.update_color();
                    
                    if (stream_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(stream_delay_ms));
                }
            } else {
                if (!format_output) { 
                    std::cout << expand_tabs(ev.data) << std::flush; 
                    return; 
                }
                process_format_buffer(ev.data, format_ctx, Config::instance().get_highlight_theme(), false);
                if (stream_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(stream_delay_ms));
            }
        } else if (ev.type == SSEParser::EventType::TOOL_CALL) {
            if (!ev.tool_name.empty()) std::cerr << "[Tool: " << ev.tool_name << "]" << std::flush;
        }
    });

    const Config& config = Config::instance();
    if (!config.is_verbose() && isatty(STDERR_FILENO)) {
        loader.start();
    }

    bool was_interrupted_before = g_interrupted.load();
    Logger::debug("[Chat] Starting request. Initial interrupted state: %d", was_interrupted_before);

    bool success = APIClient::send_chat_request(model, full_history, parser, g_running);
    loader.stop();

    Logger::debug("[Chat] Request finished. Success: %d, g_interrupted: %d", success, g_interrupted.load());

    if (format_ctx.highlighter.is_active()) {
        format_ctx.highlighter.end();
    }

    std::cout << "\033[0m" << std::flush;

    if (!success && !was_interrupted_before && !g_interrupted.load()) {
        Logger::error("Request failed (network error or API issue)");
    }

    if ( success ) {
        auto start = std::chrono::steady_clock::now();
        while (!done && g_running) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > config.get_timeout()) {
                Logger::error("Response timeout");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (in_reasoning) {
        std::cout << THINK_CLOSE << std::flush;
        format_ctx.md_highlighter.reset();
        in_reasoning = false;
        format_ctx.cb_state = CodeBlockParser::State{};
        format_ctx.cb_state.at_line_start = true;
        format_ctx.parse_buffer.clear();
    }

    if (format_output && !done) process_format_buffer("", format_ctx, Config::instance().get_highlight_theme(), true);

    bool was_interrupted = g_interrupted.load();
    bool was_terminated = g_terminate.load();
    bool should_save = true;
    bool mark_interrupted = was_interrupted || was_terminated || !success;

    Logger::debug("[Chat] Post-stream state. Interrupted: %d, Terminated: %d, Success: %d",
                  was_interrupted, was_terminated, success);

    if (was_interrupted && !was_terminated) {
        Logger::debug("[Chat] Prompting user for save...");
        should_save = UIManager::prompt_save_interrupted();
    }

    if (should_save && full_history.size() > history.size()) {
        if (!assistant_response.empty()) full_history.push_back({"assistant", assistant_response});
        std::string title = save_title.empty() ? ConversationManager::generate_title() : save_title;
        ConversationManager::save_conversation(title, full_history, model, mark_interrupted);

        // Add newline to ensure we're on a fresh line
        std::cout << "\n" << std::flush;

        if (mark_interrupted) {
            std::cerr << "[Conversation saved (incomplete): " << title << "]\n";
        } else {
            std::cerr << "[Conversation: " << title << "]\n";
        }
    } else if (!should_save) {
        std::cout << "\n" << std::flush;  // ← Also add here for consistency
        std::cerr << "[Conversation discarded]\n";
    }
}
