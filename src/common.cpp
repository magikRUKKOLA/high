#include "common.hpp"
#include "logger.hpp"
#include "config.hpp"
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <cwchar>
#include <clocale>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <algorithm>

/*
 * =============================================================================
 * ANSI Escape Sequence Reference
 * Based on: docs/ANSI-escape-sequences.md, ANSI X3.64-1977, ECMA-48
 * =============================================================================
 * 
 * ANSI escape sequences are used to control terminal formatting (colors,
 * cursor movement, etc.). They have ZERO display width - they only affect
 * rendering, not the actual character count.
 * 
 * This is CRITICAL for ghost text calculation in preview mode. If ANSI codes
 * are incorrectly counted as display width, we may think there are 2 lines
 * of ghost text when there's actually only 1 (especially with --chunk-size 1).
 * 
 * CSI (Control Sequence Introducer) Structure:
 *   ESC [ <params> <intermediates> <final>
 *   \x1b [ 0x30-0x3F   0x20-0x2F      0x40-0x7E
 *   1B   5B
 * 
 * Byte Ranges:
 *   0x30-0x3F : Parameter bytes (digits 0-9, semicolon ';', '<', '>', '=', '?')
 *   0x20-0x2F : Intermediate bytes (space, '!', '"', '#', '$', etc.)
 *   0x40-0x7E : Final byte (terminates the sequence - the command)
 * 
 * Common Final Bytes (commands):
 *   'm' (0x6D) - SGR (Select Graphic Rendition) - colors, bold, italic, etc.
 *   'H' (0x48) - CUP (Cursor Position)
 *   'f' (0x66) - HVP (Horizontal Vertical Position)
 *   'A' (0x41) - CUU (Cursor Up)
 *   'B' (0x42) - CUD (Cursor Down)
 *   'C' (0x43) - CUF (Cursor Forward/Right)
 *   'D' (0x44) - CUB (Cursor Backward/Left)
 *   'E' (0x45) - CNL (Cursor Next Line)
 *   'F' (0x46) - CPL (Cursor Previous Line)
 *   'G' (0x47) - CHA (Cursor Horizontal Absolute)
 *   'J' (0x4A) - ED (Erase in Display)
 *   'K' (0x4B) - EL (Erase in Line)
 *   'n' (0x6E) - DSR (Device Status Report)
 *   'c' (0x63) - DA (Device Attributes)
 *   'g' (0x67) - TBC (Tabulation Clear)
 *   'h' (0x68) - SM (Set Mode)
 *   'l' (0x6C) - RM (Reset Mode)
 *   'q' (0x71) - DECLL (Load LEDs)
 *   'r' (0x72) - DECSTBM (Set Top and Bottom Margins)
 *   's' (0x73) - Save cursor (SCO)
 *   'u' (0x75) - Restore cursor (SCO)
 * 
 * Common Examples:
 *   \x1b[0m           - Reset all attributes (SGR)
 *   \x1b[1;31m        - Bold red text (SGR)
 *   \x1b[38;5;20m     - 256-color foreground, color 20 (SGR)
 *   \x1b[48;5;20m     - 256-color background, color 20 (SGR)
 *   \x1b[38;2;R;G;Bm  - RGB foreground color (Truecolor/24-bit)
 *   \x1b[48;2;R;G;Bm  - RGB background color (Truecolor/24-bit)
 *   \x1b[?25l         - Hide cursor (private mode)
 *   \x1b[?25h         - Show cursor (private mode)
 *   \x1b[2K           - Erase entire line (EL)
 *   \x1b[1F           - Move to beginning of previous line (CPL)
 *   \x1b[s            - Save cursor position
 *   \x1b[u            - Restore cursor position
 *   \x1b[H            - Move cursor to home position (0,0)
 *   \x1b[2J           - Clear entire screen (ED)
 *   \x1b[?1049h       - Enable alternative buffer
 *   \x1b[?1049l       - Disable alternative buffer
 * 
 * Other Escape Sequence Types:
 *   OSC (Operating System Command): ESC ] ... BEL or ESC ] ... ESC \
 *   DCS (Device Control String): ESC P ... ESC \
 *   APC (Application Program Command): ESC _ ... ESC \
 *   PM  (Privacy Message): ESC ^ ... ESC \
 * 
 * Key Point: ALL escape sequences have ZERO display width.
 * =============================================================================
 */

static Loader* g_common_loader = nullptr;
static std::mutex g_loader_mutex;

void set_common_loader(Loader* loader) {
    std::lock_guard<std::mutex> lock(g_loader_mutex);
    g_common_loader = loader;
}

Loader* get_common_loader() {
    std::lock_guard<std::mutex> lock(g_loader_mutex);
    return g_common_loader;
}

const char* GHOST_SPINNERS[] = {"⠋", "⠙", "⠹", "⢸", "⣰", "⣤", "⣆"};
const size_t GHOST_SPINNER_COUNT = 7;

/*
 * =============================================================================
 * UTF-8 Character Display Width
 * =============================================================================
 * 
 * Determines how many terminal columns a character occupies:
 * - ASCII (0x00-0x7F): 1 column
 * - Most CJK characters: 2 columns (wide)
 * - Combining marks, zero-width joiners: 0 columns
 * - Emoji: typically 2 columns
 * 
 * Uses wcwidth() from libc as base, with manual overrides for emoji ranges
 * that wcwidth() may not handle correctly on all systems.
 * 
 * Unicode Ranges Handled:
 *   0x1F600-0x1F64F : Emoticons
 *   0x1F680-0x1F6FF : Transport and Map Symbols
 *   0x1F300-0x1F5FF : Miscellaneous Symbols and Pictographs
 *   0x2600-0x26FF   : Miscellaneous Symbols
 *   0x2700-0x27BF   : Dingbats
 *   0x1F900-0x1F9FF : Supplemental Symbols and Pictographs
 *   0x1FA00-0x1FA6F : Chess, Symbols and Pictographs Extended-A
 *   0xFE00-0xFE0F   : Variation Selectors (zero-width)
 *   0xE0100-0xE01EF : Variation Selectors Supplement (zero-width)
 *   0x200D          : Zero Width Joiner (zero-width)
 * =============================================================================
 */
int get_char_display_width(wchar_t wc) {
    int width = wcwidth(wc);
    if (width <= 0) {
        // Emoji and special characters that should be 2 columns
        if ((wc >= 0x1F600 && wc <= 0x1F64F) ||
            (wc >= 0x1F680 && wc <= 0x1F6FF) ||
            (wc >= 0x1F300 && wc <= 0x1F5FF) ||
            (wc >= 0x2600 && wc <= 0x26FF) ||
            (wc >= 0x2700 && wc <= 0x27BF) ||
            (wc >= 0x1F900 && wc <= 0x1F9FF) ||
            (wc >= 0x1FA00 && wc <= 0x1FA6F)) {
            return 2;
        }
        // Zero-width characters (variation selectors, joiners)
        if ((wc >= 0xFE00 && wc <= 0xFE0F) ||
            (wc >= 0xE0100 && wc <= 0xE01EF) ||
            wc == 0x200D) {
            return 0;
        }
        return 1;
    }
    return width;
}

/*
 * =============================================================================
 * UTF-8 Byte Sequence Parser
 * =============================================================================
 * 
 * Determines how many bytes a UTF-8 character uses and its display width.
 * 
 * UTF-8 Encoding:
 *   0x00-0x7F : 1 byte (ASCII)
 *   0xC0-0xDF : 2 bytes (110xxxxx 10xxxxxx)
 *   0xE0-0xEF : 3 bytes (1110xxxx 10xxxxxx 10xxxxxx)
 *   0xF0-0xF7 : 4 bytes (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
 * 
 * Uses mbrtowc() to convert UTF-8 to wchar_t, then gets display width.
 * =============================================================================
 */
int get_utf8_char_width(const char* str, size_t len, size_t& bytes_consumed) {
    if (!str || len == 0) {
        bytes_consumed = 0;
        return 0;
    }
    
    unsigned char c = static_cast<unsigned char>(str[0]);
    if (c < 0x80) {
        // ASCII character
        bytes_consumed = 1;
        return 1;
    }
    
    // Determine sequence length from first byte
    int seq_len = 1;
    if ((c & 0xE0) == 0xC0) seq_len = 2;
    else if ((c & 0xF0) == 0xE0) seq_len = 3;
    else if ((c & 0xF8) == 0xF0) seq_len = 4;
    else {
        // Invalid UTF-8 start byte
        bytes_consumed = 1;
        return 1;
    }
    
    if (static_cast<size_t>(seq_len) > len) {
        // Incomplete sequence
        bytes_consumed = len;
        return 1;
    }
    
    std::mbstate_t state = std::mbstate_t();
    wchar_t wc;
    size_t res = std::mbrtowc(&wc, str, seq_len, &state);
    
    if (res != static_cast<size_t>(-1) && res != static_cast<size_t>(-2)) {
        bytes_consumed = res;
        return get_char_display_width(wc);
    }
    
    // Conversion failed, treat as single byte
    bytes_consumed = 1;
    return 1;
}

/*
 * =============================================================================
 * Tab Expansion
 * =============================================================================
 * 
 * Replaces tab characters with spaces based on configured tab width.
 * Tab stops are at columns: 0, tab_width, 2*tab_width, 3*tab_width, ...
 * 
 * Example with tab_width=8:
 *   "hello\tworld" -> "hello   world" (3 spaces to reach column 8)
 *   "12345678\tX"  -> "12345678        X" (8 spaces to reach column 16)
 * 
 * Default tab_width: 8 (configurable via LLM_TAB_WIDTH env var or --tab-width)
 * =============================================================================
 */
std::string expand_tabs(const std::string& text, int tab_width) {
    if (tab_width <= 0) {
        tab_width = Config::instance().get_tab_width();
        if (tab_width <= 0) tab_width = 8;
    }
    
    // Fast path: no tabs
    if (text.find('\t') == std::string::npos) {
        return text;
    }
    
    std::string result;
    result.reserve(text.size() + std::count(text.begin(), text.end(), '\t') * (tab_width - 1));
    
    size_t col = 0;
    for (char c : text) {
        if (c == '\t') {
            // Calculate spaces needed to reach next tab stop
            size_t spaces = tab_width - (col % tab_width);
            result.append(spaces, ' ');
            col += spaces;
        } else {
            result += c;
            if (c == '\n' || c == '\r') {
                col = 0;  // Reset column on newline
            } else {
                col++;
            }
        }
    }
    return result;
}

/*
 * =============================================================================
 * Display Width Calculation (without ANSI stripping)
 * =============================================================================
 * 
 * Calculates display width of text, stopping at newline.
 * Does NOT strip ANSI codes - use calculate_display_width_no_ansi for that.
 * 
 * Used for: Basic width calculations where ANSI codes are not expected.
 * =============================================================================
 */
size_t calculate_display_width(const std::string& text, size_t max_width) {
    size_t total_width = 0;
    size_t i = 0;

    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        if (c == '\n' || c == '\r') {
            break;
        } else if (c < 0x80) {
            total_width++;
            i++;
        } else {
            size_t bytes = 0;
            int width = get_utf8_char_width(&text[i], text.size() - i, bytes);
            total_width += width;
            i += bytes;
        }

        if (max_width > 0 && total_width >= max_width) {
            break;
        }
    }

    return total_width;
}

/*
 * =============================================================================
 * ANSI Sequence Parser - Core Logic
 * =============================================================================
 * 
 * Parses and skips a single ANSI escape sequence.
 * Returns: bytes consumed if valid sequence found, 0 if not an ANSI sequence.
 * 
 * This is the KEY FIX for the ghost text issue with --chunk-size 1.
 * 
 * Previously, sequences like \x1b[48;5;20m (256-color background) were not
 * fully recognized, causing ANSI bytes to be counted as display width.
 * This led to incorrect line count calculations - thinking there were 2 lines
 * of ghost text when there was actually only 1.
 * 
 * Now properly handles:
 * - CSI sequences: ESC [ <params> <intermediates> <final>
 * - OSC sequences: ESC ] ... BEL or ESC ] ... ESC \
 * - DCS sequences: ESC P ... ESC \
 * - APC sequences: ESC _ ... ESC \
 * - 2-char sequences: ESC 7, ESC 8, ESC c, ESC D, ESC E, ESC M, etc.
 * - ESC # N sequences: ESC # 3, ESC # 4, ESC # 5, ESC # 6, ESC # 8
 * - Character set sequences: ESC ( A, ESC ) B, ESC ( 0, etc.
 * - Keypad modes: ESC =, ESC >
 * - ANSI mode: ESC <
 * 
 * Reference: docs/ANSI-escape-sequences.md
 * =============================================================================
 */
static size_t skip_ansi_sequence(const std::string& text, size_t pos) {
    if (pos >= text.size() || text[pos] != '\x1b') {
        return 0;
    }
    
    size_t i = pos + 1;
    if (i >= text.size()) {
        return 1;  // Incomplete sequence (just ESC)
    }
    
    // =========================================================================
    // CSI (Control Sequence Introducer): ESC [ or ESC ]
    // Structure: ESC [ <params> <intermediates> <final>
    //            1B   5B  0x30-0x3F  0x20-0x2F      0x40-0x7E
    // =========================================================================
    if (text[i] == '[' || text[i] == ']') {
        i++;  // Skip '[' or ']'
        
        // Skip parameter bytes (0x30-0x3F): digits, semicolons, <, >, =, ?
        // Example: \x1b[48;5;20m - the "48;5;20" part
        // Example: \x1b[?25l - the "?25" part (private mode)
        while (i < text.size() && 
               static_cast<unsigned char>(text[i]) >= 0x30 && 
               static_cast<unsigned char>(text[i]) <= 0x3F) {
            i++;
        }
        
        // Skip intermediate bytes (0x20-0x2F): space, !, ", #, $, %, &, etc.
        // Rarely used in practice but part of ANSI spec
        while (i < text.size() && 
               static_cast<unsigned char>(text[i]) >= 0x20 && 
               static_cast<unsigned char>(text[i]) <= 0x2F) {
            i++;
        }
        
        // Skip final byte (0x40-0x7E): the command character
        // Common: m (SGR), H (cursor position), A/B/C/D (cursor move), etc.
        if (i < text.size() && 
            static_cast<unsigned char>(text[i]) >= 0x40 && 
            static_cast<unsigned char>(text[i]) <= 0x7E) {
            i++;
        }
        
        return i - pos;
    }
    
    /*
    // =========================================================================
    // OSC (Operating System Command): ESC ]
    // Structure: ESC ] <data> BEL  or  ESC ] <data> ESC \
    // Used for: Terminal title, colors, notifications, etc.
    // =========================================================================
    */
    if (text[i] == ']') {
        i++;
        // OSC sequences end with BEL (\x07) or ESC \ (string terminator)
        while (i < text.size()) {
            if (text[i] == '\x07') {
                return i - pos + 1;
            }
            if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '\\') {
                return i - pos + 2;
            }
            i++;
        }
        return i - pos;
    }
    
    // =========================================================================
    // Other Escape Sequences (2-character or special formats)
    // =========================================================================
    if (i < text.size()) {
        // Single character after ESC (0x40-0x5F)
        // Examples: ESC 7 (save cursor DEC), ESC 8 (restore cursor DEC),
        //           ESC c (reset), ESC D (index), ESC E (next line),
        //           ESC M (reverse index), ESC = (keypad application),
        //           ESC > (keypad numeric), ESC < (enter ANSI mode)
        if (static_cast<unsigned char>(text[i]) >= 0x40 && 
            static_cast<unsigned char>(text[i]) <= 0x5F) {
            return 2;
        }
        
        // ESC # N sequences (DEC special graphics)
        // ESC # 3: Double-height line, top half
        // ESC # 4: Double-height line, bottom half
        // ESC # 5: Single-width single-height line
        // ESC # 6: Double-width single-height line
        // ESC # 8: Screen alignment display (fill with 'E')
        if (text[i] == '#' && i + 1 < text.size()) {
            return 3;
        }
        
        // ESC ( / ESC ) character set sequences (SCS - Select Character Set)
        // ESC ( A: UK character set (G0)
        // ESC ( B: ASCII character set (G0)
        // ESC ( 0: Special graphics (G0)
        // ESC ) A: UK character set (G1)
        // ESC ) B: ASCII character set (G1)
        // ESC ) 0: Special graphics (G1)
        if ((text[i] == '(' || text[i] == ')') && i + 1 < text.size()) {
            return 3;
        }
        
        // ESC = (keypad application mode) and ESC > (keypad numeric mode)
        if (text[i] == '=' || text[i] == '>') {
            return 2;
        }
        
        // ESC < (enter ANSI mode from VT52 mode)
        if (text[i] == '<') {
            return 2;
        }
        
        // ESC c (reset to initial state - RIS)
        if (text[i] == 'c') {
            return 2;
        }
        
        /*
        // ESC P (DCS - Device Control String)
        // Structure: ESC P <params> <data> ESC \
        // Used for: Download characters, soft fonts, etc.
        */
        if (text[i] == 'P') {
            i++;
            while (i < text.size()) {
                if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '\\') {
                    return i - pos + 2;
                }
                i++;
            }
            return i - pos;
        }
        
        /*
        // ESC _ (APC - Application Program Command)
        // Structure: ESC _ <data> ESC \
        // Used for: Application-specific commands
        */
        if (text[i] == '_') {
            i++;
            while (i < text.size()) {
                if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '\\') {
                    return i - pos + 2;
                }
                i++;
            }
            return i - pos;
        }
        
        // Default: 2-character sequence
        return 2;
    }
    
    return 1;
}

/*
 * =============================================================================
 * Display Width Calculation WITH ANSI Stripping
 * =============================================================================
 * 
 * Calculates display width of text, treating ANSI escape sequences as 0 width.
 * 
 * This is CRITICAL for ghost text in preview mode (--preview flag).
 * 
 * Problem (before fix):
 *   When streaming with --chunk-size 1, ANSI sequences like \x1b[48;5;20m
 *   could arrive split across chunks. The old code didn't properly recognize
 *   these sequences, so ANSI bytes were counted as display width.
 *   
 *   Example: "test\x1b[48;5;20m" at terminal width 10
 *   - Old: "test" (4) + "\x1b" (1) + "[" (1) + "4" (1) + "8" (1) + ";" (1) + "5" (1) + ";" (1) + "2" (1) + "0" (1) + "m" (1) = 15 cols -> 2 lines
 *   - New: "test" (4) + ANSI (0) = 4 cols -> 1 line
 * 
 * Solution:
 *   skip_ansi_sequence() now properly parses complete CSI sequences,
 *   including complex ones with multiple parameters like 256-color codes.
 * 
 * Examples:
 *   "hello"              = 5 columns
 *   "\x1b[31mhello\x1b[0m" = 5 columns (ANSI codes don't count)
 *   "\x1b[48;5;20mtest"   = 4 columns
 *   "\x1b[38;2;255;128;64mRGB" = 3 columns
 * =============================================================================
 */
size_t calculate_display_width_no_ansi(const std::string& text) {
    size_t width = 0;
    size_t i = 0;
    
    while (i < text.size()) {
        // Check for ANSI escape sequence
        if (text[i] == '\x1b') {
            size_t skip = skip_ansi_sequence(text, i);
            if (skip > 0) {
                i += skip;
                // ANSI codes have 0 display width
                continue;
            }
            // If skip_ansi_sequence returned 0, treat as regular character
        }
        
        if (text[i] == '\n' || text[i] == '\r') {
            break;
        } else {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 0x80) {
                width++;
                i++;
            } else {
                size_t bytes = 0;
                int char_width = get_utf8_char_width(&text[i], text.size() - i, bytes);
                if (char_width > 0) width += char_width;
                else width++;
                i += (bytes > 0) ? bytes : 1;
            }
        }
    }
    
    return width;
}

/*
 * =============================================================================
 * Line Count Calculation
 * =============================================================================
 * 
 * Calculates number of lines needed to display text at given terminal width.
 * 
 * ANSI sequences are skipped (0 width) but preserved in output.
 * This is used for ghost text line calculation in preview mode.
 * 
 * Example: If text is "\x1b[31mhello world\x1b[0m" and term_width=5:
 *   - "hello" = 5 cols (line 1)
 *   - " world" = 6 cols -> wraps to line 2
 *   - Result: 2 lines
 * 
 * Critical for: Determining how many lines of ghost text to clear when
 * switching from ghost text to highlighted output.
 * =============================================================================
 */
size_t calculate_line_count(const std::string& text, size_t term_width) {
    if (text.empty() || term_width == 0) return 1;
    
    size_t line_count = 1;
    size_t current_width = 0;
    size_t i = 0;
    
    while (i < text.size()) {
        // Skip ANSI sequences (they have 0 width)
        if (text[i] == '\x1b') {
            size_t skip = skip_ansi_sequence(text, i);
            if (skip > 0) {
                i += skip;
                continue;
            }
        }
        
        if (text[i] == '\n') {
            line_count++;
            current_width = 0;
            i++;
            continue;
        }
        
        int char_width = 1;
        size_t bytes_consumed = 1;
        unsigned char c = static_cast<unsigned char>(text[i]);
        
        if (c < 0x80) {
            char_width = 1;
        } else {
            char_width = get_utf8_char_width(&text[i], text.size() - i, bytes_consumed);
            if (char_width <= 0) char_width = 1;
            if (bytes_consumed == 0) bytes_consumed = 1;
        }
        
        if (current_width + char_width > term_width) {
            line_count++;
            current_width = char_width;
        } else {
            current_width += char_width;
        }
        
        i += bytes_consumed;
    }
    
    return line_count;
}

size_t get_terminal_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return w.ws_col;
    }
    const char* cols = std::getenv("COLUMNS");
    return cols ? std::max(atoi(cols), 80) : 80;
}

size_t get_terminal_height() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
        return w.ws_row;
    }
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
        return w.ws_row;
    }
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
        return w.ws_row;
    }
    const char* rows = std::getenv("LINES");
    if (rows) {
        int r = atoi(rows);
        if (r > 0) return r;
    }
    return 24;
}

bool is_terminal_output() {
    return isatty(STDOUT_FILENO) != 0;
}

/*
 * =============================================================================
 * Strip ANSI Codes
 * =============================================================================
 * 
 * Removes all ANSI escape sequences from text, leaving only visible content.
 * 
 * Example:
 *   Input:  "\x1b[31mhello\x1b[0m world"
 *   Output: "hello world"
 * 
 * Uses the same ANSI parsing logic as calculate_display_width_no_ansi.
 * 
 * Used for: Logging, debugging, plain text output modes.
 * =============================================================================
 */
std::string strip_ansi_codes(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    
    for (size_t i = 0; i < text.size(); ) {
        if (text[i] == '\x1b') {
            size_t skip = skip_ansi_sequence(text, i);
            if (skip > 0) {
                i += skip;
                continue;
            }
        }
        result += text[i];
        i++;
    }
    
    return result;
}

/*
 * =============================================================================
 * Trim to Width
 * =============================================================================
 * 
 * Trims text to max display width, adding "..." if truncated.
 * 
 * ANSI sequences are NOT stripped - they're preserved in output but not
 * counted toward width. This ensures colored text can be trimmed correctly.
 * 
 * Example:
 *   Input:  "\x1b[31mhello world\x1b[0m", max_width=8
 *   Output: "\x1b[31mhello...\x1b[0m" (preserves color codes)
 * 
 * Used for: Conversation list display, truncating long titles.
 * =============================================================================
 */
std::string trim_to_width(const std::string& text, size_t max_width) {
    if (text.empty() || max_width == 0) return "";
    if (max_width < 3) return std::string(max_width, '.');
    
    std::string result;
    result.reserve(text.size());
    size_t display_width = 0;
    size_t i = 0;
    
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        int char_width = 1;
        size_t bytes_consumed = 1;
        
        if (c < 0x80) {
            char_width = 1;
            bytes_consumed = 1;
        } else {
            char_width = get_utf8_char_width(&text[i], text.size() - i, bytes_consumed);
            if (char_width <= 0) char_width = 1;
            if (bytes_consumed == 0) bytes_consumed = 1;
        }
        
        if (display_width + char_width > max_width - 3) {
            result += "...";
            break;
        }
        
        result.append(text, i, bytes_consumed);
        display_width += char_width;
        i += bytes_consumed;
    }
    
    return result;
}

bool ends_with_ansi_reset(const std::string& text) {
    if (text.empty()) return false;
    const char* reset = "\033[0m";
    size_t reset_len = 4;
    if (text.size() >= reset_len) {
        return text.compare(text.size() - reset_len, reset_len, reset) == 0;
    }
    return false;
}

/*
 * =============================================================================
 * Strip Leading ANSI Reset + Newline
 * =============================================================================
 * 
 * Strips leading ANSI reset code (\x1b[0m) followed by newline from string.
 * 
 * Example:
 *   Input:  "\x1b[0m\nhello"
 *   Output: "hello"
 * 
 * Used to clean up output from syntax highlighter (highlight binary),
 * which often prepends reset codes before output.
 * 
 * Reference: docs/ANSI-escape-sequences.md - SGR (Select Graphic Rendition)
 * =============================================================================
 */
std::string strip_leading_ansi_reset_newline(const std::string& s) {
    if (s.empty()) return s;
    size_t pos = 0;

    while (pos < s.size()) {
        if (s[pos] == '\x1b' && pos + 2 < s.size() && s[pos + 1] == '[') {
            size_t code_start = pos + 2;
            size_t code_end = code_start;
            // Skip parameter bytes (0x30-0x3F)
            while (code_end < s.size() &&
                   static_cast<unsigned char>(s[code_end]) >= 0x30 &&
                   static_cast<unsigned char>(s[code_end]) <= 0x3F) {
                code_end++;
            }
            // Skip intermediate bytes (0x20-0x2F)
            while (code_end < s.size() &&
                   static_cast<unsigned char>(s[code_end]) >= 0x20 &&
                   static_cast<unsigned char>(s[code_end]) <= 0x2F) {
                code_end++;
            }
            // Check for final byte (0x40-0x7E)
            if (code_end < s.size() && 
                static_cast<unsigned char>(s[code_end]) >= 0x40 &&
                static_cast<unsigned char>(s[code_end]) <= 0x7E) {
                size_t after_ansi = code_end + 1;
                while (after_ansi < s.size() && (s[after_ansi] == ' ' || s[after_ansi] == '\t')) {
                    after_ansi++;
                }
                if (after_ansi < s.size() && (s[after_ansi] == '\n' || s[after_ansi] == '\r')) {
                    size_t newline_pos = after_ansi;
                    pos = newline_pos + 1;
                    if (s[newline_pos] == '\r' && pos < s.size() && s[pos] == '\n') {
                        pos++;
                    }
                    continue;
                } else {
                    break;
                }
            } else {
                break;
            }
        } else if (s[pos] == '\n' || s[pos] == '\r' || s[pos] == ' ' || s[pos] == '\t') {
            pos++;
        } else {
            break;
        }
    }

    return s.substr(pos);
}

/*
 * =============================================================================
 * Check if String Ends with Newline (Ignoring ANSI)
 * =============================================================================
 * 
 * Checks if string ends with newline, ignoring trailing ANSI codes.
 * 
 * Example:
 *   "hello\n\x1b[0m" returns true (newline before ANSI reset)
 *   "hello\x1b[0m" returns false
 *   "hello" returns false
 * 
 * Used to determine if we need to add a newline after highlighted output.
 * 
 * Implementation: Works backwards through string, skipping ANSI sequences
 * until finding a non-ANSI character or newline.
 * =============================================================================
 */
bool ends_with_newline_ignore_ansi(const std::string& s) {
    if (s.empty()) return false;
    size_t i = s.length();
    while (i > 0) {
        char c = s[i - 1];
        if (c == '\n') return true;
        if (c == '\x1b') {
            // Try to skip back over an ANSI sequence
            size_t seq_start = i - 1;
            size_t check = 0;
            if (seq_start > 0) {
                // Look for the start of this sequence
                check = seq_start;
                while (check > 0) {
                    check--;
                    if (s[check] == '\x1b') {
                        // Found start, skip this whole sequence
                        size_t skip = skip_ansi_sequence(s, check);
                        if (skip > 0 && check + skip == i) {
                            i = check;
                            break;
                        }
                    }
                }
            }
            if (check == 0 && s[0] == '\x1b') {
                // Reached start of string with ANSI
                i = 0;
                break;
            }
            continue;
        }
        if (c == 'm') {
            // Could be end of SGR sequence, try to skip back
            size_t seq_start = i - 1;
            while (seq_start > 0) {
                char ic = s[seq_start - 1];
                if (static_cast<unsigned char>(ic) >= 0x20 && static_cast<unsigned char>(ic) <= 0x2F) seq_start--;
                else break;
            }
            while (seq_start > 0) {
                char pc = s[seq_start - 1];
                if (static_cast<unsigned char>(pc) >= 0x30 && static_cast<unsigned char>(pc) <= 0x3F) seq_start--;
                else break;
            }
            if (seq_start > 0 && s[seq_start - 1] == '[' && seq_start > 1 && s[seq_start - 2] == '\x1b') {
                i = seq_start - 2;
                continue;
            }
        }
        break;
    }
    return false;
}

// Check if string contains newline
bool contains_newline(const std::string& s) {
    return s.find('\n') != std::string::npos;
}

/*
 * =============================================================================
 * Hex Encode String (Debug Helper)
 * =============================================================================
 * 
 * Converts string to hex representation for debugging.
 * Useful for inspecting ANSI sequences in logs.
 * 
 * Example:
 *   Input:  "hello\x1b[0m"
 *   Output: "hello\e[0m"
 * 
 * Special escapes: \n, \r, \t, \e (ESC), printable ASCII, \xHH for others.
 * =============================================================================
 */
std::string hex_encode_string(const std::string& s, size_t max_len) {
    std::ostringstream oss;
    size_t limit = std::min(s.size(), max_len);
    for (size_t i = 0; i < limit; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n') oss << "\\n";
        else if (c == '\r') oss << "\\r";
        else if (c == '\t') oss << "\\t";
        else if (c == '\x1b') oss << "\\e";
        else if (c >= 0x20 && c < 0x7F) oss << static_cast<char>(c);
        else oss << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    if (s.size() > max_len) oss << "...";
    return oss.str();
}

/*
 * =============================================================================
 * Terminal Control Functions
 * =============================================================================
 * 
 * reset_terminal(): Reset all attributes and show cursor
 *   Sends: \x1b[0m (SGR reset) + \x1b[?25h (show cursor)
 * 
 * hide_cursor(): Hide cursor during output
 *   Sends: \x1b[?25l (private mode - hide cursor)
 * 
 * show_cursor(): Show cursor after output
 *   Sends: \x1b[?25h (private mode - show cursor)
 * 
 * clear_screen(): Clear entire screen and move cursor to home
 *   Sends: \x1b[H (cursor home) + \x1b[2J (erase display)
 * 
 * Reference: docs/ANSI-escape-sequences.md - Cursor Controls, Colors, Screen Modes
 * =============================================================================
 */
void reset_terminal() {
    if (!isatty(STDOUT_FILENO)) return;
    const char* reset = "\033[0m\033[?25h";
    write(STDOUT_FILENO, reset, strlen(reset));
    fsync(STDOUT_FILENO);
}

void hide_cursor() {
    if (isatty(STDOUT_FILENO)) {
        std::cout << "\033[?25l" << std::flush;
    }
}

void show_cursor() {
    if (isatty(STDOUT_FILENO)) {
        std::cout << "\033[?25h" << std::flush;
    }
}

void clear_screen() {
    std::cout << "\033[H\033[J" << std::flush;
}

/*
 * =============================================================================
 * Remove Think Tags
 * =============================================================================
 * 
 * Strips
<think>

...
</think>



tags and their content from assistant output.
 * Used with --remove-think-tags flag to hide reasoning content.
 * 
 * Format:
 *   Start: \n
<think>

\n
 *   End:   \n
</think>



\n\n
 * 
 * Example:
 *   Input:  "Let me think\n
<think>

\nHmm, the answer is...\n
</think>



\n\n42"
 *   Output: "Let me think\n42"
 * 
 * Used for: Models that output reasoning in think tags (e.g., DeepSeek-R1)
 * =============================================================================
 */
std::string remove_think_tags(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    
    bool in_think_tag = false;
    size_t i = 0;
    
    while (i < text.size()) {
        if (!in_think_tag && i + 9 <= text.size() && 
            text[i] == '\n' && 
            text[i+1] == '<' && text[i+2] == 't' && text[i+3] == 'h' &&
            text[i+4] == 'i' && text[i+5] == 'n' && text[i+6] == 'k' &&
            text[i+7] == '>' && text[i+8] == '\n') {
            in_think_tag = true;
            i += 9;
            continue;
        }
        
        if (in_think_tag && i + 11 <= text.size() &&
            text[i] == '\n' &&
            text[i+1] == '<' && text[i+2] == '/' && text[i+3] == 't' &&
            text[i+4] == 'h' && text[i+5] == 'i' && text[i+6] == 'n' &&
            text[i+7] == 'k' && text[i+8] == '>' && text[i+9] == '\n' &&
            text[i+10] == '\n') {
            in_think_tag = false;
            i += 11;
            continue;
        }
        
        if (!in_think_tag) {
            result += text[i];
        }
        i++;
    }
    
    return result;
}
