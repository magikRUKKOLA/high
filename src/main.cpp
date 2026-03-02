#include "config.hpp"
#include "logger.hpp"
#include "loader.hpp"
#include "banner.hpp"
#include "api_client.hpp"
#include "conversation_manager.hpp"
#include "arg_parser.hpp"
#include "clipboard.hpp"
#include "ui_manager.hpp"
#include "chat_controller.hpp"
#include "output_formatter.hpp"
#include "codeblock_parser.hpp"
#include "version.hpp"
#include "common.hpp"
#include <curl/curl.h>
#include <csignal>
#include <iostream>
#include <iomanip>
#include <thread>
#include <unistd.h>
#include <termios.h>
#include <chrono>
#include <sstream>
#include <fstream>
#include <sys/ioctl.h>
#include <limits>
#include <fcntl.h>
#include <poll.h>
#include <filesystem>

static std::string read_stdin() {
    std::stringstream ss;
    std::string line;
    while (std::getline(std::cin, line)) ss << line << "\n";
    return ss.str();
}

static void handle_signal(int sig) {
    if (sig == SIGINT) { g_interrupted.store(true); g_running.store(false); }
    else if (sig == SIGTERM) { g_terminate.store(true); g_running.store(false); }
}

// FIX: Lazy load supported languages only when formatting is actually needed
static void ensure_languages_loaded() {
    static std::atomic<bool> loaded{false};
    static std::mutex load_mutex;
    
    if (loaded.load(std::memory_order_acquire)) return;
    
    std::lock_guard<std::mutex> lock(load_mutex);
    if (loaded.load(std::memory_order_relaxed)) return;
    
    CodeBlockParser::load_supported_languages();
    loaded.store(true, std::memory_order_release);
}

// FIX: Fast path to find last conversation without parsing all JSON metadata
static std::string find_last_conversation_title() {
    const Config& config = Config::instance();
    std::string conv_dir = config.get_config_dir() + "/conversations";
    
    if (!std::filesystem::exists(conv_dir)) return "";
    
    std::string last_title;
    auto last_time = std::filesystem::file_time_type::min();
    
    // Only scan for most recent file, don't parse JSON
    for (const auto& entry : std::filesystem::directory_iterator(conv_dir)) {
        if (entry.path().extension() != ".json") continue;
        
        try {
            auto ftime = std::filesystem::last_write_time(entry.path());
            if (ftime > last_time) {
                last_time = ftime;
                last_title = entry.path().stem().string();
            }
        } catch (...) {
            Logger::warn("Skipping invalid conversation file: %s", entry.path().filename().c_str());
        }
    }
    
    return last_title;
}

// Consolidated conversation display logic used by both list and show modes
static int display_conversation(const std::string& title, const ConversationHistory& messages, 
                                const std::string& model, const Args& args, bool interactive_mode) {
    if (messages.empty()) {
        Logger::error("Conversation '%s' not found or empty", title.c_str());
        return 1;
    }

    bool raw_json = args.force_raw_json;
    bool format_output = !args.no_format && !raw_json && Config::instance().format_markdown_enabled();
    bool simulate_streaming = Config::instance().preview_enabled();

    // FIX: Load supported languages lazily only when formatting is enabled
    if (format_output || simulate_streaming) {
        ensure_languages_loaded();
    }

    TerminalColorGuard color_guard;
    CursorGuard cursor_guard;
    
    std::cout << "=== " << title;
    if (!model.empty()) std::cout << " (Model: " << model << ")";
    std::cout << " ===\n";

    Loader::get_instance();

    for (const auto& msg : messages) {
        if (g_interrupted.load()) break;
        
        std::string role = (msg.role == "user") ? "You" : 
                           (msg.role == "assistant") ? "Assistant" : "System";
        std::cout << "\n" << role << ":\n";
        
        // Apply remove_think_tags filter for assistant messages
        std::string content = msg.content;
        if (args.remove_think_tags && msg.role == "assistant") {
            content = remove_think_tags(content);
        }
        
        if (format_output) {
            FormatContext ctx;
            if (simulate_streaming && !content.empty()) {
                for (size_t i = 0; i < content.size(); i += args.stream_chunk_size) {
                    if (g_interrupted.load()) break;
                    std::string chunk = content.substr(i, std::min(args.stream_chunk_size, content.size() - i));
                    process_format_buffer(chunk, ctx, Config::instance().get_highlight_theme(), false);
                    if (args.stream_delay_ms > 0) 
                        std::this_thread::sleep_for(std::chrono::milliseconds(args.stream_delay_ms));
                }
                process_format_buffer("", ctx, Config::instance().get_highlight_theme(), true);
            } else {
                process_format_buffer(content, ctx, Config::instance().get_highlight_theme(), true);
            }
        } else {
            std::cout << expand_tabs(content);
            if (!content.empty() && content.back() != '\n') std::cout << "\n";
        }
    }
    std::cout << "\n";
    
    if (interactive_mode) {
        Clipboard::set_content(title);
    }
    
    return 0;
}

static int run_list_mode(const Args& args) {
    // FIX: Load languages for list mode (needed for formatting)
    ensure_languages_loaded();
    
    auto conv_infos = ConversationManager::list_conversations_info();

    bool raw_json = args.force_raw_json;

    size_t term_height = 24;
    if (isatty(STDOUT_FILENO)) {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
            term_height = w.ws_row;
        }
    }
    size_t pause_interval = (term_height > 2) ? (term_height - 2) : 20;

    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && args.prompt.empty() && !args.no_format && !raw_json) {
        std::string selected = UIManager::select_conversation_interactive(conv_infos);
        if (!selected.empty()) {
            std::string conv_model;
            const auto messages = ConversationManager::load_conversation(selected, conv_model);
            return display_conversation(selected, messages, conv_model, args, true);
        }
        return 0;
    } else {
        for (size_t i = 0; i < conv_infos.size(); ++i) {
            const auto& info = conv_infos[i];
            auto time_t = std::chrono::system_clock::to_time_t(info.timestamp);
            std::cout << info.title << " (" 
                      << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M") << ")";
            if (!info.model.empty()) std::cout << " [" << info.model << "]";
            if (info.interrupted) std::cout << " [Interrupted]";
            std::cout << "\n";

            if (isatty(STDOUT_FILENO) && !args.no_format && !raw_json && 
                (i + 1) % pause_interval == 0 && (i + 1) < conv_infos.size()) {
                std::cout << "-- Press Enter to continue --" << std::flush;
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }
        }
        return 0;
    }
}

static int run_show_mode(const Args& args, const std::string& target_title) {
    if (target_title.empty()) { 
        Logger::error("No conversation specified"); 
        return 1; 
    }

    if (args.force_raw_json) {
        std::string filename = Config::instance().get_config_dir() + "/conversations/" + target_title + ".json";
        std::ifstream file(filename);
        if (!file.is_open()) {
            Logger::error("Conversation '%s' not found", target_title.c_str());
            return 1;
        }
        std::cout << file.rdbuf();
        return 0;
    }

    std::string conv_model;
    const auto messages = ConversationManager::load_conversation(target_title, conv_model);
    return display_conversation(target_title, messages, conv_model, args, false);
}

static std::string read_model_selection_interactive(const std::vector<std::string>& models) {
    int tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd == -1) {
        Logger::warn("Could not open /dev/tty for model selection");
        return "";
    }

    struct termios orig_tio;
    if (tcgetattr(tty_fd, &orig_tio) == -1) {
        close(tty_fd);
        return "";
    }

    struct termios new_tio = orig_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    new_tio.c_cc[VMIN] = 0;
    new_tio.c_cc[VTIME] = 0;
    tcsetattr(tty_fd, TCSANOW, &new_tio);

    char response_buf[8] = {0};
    int buf_idx = 0;
    std::string result;

    struct pollfd fds[1];
    fds[0].fd = tty_fd;
    fds[0].events = POLLIN;

    while (buf_idx < static_cast<int>(sizeof(response_buf) - 1)) {
        if (g_interrupted.load()) {
            Logger::debug("Model selection interrupted");
            tcsetattr(tty_fd, TCSANOW, &orig_tio);
            close(tty_fd);
            return "";
        }

        int ret = poll(fds, 1, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            char ch;
            ssize_t r = read(tty_fd, &ch, 1);
            if (r == 1) {
                if (ch == '\n' || ch == '\r') {
                    break;
                } else if (ch == '\033') {
                    char seq[2];
                    read(tty_fd, seq, 2);
                    tcsetattr(tty_fd, TCSANOW, &orig_tio);
                    close(tty_fd);
                    return "";
                } else if (ch >= '0' && ch <= '9') {
                    response_buf[buf_idx++] = ch;
                    write(tty_fd, &ch, 1);
                }
            } else if (r == 0) {
                break;
            }
        }
    }

    tcsetattr(tty_fd, TCSANOW, &orig_tio);
    close(tty_fd);
    write(tty_fd, "\n", 1);

    if (buf_idx == 0) return "";

    try {
        int choice = std::stoi(response_buf);
        if (choice >= 1 && choice <= static_cast<int>(models.size())) {
            result = models[choice - 1];
        }
    } catch (...) {}

    return result;
}

static int run_chat_mode(Args& args) {
    std::string input = args.prompt;
    if (!isatty(STDIN_FILENO)) {
        std::string piped = read_stdin();
        if (!piped.empty()) input = input.empty() ? piped : input + "\n" + piped;
    }

    ConversationHistory initial_history;
    std::string restored_model;
    std::string target_title;
    if (!args.continue_title.empty()) {
        target_title = args.continue_title;
        if (target_title == "__last__") {
            // FIX: Use fast path for finding last conversation
            target_title = find_last_conversation_title();
        }
        if (!target_title.empty()) {
            initial_history = ConversationManager::load_conversation(target_title, restored_model);
            if (!restored_model.empty() && args.model.empty()) {
                args.model = restored_model;
            }
            
            if (args.remove_think_tags) {
                for (auto& msg : initial_history) {
                    if (msg.role == "assistant") {
                        msg.content = remove_think_tags(msg.content);
                    }
                }
            }
        }
    }

    if (!target_title.empty() && input.empty()) {
        std::cout << target_title << "\n";
        Clipboard::set_content(target_title);
        return 0;
    }

    if (input.empty()) { 
        Logger::error("No input provided"); 
        return 1; 
    }

    Loader& loader = Loader::get_instance();
    loader.start();
    const auto models = APIClient::fetch_models();

    if (models.empty()) {
        Logger::error("No models available at %s", Config::instance().get_api_base().c_str());
        loader.stop();
        return 1;
    }

    std::string selected_model;
    if (args.ask_model) {
        if (!isatty(STDIN_FILENO)) {
            Logger::error("Cannot use -M in non-TTY mode");
            loader.stop();
            return 1;
        }
        
        loader.stop();
        
        g_interrupted.store(false);
        g_running.store(true);
        
        for (size_t i = 0; i < models.size(); ++i) 
            std::cout << "  " << i + 1 << ". " << models[i] << "\n";
        std::cout << "Select model [1-" << models.size() << "]: ";
        std::cout << std::flush;
        
        selected_model = read_model_selection_interactive(models);
        
        if (selected_model.empty()) {
            Logger::warn("Model selection cancelled, using first model");
            selected_model = models[0];
        }
    } else {
        selected_model = args.model.empty() ? models[0] : ChatController::match_model(models, args.model);
    }

    loader.update_model(selected_model);
    loader.start();

    const std::string save_title = (args.continue_title.empty() || 
                                    args.continue_title == "__last__" || 
                                    args.isolate) ? "" : args.continue_title;
    
    g_interrupted.store(false);
    g_running.store(true);
    
    ChatController::process_single_message(selected_model, input, initial_history, save_title);
    return 0;
}

int main(int argc, char* argv[]) {
    std::setlocale(LC_ALL, "");
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    Config::instance().load_from_env();

    Args args = ArgParser::parse(argc, argv);

    if (args.debug_logging) {
        Logger::init(LogLevel::DEBUG);
    } else if (!Config::instance().is_verbose()) {
        Logger::init(LogLevel::WARN);
    }

    if (args.config_dump) {
        Config::instance().dump_as_env(std::cout);
        return 0;
    }

    if (args.show_banner) {
        Banner::run(0.0f);
        return 0;
    }

    if (!args.log_file.empty()) {
        Logger::init(args.log_file,
                     args.debug_logging || Config::instance().is_verbose()
                         ? LogLevel::DEBUG
                         : LogLevel::INFO);
    }

    // FIX: Don't load languages unconditionally - only when needed
    if (Config::instance().format_markdown_enabled()) {
        CodeBlockParser::load_supported_languages();
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        Logger::error("Failed to initialize CURL");
        return 1;
    }

    Loader::get_instance();

    int ret = 0;
    if (args.list_models) {
        const auto models = APIClient::fetch_models();
        if (models.empty()) { 
            Logger::error("No models available"); 
            ret = 1; 
        } else {
            for (const auto& m : models) std::cout << m << "\n";
        }
    } else if (args.list_conversations) {
        ret = run_list_mode(args);
    } else if (!args.show_title.empty() || args.show_last) {
        std::string title = args.show_title;
        
        // FIX: Use fast path for -S (show-last) - don't load all conversations
        if (args.show_last) {
            title = find_last_conversation_title();
            if (title.empty()) {
                Logger::error("No conversations found");
                return 1;
            }
        }
        
        ret = run_show_mode(args, title);
    } else {
        ret = run_chat_mode(args);
    }
    
    Logger::shutdown();
    curl_global_cleanup();
    return ret;
}
