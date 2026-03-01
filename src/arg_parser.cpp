#include "arg_parser.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "version.hpp"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <functional>
#include <unordered_map>

static constexpr const char* COMBINED_BOOL_FLAGS = "hvMlSfrpiLFd";

// Unified flag action handler to eliminate duplication between combined and long flags
class FlagHandler {
public:
    using Action = std::function<void()>;
    
    FlagHandler(Args& arguments) : args(arguments) {
        setup_handlers();
    }
    
    void handle(char flag) {
        auto it = handlers.find(flag);
        if (it != handlers.end()) {
            it->second();
        } else {
            Logger::error("Unknown flag -%c", flag);
            exit(1);
        }
    }
    
    void handle_string(char flag, const std::string& value) {
        switch (flag) {
            case 'e': Config::instance().set_api_base(value); break;
            case 'm': args.model = value; break;
            case 'c': args.continue_title = value; break;
            case 'x': Config::instance().set_http_proxy(value); break;
            case 's': args.show_title = value; break;
            case 'R': Config::instance().set_system_role(value); break;
            default: 
                Logger::error("Flag -%c does not accept string value", flag);
                exit(1);
        }
    }

private:
    Args& args;
    std::unordered_map<char, Action> handlers;
    
    void setup_handlers() {
        handlers['h'] = [&]() { show_help(); exit(0); };
        handlers['v'] = [&]() { show_version(); exit(0); };
        handlers['M'] = [&]() { args.ask_model = true; };
        handlers['l'] = [&]() { args.list_conversations = true; };
        handlers['L'] = [&]() { args.list_models = true; };
        handlers['S'] = [&]() { args.show_last = true; };
        handlers['f'] = [&]() { Config::instance().set_format_markdown(true); };
        handlers['r'] = [&]() { args.force_raw_json = true; };
        handlers['p'] = [&]() { Config::instance().set_preview_enabled(true); };
        handlers['i'] = [&]() { args.isolate = true; };
        handlers['F'] = [&]() { 
            args.no_format = true;
            Config::instance().set_format_markdown(false);
            Config::instance().set_preview_enabled(false);
            Config::instance().set_markdown_enabled(false);
        };
        handlers['d'] = [&]() { args.debug_logging = true; };
    }
    
    void show_help() {
        std::cout << "Usage: high [OPTIONS] [PROMPT]\n"
                  << "\nTUI LLM Chat Interface v" << HIGH_VERSION_STRING << "\n"
                  << "\nOptions:\n"
                  << "  -e, --api URL        API base (default: http://localhost:8042/v1)\n"
                  << "  -m, --model MODEL    Model to use\n"
                  << "  -M, --ask-model      Interactive model selection\n"
                  << "  -L, --models         List available models\n"
                  << "  -c, --continue TITLE Continue from saved conversation\n"
                  << "  -C, --continue-last  Continue from last conversation\n"
                  << "  -i, --isolate        Branch conversation (save as new)\n"
                  << "  -l, --list           List saved conversations\n"
                  << "  -s, --show TITLE     Show a saved conversation\n"
                  << "  -S, --show-last      Show last saved conversation\n"
                  << "  -x, --http-proxy URL HTTP proxy\n"
                  << "  -f, --format         Enable markdown formatting\n"
                  << "  -r, --raw            Output raw JSON (for debugging)\n"
                  << "  -F, --no-format      Plain text output (no markdown, overrides -f and env vars)\n"
                  << "  -p, --preview        Preview code blocks\n"
                  << "  -d, --debug          Enable debug logging\n"
                  << "      --md             Enable markdown syntax highlighting\n"
                  << "      --tps RATE       Tokens per second for streaming\n"
                  << "      --chunk-size N   Characters per token chunk\n"
                  << "  -R, --role ROLE      System role/prompt\n"
                  << "      --theme THEME    Syntax highlight theme\n"
                  << "      --max-tokens N   Maximum response tokens\n"
                  << "      --log-file PATH  Log to file\n"
                  << "      --tab-width N    Tab width in spaces (default: 8, env: LLM_TAB_WIDTH)\n"
                  << "      --config-dump    Output config as env variables\n"
                  << "      --remove-think-tags  Strip think tags from assistant output\n"
                  << "  -v, --version        Show version information\n"
                  << "  -h, --help           Show help\n"
                  << "\nExamples:\n"
                  << "  high -l                    # Interactive list\n"
                  << "  high -F -l                 # List for piping (no formatting)\n"
                  << "  high -F -s myconv          # Plain text conversation\n"
                  << "  high -F -s myconv | bat    # Pipe to external highlighter\n"
                  << "  high -r -s myconv          # Raw JSON output\n"
                  << "  high -d -l                 # List with debug logging\n"
                  << "  high -s myconv --remove-think-tags  # Show without reasoning\n"
                  << "  alias high-plain='high -F' # Alias for unformatted output\n";
    }
    
    void show_version() {
        std::cout << "high v" << HIGH_VERSION_STRING << "\n"
                  << "TUI LLM Chat Interface\n"
                  << "\n"
                  << "Build Information:\n"
                  << "  Version: " << HIGH_VERSION_STRING << "\n"
                  << "  Major:   " << HIGH_VERSION_MAJOR << "\n"
                  << "  Minor:   " << HIGH_VERSION_MINOR << "\n"
                  << "  Patch:   " << HIGH_VERSION_PATCH << "\n"
                  << "\n"
                  << "License: GPLv3\n";
    }
};

bool ArgParser::is_combined_flag(const std::string& arg) {
    if (arg.size() < 2 || arg[0] != '-' || arg[1] == '-') return false;
    for (size_t i = 1; i < arg.size(); ++i) {
        if (!std::strchr(COMBINED_BOOL_FLAGS, arg[i])) return false;
    }
    return true;
}

Args ArgParser::parse(int argc, char* argv[]) {
    Args args;
    FlagHandler handler(args);
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (is_combined_flag(arg)) {
            for (size_t j = 1; j < arg.size(); ++j) {
                handler.handle(arg[j]);
            }
            continue;
        }

        // Long arguments mapping to single characters or special handling
        if (arg == "-h" || arg == "--help") { handler.handle('h'); }
        else if (arg == "-v" || arg == "--version") { handler.handle('v'); }
        else if (arg == "-e" || arg == "--api") { 
            if (i+1 < argc) Config::instance().set_api_base(argv[++i]); 
        }
        else if (arg == "-m" || arg == "--model") { 
            if (i+1 < argc) args.model = argv[++i]; 
        }
        else if (arg == "-M" || arg == "--ask-model") { handler.handle('M'); }
        else if (arg == "--models") { handler.handle('L'); }
        else if (arg == "-c" || arg == "--continue") { 
            if (i+1 < argc) args.continue_title = argv[++i]; 
        }
        else if (arg == "-C" || arg == "--continue-last") { args.continue_title = "__last__"; }
        else if (arg == "-l" || arg == "--list") { handler.handle('l'); }
        else if (arg == "-s" || arg == "--show") { 
            if (i+1 < argc) args.show_title = argv[++i]; 
        }
        else if (arg == "-S" || arg == "--show-last") { handler.handle('S'); }
        else if (arg == "-x" || arg == "--http-proxy") { 
            if (i+1 < argc) Config::instance().set_http_proxy(argv[++i]); 
        }
        else if (arg == "-f" || arg == "--format") { handler.handle('f'); }
        else if (arg == "-r" || arg == "--raw") { handler.handle('r'); }
        else if (arg == "-p" || arg == "--preview") { handler.handle('p'); }
        else if (arg == "-F" || arg == "--no-format") { handler.handle('F'); }
        else if (arg == "-i" || arg == "--isolate") { handler.handle('i'); }
        else if (arg == "-R" || arg == "--role") { 
            if (i+1 < argc) Config::instance().set_system_role(argv[++i]); 
        }
        else if (arg == "--md") { Config::instance().set_markdown_enabled(true); }
        else if (arg == "--max-tokens") {
            if (i+1 < argc) {
                try { Config::instance().set_max_tokens(std::stoi(argv[++i])); }
                catch (...) { Logger::error("Invalid max-tokens: %s", argv[i]); exit(1); }
            }
        } else if (arg == "--log-file") { 
            if (i+1 < argc) args.log_file = argv[++i]; 
        }
        else if (arg == "--theme") { 
            if (i+1 < argc) Config::instance().set_highlight_theme(argv[++i]); 
        }
        else if (arg == "--tps" || arg == "--tokens-per-second") {
            if (i+1 < argc) {
                try {
                    double tps = std::stod(argv[++i]);
                    if (tps > 0) args.stream_delay_ms = static_cast<size_t>(1000.0 / tps);
                    else { Logger::error("TPS must be positive"); exit(1); }
                } catch (...) { Logger::error("Invalid TPS: %s", argv[i]); exit(1); }
            }
        } else if (arg == "--chunk-size") {
            if (i+1 < argc) {
                try {
                    int chunk = std::stoi(argv[++i]);
                    args.stream_chunk_size = (chunk > 0) ? static_cast<size_t>(chunk) : 1;
                } catch (...) { Logger::error("Invalid chunk-size: %s", argv[i]); exit(1); }
            }
        } else if (arg == "--tab-width") {
            if (i+1 < argc) {
                try {
                    int tw = std::stoi(argv[++i]);
                    if (tw > 0 && tw <= 16) {
                        args.tab_width = tw;
                        Config::instance().set_tab_width(tw);
                    } else {
                        Logger::error("Tab width must be between 1 and 16");
                        exit(1);
                    }
                } catch (...) { 
                    Logger::error("Invalid tab-width: %s", argv[i]); 
                    exit(1); 
                }
            }
        } else if (arg == "--config-dump") {
            args.config_dump = true;
        } else if (arg == "-d" || arg == "--debug") {
            handler.handle('d');
        } else if (arg == "--remove-think-tags") {
            args.remove_think_tags = true;
        } else if (!arg.empty() && arg[0] != '-') {
            if (!args.prompt.empty()) args.prompt += " ";
            args.prompt += arg;
        } else {
            Logger::error("Unknown option: %s", arg.c_str());
            exit(1);
        }
    }
    return args;
}

void ArgParser::show_help() {
    Args dummy;
    FlagHandler(dummy).handle('h');
}

void ArgParser::show_version() {
    Args dummy;
    FlagHandler(dummy).handle('v');
}
