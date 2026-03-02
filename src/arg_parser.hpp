#ifndef ARG_PARSER_HPP
#define ARG_PARSER_HPP

#include <string>
#include <vector>

struct Args {
    std::string model;
    bool ask_model = false;
    bool list_conversations = false;
    bool list_models = false;
    std::string show_title;
    bool show_last = false;
    std::string continue_title;
    std::string prompt;
    std::string log_file;
    bool preview = false;
    bool isolate = false;
    bool config_dump = false;
    bool no_format = false;
    bool force_raw_json = false;
    bool show_version = false;
    bool remove_think_tags = false;
    size_t stream_delay_ms = 0;
    size_t stream_chunk_size = 1;
    bool debug_logging = false;
    int tab_width = 0;  // 0 = use config default
    bool show_banner = false;
};

class ArgParser {
public:
    static Args parse(int argc, char* argv[]);
    
private:
    static void show_help();
    static void show_version();
    static bool is_combined_flag(const std::string& arg);
};

#endif
