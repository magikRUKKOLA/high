#ifndef CLIPBOARD_HPP
#define CLIPBOARD_HPP

#include <string>

class Clipboard {
public:
    static std::string get_content();
    static void set_content(const std::string& content);
};

#endif