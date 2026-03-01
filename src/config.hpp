#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <mutex>
#include <string>
#include <iostream>

class Config {
public:
    static Config& instance();
    
    void load_from_env();
    void dump_as_env(std::ostream& out = std::cout) const;
    
    // Getters
    std::string get_api_base() const;
    std::string get_config_dir() const;
    std::string get_http_proxy() const;
    std::string get_system_role() const;
    int get_max_tokens() const;
    double get_temperature() const;
    bool is_verbose() const;
    bool format_markdown_enabled() const;
    bool raw_output_enabled() const;
    int get_timeout() const;
    std::string get_highlight_theme() const;
    bool preview_enabled() const;
    bool markdown_enabled() const;
    int get_tab_width() const;

    // Setters
    void set_api_base(const std::string& val);
    void set_http_proxy(const std::string& val);
    void set_system_role(const std::string& val);
    void set_max_tokens(int val);
    void set_verbose(bool val);
    void set_format_markdown(bool val);
    void set_raw_output(bool val);
    void set_highlight_theme(const std::string& theme);
    void set_preview_enabled(bool val);
    void set_markdown_enabled(bool val);
    void set_tab_width(int val);

private:
    Config();
    
    mutable std::mutex mutex;
    std::string api_base{"http://localhost:8042/v1"};
    std::string config_dir;
    std::string http_proxy;
    std::string system_role{"You are a helpful assistant."};
    int max_tokens = 0;
    double temperature = 1.0;
    bool verbose = false;
    bool format_markdown = true;
    bool raw_output = false;
    int timeout = 36000;
    std::string highlight_theme{"xoria256"};
    bool preview_code_blocks = false;
    bool markdown_enabled_ = true;
    int tab_width = 8;  // Default tab width
};

#endif
