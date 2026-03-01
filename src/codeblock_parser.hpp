#ifndef CODEBLOCK_PARSER_HPP
#define CODEBLOCK_PARSER_HPP

#include <string>
#include <unordered_set>

class CodeBlockParser {
public:
    struct State { 
        enum Type { NONE, IN_BLOCK } type = NONE;
        size_t fence_indent = 0;  // Tracks opening fence indentation
        bool at_line_start = false;
        //bool at_line_start = true;
    };
    
    struct ParseResult {
        enum Type { TEXT_CHUNK, OPEN_FENCE_COMPLETE, CLOSE_FENCE, NEED_MORE_DATA };
        Type type = NEED_MORE_DATA;
        std::string content;
        std::string language;
        size_t advance_by = 0;
    };

    static std::unordered_set<std::string> supported_languages;
    
    static void load_supported_languages();
    static bool is_language_supported(const std::string& lang);
    static ParseResult parse_next(const std::string& data, size_t pos, State& current_state, bool is_final = false);
    
private:
    static bool is_valid_fence_language_char(char c);
    static std::string extract_language_from_fence(const std::string& line_after_fence);
    static bool is_partial_fence(const std::string& str);
};

#endif
