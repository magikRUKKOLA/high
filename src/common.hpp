#ifndef COMMON_HPP
#define COMMON_HPP

#include <string>
#include <cstddef>
#include <wchar.h>
#include <memory>
#include <cstdio>

/*
 * =============================================================================
 * UTF-8 Character Width Utilities
 * =============================================================================
 * 
 * get_char_display_width(): Get display width of a wchar_t character
 *   - Returns 0 for zero-width characters (variation selectors, joiners)
 *   - Returns 1 for ASCII and most characters
 *   - Returns 2 for wide characters (CJK, emoji)
 * 
 * get_utf8_char_width(): Get display width of UTF-8 string segment
 *   - Parses UTF-8 byte sequence
 *   - Returns bytes consumed and display width
 * 
 * Reference: docs/ANSI-escape-sequences.md - Unicode handling
 * =============================================================================
 */
int get_char_display_width(wchar_t wc);
int get_utf8_char_width(const char* str, size_t len, size_t& bytes_consumed);

/*
 * =============================================================================
 * Display Width Calculations
 * =============================================================================
 * 
 * calculate_display_width_no_ansi(): Width with ANSI codes stripped (0 width)
 *   - CRITICAL for ghost text calculation in preview mode
 *   - Fixes issue where \x1b[48;5;20m was counted as display width
 * 
 * calculate_display_width(): Width without ANSI stripping
 *   - Use when ANSI codes are not expected
 * 
 * calculate_line_count(): Number of lines at given terminal width
 *   - Used for ghost text line calculation
 *   - Skips ANSI sequences (0 width)
 * 
 * All functions assume tabs are already expanded to spaces.
 * =============================================================================
 */
size_t calculate_display_width_no_ansi(const std::string& text);
size_t calculate_display_width(const std::string& text, size_t max_width = 0);
size_t calculate_line_count(const std::string& text, size_t term_width);

/*
 * =============================================================================
 * Tab Expansion
 * =============================================================================
 * 
 * expand_tabs(): Replace tabs with configured number of spaces
 *   - Default tab_width: 8 (configurable via LLM_TAB_WIDTH or --tab-width)
 *   - Tab stops at: 0, tab_width, 2*tab_width, ...
 * 
 * Example: "hello\tworld" with tab_width=8 -> "hello   world"
 * =============================================================================
 */
std::string expand_tabs(const std::string& text, int tab_width = 0);

/*
 * =============================================================================
 * ANSI Code Utilities
 * =============================================================================
 * 
 * strip_leading_ansi_reset_newline(): Strip \x1b[0m\n from start of string
 *   - Cleans up output from syntax highlighter
 * 
 * ends_with_newline_ignore_ansi(): Check if ends with \n, ignoring trailing ANSI
 *   - Used to determine if newline needs to be added after highlighted output
 * 
 * contains_newline(): Check if string contains \n
 * 
 * Reference: docs/ANSI-escape-sequences.md - CSI sequences, SGR codes
 * =============================================================================
 */
std::string strip_leading_ansi_reset_newline(const std::string& s);
bool ends_with_newline_ignore_ansi(const std::string& s);
bool contains_newline(const std::string& s);

/*
 * =============================================================================
 * Debug Helper
 * =============================================================================
 * 
 * hex_encode_string(): Convert string to hex representation for debugging
 *   - Useful for inspecting ANSI sequences in logs
 *   - Example: "hello\x1b[0m" -> "hello\e[0m"
 * =============================================================================
 */
std::string hex_encode_string(const std::string& s, size_t max_len = 100);

/*
 * =============================================================================
 * Terminal Dimensions
 * =============================================================================
 * 
 * get_terminal_width(): Get terminal width in columns
 *   - Uses ioctl(TIOCGWINSZ) or COLUMNS env var
 * 
 * get_terminal_height(): Get terminal height in rows
 *   - Uses ioctl(TIOCGWINSZ) or LINES env var
 * 
 * is_terminal_output(): Check if stdout is a terminal
 *   - Uses isatty(STDOUT_FILENO)
 * =============================================================================
 */
size_t get_terminal_width();
size_t get_terminal_height();
bool is_terminal_output();

/*
 * =============================================================================
 * ANSI String Manipulation
 * =============================================================================
 * 
 * strip_ansi_codes(): Remove all ANSI escape sequences from text
 *   - Used for logging, debugging, plain text output
 * 
 * trim_to_width(): Trim text to max width, adding "..." if truncated
 *   - Preserves ANSI codes in output but doesn't count their width
 *   - Used for conversation list display
 * 
 * ends_with_ansi_reset(): Check if text ends with \x1b[0m
 * =============================================================================
 */
std::string strip_ansi_codes(const std::string& text);
std::string trim_to_width(const std::string& text, size_t max_width);
bool ends_with_ansi_reset(const std::string& text);

/*
 * =============================================================================
 * Terminal Control
 * =============================================================================
 * 
 * reset_terminal(): Reset all attributes and show cursor
 *   - Sends: \x1b[0m + \x1b[?25h
 * 
 * hide_cursor(): Hide cursor during output
 *   - Sends: \x1b[?25l
 * 
 * show_cursor(): Show cursor after output
 *   - Sends: \x1b[?25h
 * 
 * clear_screen(): Clear screen and move cursor to home
 *   - Sends: \x1b[H + \x1b[2J
 * 
 * Reference: docs/ANSI-escape-sequences.md - Cursor Controls, Screen Modes
 * =============================================================================
 */
void reset_terminal();
void hide_cursor();
void show_cursor();
void clear_screen();

/*
 * =============================================================================
 * Content Filtering
 * =============================================================================
 * 
 * remove_think_tags(): Strip
<think>

...
</think>



tags and content
 *   - Used with --remove-think-tags flag
 *   - Hides reasoning content from models like DeepSeek-R1
 * =============================================================================
 */
std::string remove_think_tags(const std::string& text);

/*
 * =============================================================================
 * FILE Deleter for popen
 * =============================================================================
 * 
 * Smart pointer deleter for FILE* returned by popen().
 * Automatically calls pclose() when UniqueFILE goes out of scope.
 * 
 * Example:
 *   UniqueFILE pipe(popen("cmd", "r"));
 *   // pclose() called automatically when pipe goes out of scope
 * =============================================================================
 */
struct FILEDeleter { 
    void operator()(FILE* f) const noexcept { 
        if (f) pclose(f); 
    } 
};
using UniqueFILE = std::unique_ptr<FILE, FILEDeleter>;

// Ghost spinner characters for loader animation
extern const char* GHOST_SPINNERS[];
extern const size_t GHOST_SPINNER_COUNT;

// Global loader instance management
class Loader;
void set_common_loader(Loader* loader);
Loader* get_common_loader();

#endif
