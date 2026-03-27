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
#include <fstream>
#include <unordered_map>
#include <filesystem>

std::unordered_set<std::string> CodeBlockParser::supported_languages;
std::unordered_map<std::string, std::string> CodeBlockParser::extension_to_lang;

static bool g_filetypes_loaded = false;

// Helper to convert string to lowercase
static std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), 
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// Extract quoted string from position, returns the string and advances pos
static std::string extract_quoted_string(const std::string& str, size_t& pos) {
    if (pos >= str.size()) return "";
    
    // Skip whitespace
    while (pos < str.size() && std::isspace(str[pos])) pos++;
    
    if (pos >= str.size() || (str[pos] != '"' && str[pos] != '\'')) return "";
    
    char quote = str[pos];
    pos++; // Skip opening quote
    
    size_t start = pos;
    while (pos < str.size() && str[pos] != quote) {
        pos++;
    }
    
    std::string result = str.substr(start, pos - start);
    if (pos < str.size()) pos++; // Skip closing quote
    return result;
}

// Parse extensions array: {"ext1", "ext2", "ext3"}
static std::vector<std::string> parse_extensions_array(const std::string& str) {
    std::vector<std::string> result;
    
    size_t pos = str.find('{');
    if (pos == std::string::npos) return result;
    pos++;
    
    while (pos < str.size()) {
        std::string ext = extract_quoted_string(str, pos);
        if (ext.empty()) break;
        result.push_back(ext);
        
        // Skip comma and whitespace
        while (pos < str.size() && (std::isspace(str[pos]) || str[pos] == ',')) pos++;
    }
    
    return result;
}

// Parse a single line like: { Lang="cpp", Extensions={"c++", "cpp", "cxx"} },
static bool parse_entry_line(const std::string& line, 
                             std::string& out_lang,
                             std::vector<std::string>& out_extensions) {
    // Find Lang=
    size_t lang_pos = line.find("Lang=");
    if (lang_pos == std::string::npos) return false;
    
    lang_pos += 5;
    out_lang = extract_quoted_string(line, lang_pos);
    
    if (out_lang.empty()) {
        Logger::debug("[CB] parse_entry_line: Failed to extract Lang from: %s", line.c_str());
        return false;
    }
    
    // Find Extensions=
    size_t ext_pos = line.find("Extensions=");
    if (ext_pos == std::string::npos) {
        Logger::debug("[CB] parse_entry_line: No Extensions= found for Lang=%s", out_lang.c_str());
        return false;
    }
    
    ext_pos += 11;
    out_extensions = parse_extensions_array(line.substr(ext_pos));
    
    if (out_extensions.empty()) {
        Logger::debug("[CB] parse_entry_line: No extensions parsed for Lang=%s", out_lang.c_str());
        return false;
    }
    
    Logger::debug("[CB] parse_entry_line: Lang=%s, Extensions=%zu", out_lang.c_str(), out_extensions.size());
    return true;
}

// Parse filetypes.conf file line by line
static void parse_filetypes_conf(const std::string& filepath) {
    Logger::debug("[CB] Parsing filetypes.conf: %s", filepath.c_str());
    
    std::ifstream file(filepath);
    if (!file.is_open()) {
        Logger::warn("[CB] Cannot open filetypes.conf: %s", filepath.c_str());
        return;
    }
    
    std::string line;
    int entry_count = 0;
    bool in_filemapping = false;
    
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        
        if (line[start] == '-' && line[start + 1] == '-') continue;
        
        // Check for FileMapping start
        if (line.find("FileMapping") != std::string::npos && 
            line.find('{') != std::string::npos) {
            in_filemapping = true;
            Logger::debug("[CB] Found FileMapping section");
            continue;
        }
        
        if (!in_filemapping) continue;
        
        // Check for end of FileMapping
        if (line.find('}') != std::string::npos && 
            line.find("Lang=") == std::string::npos) {
            in_filemapping = false;
            break;
        }
        
        // Parse entry line
        if (line.find("Lang=") != std::string::npos) {
            std::string lang;
            std::vector<std::string> extensions;
            
            if (parse_entry_line(line, lang, extensions)) {
                std::string lower_lang = to_lower(lang);
                CodeBlockParser::supported_languages.insert(lower_lang);
                
                for (const auto& ext : extensions) {
                    std::string lower_ext = to_lower(ext);
                    // First association wins (matches highlight CLI behavior)
                    if (CodeBlockParser::extension_to_lang.find(lower_ext) == CodeBlockParser::extension_to_lang.end()) {
                        CodeBlockParser::extension_to_lang[lower_ext] = lower_lang;
                        Logger::debug("[CB] Mapped extension '%s' -> '%s'", 
                                     lower_ext.c_str(), lower_lang.c_str());
                    }
                }
                entry_count++;
            }
        }
    }
    
    Logger::info("[CB] Parsed %d filetype entries from %s", entry_count, filepath.c_str());
    Logger::info("[CB] Loaded %zu extension-to-language mappings", CodeBlockParser::extension_to_lang.size());
}

void CodeBlockParser::load_filetype_mappings() {
    if (g_filetypes_loaded) {
        Logger::debug("[CB] Filetype mappings already loaded");
        return;
    }
    
    // Search paths for filetypes.conf
    std::vector<std::string> search_paths = {
        "/etc/highlight/filetypes.conf",
        "/usr/share/highlight/filetypes.conf",
        "/usr/local/share/highlight/filetypes.conf"
    };
    
    // Add user config path
    const char* home = std::getenv("HOME");
    if (home) {
        search_paths.push_back(std::string(home) + "/.highlight/filetypes.conf");
    }
    
    // Find first existing file
    std::string conf_path;
    for (const auto& path : search_paths) {
        if (std::filesystem::exists(path)) {
            conf_path = path;
            Logger::debug("[CB] Found filetypes.conf at: %s", path.c_str());
            break;
        }
    }
    
    if (conf_path.empty()) {
        Logger::warn("[CB] No filetypes.conf found in search paths");
        g_filetypes_loaded = true;
        return;
    }
    
    parse_filetypes_conf(conf_path);
    g_filetypes_loaded = true;
}

void CodeBlockParser::load_supported_languages() {
    load_filetype_mappings();
    
    // Also load from highlight binary if available (for svg etc.)
    //if (supported_languages.empty()) {
        const char* cmd = "highlight --list-scripts=langs 2>/dev/null | grep -F ':' | cut -d':' -f2 | grep -vE '^$' | cut -c2- | sed 's/[(|)]//g' | tr ' ' '\\n' | grep -vE '^$'";

        std::array<char, 128> buffer;
        UniqueFILE pipe(popen(cmd, "r"));

        if (!pipe) {
            Logger::warn("[CB] Failed to load supported languages from highlight");
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

        Logger::info("[CB] Loaded %zu supported language abbreviations from highlight", 
                    supported_languages.size());
    //}
}

std::string CodeBlockParser::resolve_language_from_extension(const std::string& ext) {
    if (ext.empty()) return "";
    
    std::string lower_ext = to_lower(ext);
    
    auto it = extension_to_lang.find(lower_ext);
    if (it != extension_to_lang.end()) {
        return it->second;
    }
    
    return "";
}

bool CodeBlockParser::is_language_supported(const std::string& lang) {
    if (lang.empty()) return false;
    
    std::string lower_lang = to_lower(lang);
    
    if (supported_languages.find(lower_lang) != supported_languages.end()) {
        return true;
    }
    
    if (!extension_to_lang.empty()) {
        auto it = extension_to_lang.find(lower_lang);
        if (it != extension_to_lang.end()) {
            return true;
        }
    }
    
    return false;
}

bool CodeBlockParser::is_valid_fence_language_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '+' || c == '.';
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
        std::string lang = line_after_fence.substr(start, end - start);
        
        std::string resolved = resolve_language_from_extension(lang);
        if (!resolved.empty()) {
            Logger::debug("[CB] Resolved extension '%s' to language '%s'", 
                         lang.c_str(), resolved.c_str());
            return resolved;
        }
        
        return lang;
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
        Logger::debug("[CB] parse_next at end, state=%d, at_line_start=%d, pending_indent=%zu",
                     current_state.type, current_state.at_line_start, current_state.pending_indent);
        return result;
    }

    Logger::debug("[CB] parse: pos=%zu, state=%d, fence_indent=%zu, at_line_start=%d, pending_indent=%zu, data=%s",
                 pos, current_state.type, current_state.fence_indent,
                 current_state.at_line_start, current_state.pending_indent,
                 debug_summary(data.substr(pos, 20)).c_str());

    // Track pending_indent across chunks for whitespace-only content
    auto set_result = [&](ParseResult::Type t, size_t adv, const std::string& cont = "") {
        result.type = t;
        result.advance_by = adv;
        result.content = cont;
        if (t == ParseResult::TEXT_CHUNK) {
            bool ends_with_newline = !cont.empty() && (cont.back() == '\n' || cont.back() == '\r');
            bool has_non_whitespace = !cont.empty() && cont.find_first_not_of(" \t") != std::string::npos;

            if (ends_with_newline) {
                current_state.at_line_start = true;
                current_state.pending_indent = 0;
                Logger::debug("[CB] -> TEXT_CHUNK (newline), at_line_start=1, pending_indent=0");
            } else if (has_non_whitespace) {
                current_state.at_line_start = false;
                current_state.pending_indent = 0;
                Logger::debug("[CB] -> TEXT_CHUNK (non-ws), at_line_start=0, pending_indent=0");
            } else {
                // Whitespace only - accumulate indentation
                current_state.pending_indent += cont.size();
                Logger::debug("[CB] -> TEXT_CHUNK (whitespace), pending_indent=%zu", current_state.pending_indent);
            }
        } else if (t == ParseResult::OPEN_FENCE_COMPLETE || t == ParseResult::CLOSE_FENCE) {
            current_state.at_line_start = true;
            current_state.pending_indent = 0;
            Logger::debug("[CB] -> FENCE, at_line_start=1, pending_indent=0");
        }
    };

    if (current_state.type == State::IN_BLOCK) {
        if (current_state.at_line_start) {
            size_t check_pos = pos;
            // Use accumulated pending_indent + count spaces in current chunk
            size_t indent = current_state.pending_indent;

            while (check_pos < data.size() && data[check_pos] == ' ' && indent <= 16) {
                check_pos++;
                indent++;
            }

            Logger::debug("[CB] Close fence check: pending=%zu, chunk=%zu, total=%zu, expected=%zu",
                         current_state.pending_indent, check_pos - pos, indent, current_state.fence_indent);

            if (check_pos + 3 <= data.size() && std::strncmp(&data[check_pos], "```", 3) == 0) {
                size_t after_fence = check_pos + 3;
                while (after_fence < data.size() && (data[after_fence] == ' ' || data[after_fence] == '\t')) {
                    after_fence++;
                }
                bool is_newline_or_end = (after_fence >= data.size() || data[after_fence] == '\n' || data[after_fence] == '\r');
								if (indent == current_state.fence_indent && is_newline_or_end) {
										size_t newline_pos = data.find('\n', check_pos);
										if (newline_pos == std::string::npos && !is_final) {
												Logger::debug("[CB] Incomplete close fence, NEED_MORE_DATA");
												result.type = ParseResult::NEED_MORE_DATA;
												return result;
										}
										size_t advance = (newline_pos != std::string::npos) ? (newline_pos - pos + 1) : (data.size() - pos);

										// Save fence_indent BEFORE resetting state
										result.type = ParseResult::CLOSE_FENCE;
										result.advance_by = advance;
										result.fence_indent = current_state.fence_indent;

										current_state.type = State::NONE;
										current_state.fence_indent = 0;

										set_result(ParseResult::CLOSE_FENCE, advance);
										Logger::debug("[CB] CLOSE_FENCE accepted, fence_indent=%zu", result.fence_indent);
										return result;
								}
                else {
                    Logger::debug("[CB] Not a valid fence (indent=%zu vs %zu), treating as literal", indent, current_state.fence_indent);
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
            // Use accumulated pending_indent + count spaces in current chunk
            size_t indent = current_state.pending_indent;

            while (check_pos < data.size() && data[check_pos] == ' ') {
                check_pos++;
                indent++;
                if (indent > 16) break;
            }

            Logger::debug("[CB] Open fence check: pending=%zu, chunk=%zu, total=%zu",
                         current_state.pending_indent, check_pos - pos, indent);

            if (indent <= 16 && check_pos + 3 <= data.size() && std::strncmp(&data[check_pos], "```", 3) == 0) {
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

                Logger::debug("[CB] Found fence, lang='%s', supported=%d, indent=%zu", lang.c_str(), is_language_supported(lang), indent);

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
