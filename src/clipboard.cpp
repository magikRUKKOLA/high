#include "clipboard.hpp"
#include "common.hpp"
#include <cstdio>

std::string Clipboard::get_content() {
    UniqueFILE pipe(popen("xclip -selection clipboard -o 2>/dev/null || pbpaste 2>/dev/null", "r"));
    if (!pipe) return "";
    
    std::string content;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe.get())) {
        content += buffer;
    }
    
    if (!content.empty() && content.back() == '\n') content.pop_back();
    return content;
}

void Clipboard::set_content(const std::string& content) {
    UniqueFILE pipe(popen("xclip -selection clipboard 2>/dev/null || pbcopy 2>/dev/null", "w"));
    if (pipe) {
        fwrite(content.c_str(), 1, content.length(), pipe.get());
    }
}
