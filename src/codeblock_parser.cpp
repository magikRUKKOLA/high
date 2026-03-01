#include "codeblock_parser.hpp"
#include "common.hpp"
#include "logger.hpp"
#include <cctype>
#include <algorithm>
#include <cstring>
#include <array>
#include <memory>
#include <cstdio>
#include <sstream>
#include <iomanip>

std::unordered_set<std::string> CodeBlockParser::supported_languages;

void CodeBlockParser::load_supported_languages() {
    const char* cmd = "highlight --list-scripts=langs | grep -F ':' | cut -d':' -f2 | grep -vE '^$' | cut -c2- | sed 's/[(|)]//g' | tr ' ' '\n' | grep -vE '^$'";

    std::array<char, 128> buffer;
    UniqueFILE pipe(popen(cmd, "r"));

    if (!pipe) {
        Logger::warn("Failed to load supported languages from highlight");
        return;
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        std::string lang(buffer.data());
        lang.erase(std::remove_if(lang.begin(), lang.end(),
            [](char c) { return c == '\n' || c == '\r'; }), lang.end());

        if (!lang.empty()) {
            std::transform(lang.begin(), lang.end(), lang.begin(), ::tolower);
            supported_languages.insert(lang);
        }
    }

    Logger::info("Loaded %zu supported language abbreviations from highlight", supported_languages.size());
}

bool CodeBlockParser::is_language_supported(const std::string& lang) {
    if (lang.empty()) return false;
    std::string lower_lang = lang;
    std::transform(lower_lang.begin(), lower_lang.end(), lower_lang.begin(), ::tolower);
    return supported_languages.find(lower_lang) != supported_languages.end();
}

bool CodeBlockParser::is_valid_fence_language_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '+';
}

std::string CodeBlockParser::extract_language_from_fence(const std::string& line_after_fence) {
    size_t start = 0;
    while (start < line_after_fence.size() &&
           std::isspace(static_cast<unsigned char>(line_after_fence[start]))) {
        ++start;
    }

    size_t end = start;
    while (end < line_after_fence.size() &&
           is_valid_fence_language_char(line_after_fence[end])) {
        ++end;
    }

    if (end > start) {
        return line_after_fence.substr(start, end - start);
    }
    return "";
}

bool CodeBlockParser::is_partial_fence(const std::string& str) {
    if (str.empty()) return false;

    size_t backtick_count = 0;
    size_t i = str.size();
    while (i > 0 && str[i-1] == '`') {
        backtick_count++;
        i--;
    }

    if (backtick_count == 0 || backtick_count >= 16) return false;

    for (size_t j = 0; j < i; ++j) {
        if (!std::isspace(static_cast<unsigned char>(str[j]))) {
            return false;
        }
    }

    return true;
}

static std::string debug_summary(const std::string& s, size_t max_len = 100) {
    if (s.empty()) return "(empty)";
    std::string ret;
    size_t limit = std::min(s.size(), max_len);
    for (size_t i = 0; i < limit; ++i) {
        char c = s[i];
        if (c == '\n') ret += "\\n";
        else if (c == '\r') ret += "\\r";
        else if (c == '\t') ret += "\\t";
        else if (c == '`') ret += "`";
        else if (c >= 32 && c < 127) ret += c;
        else ret += "?";
    }
    if (s.size() > max_len) ret += "...";
    ret += " [len=" + std::to_string(s.size()) + "]";
    return ret;
}

CodeBlockParser::ParseResult CodeBlockParser::parse_next(
    const std::string& data,
    size_t pos,
    State& current_state,
    bool is_final)
{
    ParseResult result;

    if (pos >= data.size()) {
        Logger::debug("[CB] parse_next at end, state=%d, at_line_start=%d",
                     current_state.type, current_state.at_line_start);
        return result;
    }

    Logger::debug("[CB] parse: pos=%zu, state=%d, fence_indent=%zu, at_line_start=%d, data=%s",
                 pos, current_state.type, current_state.fence_indent,
                 current_state.at_line_start, debug_summary(data.substr(pos, 20)).c_str());

    auto set_result = [&](ParseResult::Type t, size_t adv, const std::string& cont = "") {
        result.type = t;
        result.advance_by = adv;
        result.content = cont;
        if (t == ParseResult::TEXT_CHUNK) {
            current_state.at_line_start = !cont.empty() && (cont.back() == '\n' || cont.back() == '\r');
            Logger::debug("[CB] -> TEXT_CHUNK, set at_line_start=%d", current_state.at_line_start);
        } else if (t == ParseResult::OPEN_FENCE_COMPLETE || t == ParseResult::CLOSE_FENCE) {
            current_state.at_line_start = true;
            Logger::debug("[CB] -> FENCE, set at_line_start=1");
        }
    };

    if (current_state.type == State::IN_BLOCK) {
        if (current_state.at_line_start) {
            size_t check_pos = pos;
            size_t indent = 0;

            while (check_pos < data.size() &&
                   data[check_pos] == ' ' &&
                   indent <= 16) {
                check_pos++;
                indent++;
            }

            Logger::debug("[CB] Close fence check: found_indent=%zu, expected=%zu",
                         indent, current_state.fence_indent);

            if (check_pos + 3 <= data.size() &&
                std::strncmp(&data[check_pos], "```", 3) == 0) {

                size_t after_fence = check_pos + 3;
                
                while (after_fence < data.size() &&
                       (data[after_fence] == ' ' || data[after_fence] == '\t')) {
                    after_fence++;
                }

                size_t lang_start = after_fence;
                size_t lang_end = after_fence;
                while (lang_end < data.size() &&
                       is_valid_fence_language_char(data[lang_end])) {
                    lang_end++;
                }
                
                std::string potential_lang = "";
                if (lang_end > lang_start) {
                    potential_lang = data.substr(lang_start, lang_end - lang_start);
                }

                size_t after_lang = lang_end;
                while (after_lang < data.size() &&
                       (data[after_lang] == ' ' || data[after_lang] == '\t')) {
                    after_lang++;
                }

                bool is_newline_or_end = (after_lang >= data.size() ||
                                         data[after_lang] == '\n' ||
                                         data[after_lang] == '\r');

                Logger::debug("[CB] Fence analysis: lang='%s', supported=%d, is_newline=%d",
                             potential_lang.c_str(), 
                             is_language_supported(potential_lang),
                             is_newline_or_end);

                if (!potential_lang.empty() && 
                    is_language_supported(potential_lang) &&
                    is_newline_or_end) {
                    
                    Logger::debug("[CB] NESTED codeblock detected: lang='%s'", potential_lang.c_str());
                    
                    size_t newline_pos = data.find('\n', check_pos);
                    if (newline_pos == std::string::npos && !is_final) {
                        Logger::debug("[CB] Incomplete nested fence, NEED_MORE_DATA");
                        result.type = ParseResult::NEED_MORE_DATA;
                        return result;
                    }
                    
                    size_t advance = (newline_pos != std::string::npos) ?
                                    (newline_pos - pos + 1) : (data.size() - pos);
                    
                    current_state.type = State::NONE;
                    current_state.fence_indent = 0;
                    
                    result.type = ParseResult::CLOSE_FENCE;
                    result.advance_by = advance;
                    result.language = potential_lang;
                    Logger::debug("[CB] NESTED: closing current, new lang='%s'", potential_lang.c_str());
                    return result;
                }
                else if (indent == current_state.fence_indent && is_newline_or_end) {
                    size_t newline_pos = data.find('\n', check_pos);
                    if (newline_pos == std::string::npos && !is_final) {
                        Logger::debug("[CB] Incomplete close fence, NEED_MORE_DATA");
                        result.type = ParseResult::NEED_MORE_DATA;
                        return result;
                    }
                    
                    size_t advance = (newline_pos != std::string::npos) ?
                                    (newline_pos - pos + 1) : (data.size() - pos);
                    current_state.type = State::NONE;
                    current_state.fence_indent = 0;
                    set_result(ParseResult::CLOSE_FENCE, advance);
                    Logger::debug("[CB] CLOSE_FENCE accepted, advance=%zu", advance);
                    return result;
                }
                else {
                    Logger::debug("[CB] Not a valid fence, treating as literal text");
                }
            }
        }

        size_t newline_pos = data.find('\n', pos);

        if (newline_pos == std::string::npos) {
            if (!is_final) {
                Logger::debug("[CB] No newline, NEED_MORE_DATA");
                result.type = ParseResult::NEED_MORE_DATA;
                return result;
            }
            std::string remaining = data.substr(pos);
            set_result(ParseResult::TEXT_CHUNK, remaining.size(), remaining);
            return result;
        }

        std::string line = data.substr(pos, newline_pos - pos + 1);
        set_result(ParseResult::TEXT_CHUNK, line.size(), line);
        return result;
    }

    if (current_state.type == State::NONE) {
        if (current_state.at_line_start) {
            size_t check_pos = pos;
            size_t indent = 0;

            while (check_pos < data.size() && data[check_pos] == ' ') {
                check_pos++;
                indent++;
                if (indent > 16) break;
            }

            Logger::debug("[CB] Open fence check: indent=%zu", indent);

            if (indent <= 16 &&
                check_pos + 3 <= data.size() &&
                std::strncmp(&data[check_pos], "```", 3) == 0) {

                size_t newline_pos = data.find('\n', check_pos);
                if (newline_pos == std::string::npos) {
                    if (!is_final) {
                        Logger::debug("[CB] Incomplete fence line, NEED_MORE_DATA");
                        result.type = ParseResult::NEED_MORE_DATA;
                        return result;
                    }
                    newline_pos = data.size();
                }

                std::string after_fence = data.substr(check_pos + 3, newline_pos - (check_pos + 3));
                std::string lang = extract_language_from_fence(after_fence);

                Logger::debug("[CB] Found fence, lang='%s', supported=%d",
                             lang.c_str(), is_language_supported(lang));

                if (is_language_supported(lang)) {
                    size_t lang_end = after_fence.find_first_not_of(" \t");
                    if (lang_end != std::string::npos) {
                        size_t after_lang = after_fence.find_first_of(" \t", lang_end);
                        if (after_lang == std::string::npos ||
                            after_fence.find_first_not_of(" \t", after_lang) == std::string::npos) {
                            size_t advance = newline_pos - pos + 1;
                            current_state.type = State::IN_BLOCK;
                            current_state.fence_indent = indent;
                            result.language = lang;
                            set_result(ParseResult::OPEN_FENCE_COMPLETE, advance);
                            Logger::debug("[CB] OPEN_FENCE accepted, indent=%zu", indent);
                            return result;
                        }
                    } else {
                        size_t advance = newline_pos - pos + 1;
                        current_state.type = State::IN_BLOCK;
                        current_state.fence_indent = indent;
                        result.language = "";
                        set_result(ParseResult::OPEN_FENCE_COMPLETE, advance);
                        Logger::debug("[CB] OPEN_FENCE (no lang), indent=%zu", indent);
                        return result;
                    }
                }
            }
        }

        size_t next_fence = data.find("```", pos);
        size_t newline_pos = data.find('\n', pos);

        size_t consume_end = std::min(
            next_fence != std::string::npos ? next_fence : data.size(),
            newline_pos != std::string::npos ? newline_pos + 1 : data.size()
        );

        if (next_fence != std::string::npos && next_fence < consume_end) {
            if (next_fence > pos) {
                set_result(ParseResult::TEXT_CHUNK, next_fence - pos,
                          data.substr(pos, next_fence - pos));
                return result;
            } else {
                if (!is_final && data.size() - pos < 3) {
                    result.type = ParseResult::NEED_MORE_DATA;
                    return result;
                }
                set_result(ParseResult::TEXT_CHUNK, 3, "```");
                return result;
            }
        }

        std::string remaining = data.substr(pos);
        if (!is_final && is_partial_fence(remaining)) {
            result.type = ParseResult::NEED_MORE_DATA;
            return result;
        }

        if (newline_pos != std::string::npos) {
            std::string line = data.substr(pos, newline_pos - pos + 1);
            set_result(ParseResult::TEXT_CHUNK, line.size(), line);
            return result;
        } else {
            size_t chunk_size = data.size() - pos;
            if (chunk_size == 0) {
                result.type = ParseResult::NEED_MORE_DATA;
                return result;
            }
            set_result(ParseResult::TEXT_CHUNK, chunk_size,
                      data.substr(pos, chunk_size));
            return result;
        }
    }

    return result;
}
