#include "syntax_highlighter.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "loader.hpp"
#include "common.hpp"

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <sys/ioctl.h>
#include <atomic>
#include <poll.h>
#include <errno.h>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <regex>
#include <wchar.h>
#include <cwchar>
#include <clocale>

extern std::atomic<bool> g_running;
static std::recursive_mutex g_terminal_mutex;

static Loader* g_loader = nullptr;

void set_loader_for_highlighter(Loader* loader) {
    g_loader = loader;
    set_common_loader(loader);
}

std::string SyntaxHighlighter::StreamingHighlighter::apply_simple_ansi(const std::string& text) {
    Logger::debug("[HL] apply_simple_ansi: %zu bytes", text.size());
    
    std::string result;
    size_t i = 0;

    while (i < text.size()) {
        bool matched = false;

        // bold
        if (i + 4 <= text.size() && text[i] == '*' && text[i + 1] == '*') {
            if (i + 2 < text.size() && !std::isspace(text[i + 2])) {
                size_t start = i + 2;
                size_t end = text.find("**", start);
                if (end != std::string::npos && end > start) {
                    std::string content = text.substr(start, end - start);
                    if (content.find("**") == std::string::npos) {
                        result += "\033[1m" + content + "\033[22m";
                        i = end + 2;
                        matched = true;
                    }
                }
            }
        }

        // italic
        if (!matched && i + 2 <= text.size() && text[i] == '*') {
            if (i + 1 >= text.size() || text[i + 1] != '*') {
                if (i + 1 < text.size() && !std::isspace(text[i + 1])) {
                    size_t start = i + 1;
                    size_t end = text.find('*', start);
                    if (end != std::string::npos && end > start) {
                        std::string content = text.substr(start, end - start);
                        if (content.find('*') == std::string::npos && content.find('\n') == std::string::npos) {
                            result += "\033[3m" + content + "\033[23m";
                            i = end + 1;
                            matched = true;
                        }
                    }
                }
            }
        }

        // quote/inverse
        if (!matched && text[i] == '`') {
            size_t backtick_count = 0;
            while (i < text.size() && text[i] == '`') {
                backtick_count++;
                i++;
            }
            
            size_t content_start = i;
            size_t search_pos = content_start;
            size_t content_end = std::string::npos;
            
            while (search_pos < text.size()) {
                size_t found = text.find(std::string(backtick_count, '`'), search_pos);
                if (found == std::string::npos) break;
                
                size_t after_close = found + backtick_count;
                if (after_close >= text.size() || text[after_close] != '`') {
                    if (found == content_start || text[found - 1] != '`') {
                        content_end = found;
                        break;
                    }
                }
                search_pos = found + 1;
            }
            
            if (content_end != std::string::npos && content_end > content_start) {
                std::string content = text.substr(content_start, content_end - content_start);
                if (content.find('\n') == std::string::npos) {
                    result += "\033[7m" + content + "\033[27m";
                    i = content_end + backtick_count;
                    matched = true;
                } else {
                    result += std::string(backtick_count, '`');
                    i = content_start;
                }
            } else {
                result += std::string(backtick_count, '`');
            }
        }

        if (!matched && i + 4 <= text.size() && text[i] == '~' && text[i + 1] == '~') {
            size_t start = i + 2;
            size_t end = text.find("~~", start);
            if (end != std::string::npos && end > start) {
                std::string content = text.substr(start, end - start);
                result += "\033[9m" + content + "\033[29m";
                i = end + 2;
                matched = true;
            }
        }

        if (!matched) {
            result += text[i];
            i++;
        }
    }

    Logger::debug("[HL] apply_simple_ansi output: %zu bytes", result.size());
    return result;
}

void SyntaxHighlighter::StreamingHighlighter::show_ghost(const std::string& text, bool is_update) {
    Logger::debug("[Ghost] SHOW called, text_len=%zu, preview=%d, active=%d, current_lines=%zu, is_update=%d",
                  text.size(), preview_active_, ghost_active_, ghost_lines_, is_update);

    if (text.empty()) {
        Logger::debug("[Ghost] SHOW skipped, text empty");
        return;
    }

    if (!preview_active_) return;

    std::lock_guard<std::recursive_mutex> term_lock(g_terminal_mutex);

    size_t term_width = ::get_terminal_width();
    if (term_width == 0) term_width = 80;

    // FIX: Use calculate_display_width_no_ansi to properly handle ANSI codes
    size_t text_width = calculate_display_width_no_ansi(text);
    size_t lines_needed = (text_width + term_width - 1) / term_width;
    if (lines_needed == 0) lines_needed = 1;

    if (ghost_active_ && last_term_width_ != term_width) {
        Logger::debug("[Ghost] Terminal resized %zu -> %zu, clearing old ghost (%zu lines)",
                     last_term_width_, term_width, ghost_lines_);
        clear_ghost();
    }

    if (is_update && ghost_active_ && text.size() >= last_ghost_text_.size() &&
        text.substr(0, last_ghost_text_.size()) == last_ghost_text_) {
        
        Logger::debug("[Ghost] Optimized update: moving up %zu lines and overwriting", ghost_lines_);
        
        // FIX: Move to beginning of line first, then up
        std::cout << "\r";
        if (ghost_lines_ > 1) {
            std::cout << "\033[" << (ghost_lines_ - 1) << "A";
        }
        
        //// FIX: Clear the line before writing
        //std::cout << "\033[K";
        std::cout << text << std::flush;
        
        ghost_lines_ = lines_needed;
        // FIX: Store width without ANSI for residue calculation
        ghost_text_width_ = text_width;
        last_ghost_text_ = text;
        
        Logger::debug("[Ghost] Updated in place, new lines=%zu, width=%zu", ghost_lines_, ghost_text_width_);
        return;
    }

    if (ghost_active_) {
        clear_ghost();
    }

    Logger::debug("[Ghost] Displaying %zu lines of ghost text (%zu cols wide)",
                 lines_needed, text_width);

    // FIX: Clear line before showing ghost text
    //std::cout << "\r\033[K";
    std::cout << "\r";
    std::cout << text << std::flush;

    ghost_active_ = true;
    ghost_lines_ = lines_needed;
    last_term_width_ = term_width;
    // FIX: Store width without ANSI for residue calculation
    ghost_text_width_ = text_width;
    last_ghost_text_ = text;

    Logger::debug("[Ghost] ghost_text_width_ set to %zu", ghost_text_width_);
}

void SyntaxHighlighter::StreamingHighlighter::clear_ghost() {
    if (!ghost_active_) {
        return;
    }

    std::lock_guard<std::recursive_mutex> term_lock(g_terminal_mutex);

    Logger::debug("[Ghost] Clearing %zu lines of ghost text", ghost_lines_);

    //// FIX: Move to beginning of line first
    //std::cout << "\r";
    //if (ghost_lines_ > 1) {
    //    std::cout << "\033[" << (ghost_lines_ - 1) << "A";
    //}

    //// Clear each line
    //for (size_t i = 0; i < ghost_lines_; ++i) {
    //    std::cout << "\033[K";
    //    if (i < ghost_lines_ - 1) {
    //        std::cout << "\n";
    //    }
    //}

    //// FIX: Return to original position
    //if (ghost_lines_ > 1) {
    //    std::cout << "\033[" << (ghost_lines_ - 1) << "A";
    //}
    //std::cout << "\r";

    for (size_t i = 0; i < ghost_lines_ - 1; ++i) {
        std::cout << "\033[2K\033[1F";
    }
    std::cout << "\033[2K\r";

    //if (ghost_lines_ > 1) {
    //    std::cout << "\033[" << (ghost_lines_ - 1) << "F";
    //} else {
    //    std::cout << "\r";
    //}

    std::cout << std::flush;

    ghost_active_ = false;
    ghost_lines_ = 0;
    ghost_length_ = 0;
    ghost_text_width_ = 0;
    last_ghost_text_.clear();
}

void SyntaxHighlighter::StreamingHighlighter::clear_residue(size_t rendered_width) {
    Logger::debug("[Residue] clear_residue called: ghost_text_width_=%zu, rendered_width=%zu, preview=%d",
                 ghost_text_width_, rendered_width, preview_active_);

    if (!preview_active_ || ghost_text_width_ == 0) {
        Logger::debug("[Residue] Skipping: preview=%d, ghost_text_width_=%zu",
                     preview_active_, ghost_text_width_);
        ghost_text_width_ = 0;
        return;
    }

    std::lock_guard<std::recursive_mutex> term_lock(g_terminal_mutex);

    if (ghost_text_width_ > rendered_width) {
        size_t term_width = ::get_terminal_width();

        size_t ghost_lines = (ghost_text_width_ + term_width - 1) / term_width;
        size_t rendered_lines = (rendered_width + term_width - 1) / term_width;
        if (rendered_lines == 0) rendered_lines = 1;

        Logger::debug("[Residue] Clearing residue: ghost=%zu cols/%zu lines, rendered=%zu cols/%zu lines",
                     ghost_text_width_, ghost_lines, rendered_width, rendered_lines);

        std::cout << "\033[s";

        //// Move N lines up
        //if (ghost_lines > 1) {
        //    std::cout << "\033[" << (ghost_lines - 1) << "F";
        //}

        //// Move down to last rendered line
        //if (rendered_lines > 1) {
        //    std::cout << "\033[" << (rendered_lines - 1) << "E";
        //}

        if ( ghost_lines - rendered_lines > 0 ) {
            std::cout << "\033[" << (ghost_lines - rendered_lines) << "F";
        }

        // Move to column after rendered text
        size_t rendered_final_col = rendered_width % term_width;
        std::cout << "\033[" << (rendered_final_col + 1) << "G";
        
        // Clear from cursor to end of line
        std::cout << "\033[K";

        // Clear remaining lines if ghost had more lines
        if (ghost_lines > rendered_lines) {
            for (size_t i = rendered_lines; i < ghost_lines; ++i) {
                std::cout << "\033[K";
                if (i < ghost_lines - 1) std::cout << "\033[1E";
            }
        }

        //// FIX: Return to original position
        //if (ghost_lines > 1) {
        //    std::cout << "\033[" << (ghost_lines - 1) << "F";
        //}
        //else { std::cout << "\r"; }
        //if (rendered_lines > 1) {
        //    std::cout << "\033[" << (rendered_lines - 1) << "E";
        //}
        //std::cout << "\033[" << (rendered_final_col + 1) << "G";

        std::cout << "\033[u";

        std::cout << std::flush;
        Logger::debug("[Residue] Residue cleared");
    } else {
        Logger::debug("[Residue] No residue needed (ghost=%zu <= rendered=%zu)",
                     ghost_text_width_, rendered_width);
    }

    ghost_text_width_ = 0;
    Logger::debug("[Residue] ghost_text_width_ reset to 0");
}

void SyntaxHighlighter::StreamingHighlighter::start(const std::string& lang, const std::string& theme) {
    Logger::debug("[HL] START called, lang='%s', theme='%s', simple=%d", 
                  lang.c_str(), theme.c_str(), simple_mode_);
    
    std::lock_guard<std::recursive_mutex> term_lock(g_terminal_mutex);
    
    if (simple_mode_) {
        std::unique_lock<std::mutex> lock(mutex_);
        theme_ = theme;
        lang_  = lang;
        preview_active_ = Config::instance().preview_enabled();
        line_buffer_.clear();
        ghost_active_ = false;
        ghost_hash_ = 0;
        ghost_length_ = 0;
        ghost_text_width_ = 0;
        ghost_lines_ = 0;
        last_ghost_text_.clear();
        lines_processed_ = 0;
        spinner_idx_ = 0;
        highlight_dead_ = false;
        child_pid = -1;
        write_fd = -1;
        read_fd = -1;
        Logger::debug("[HL] Simple mode initialized, preview=%d", preview_active_);
        return;
    }

    if (child_pid != -1) {
        Logger::debug("[HL] Ending existing highlighter before start");
        end();
    }

    std::unique_lock<std::mutex> lock(mutex_);
    theme_ = theme;
    lang_  = lang;
    preview_active_ = Config::instance().preview_enabled();
    line_buffer_.clear();
    ghost_active_ = false;
    ghost_hash_ = 0;
    ghost_length_ = 0;
    ghost_text_width_ = 0;
    ghost_lines_ = 0;
    last_ghost_text_.clear();
    lines_processed_ = 0;
    spinner_idx_ = 0;
    highlight_dead_ = false;

    Logger::debug("[HL] Regular mode (forked), preview=%d, lang='%s', theme='%s'", 
                  preview_active_, lang.c_str(), theme.c_str());

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) == -1 || pipe(out_pipe) == -1) {
        char err_buf[256];
        Logger::error("[HL] Failed to create pipes: %s",
                     strerror_r(errno, err_buf, sizeof(err_buf)));
        return;
    }

    child_pid = fork();
    if (child_pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);

        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDERR_FILENO);
        close(devnull);

        close(in_pipe[0]);
        close(out_pipe[1]);
        close(in_pipe[1]);
        close(out_pipe[0]);

        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 4096;
        for (int i = 3; i < max_fd; ++i) close(i);

        const char* syntax = lang.empty() ? "txt" : lang.c_str();
        setvbuf(stdout, NULL, _IOLBF, 0);
        
        long tab_w = Config::instance().get_tab_width();
        std::string replace_tabs_arg = "--replace-tabs=" + std::to_string(tab_w);
        Logger::debug("[HL] Child exec: highlight -O xterm256 --stdout -s %s -S %s %s",
                     theme.c_str(), syntax, replace_tabs_arg.c_str());

        execlp("highlight", "highlight", "-O", "xterm256", "--stdout",
               "-s", theme_.c_str(), "-S", syntax, "--force",
               replace_tabs_arg.c_str(), nullptr);
        _exit(127);
    }

    if (child_pid < 0) {
        char err_buf[256];
        Logger::error("[HL] Failed to fork: %s",
                     strerror_r(errno, err_buf, sizeof(err_buf)));
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return;
    }

    read_fd   = out_pipe[0];
    write_fd  = in_pipe[1];
    close(in_pipe[0]);
    close(out_pipe[1]);

    fcntl(read_fd, F_SETFL, fcntl(read_fd, F_GETFL) | O_NONBLOCK);
    fcntl(write_fd, F_SETFL, fcntl(write_fd, F_GETFL) | O_NONBLOCK);

    Logger::debug("[HL] Started child PID %d, read_fd=%d, write_fd=%d, preview=%d",
                 child_pid, read_fd, write_fd, preview_active_);
}

std::string SyntaxHighlighter::StreamingHighlighter::feed(const std::string& code) {
    Logger::debug("[HL] FEED called, bytes=%zu, simple=%d, preview=%d, child_pid=%d, ghost_active=%d",
                  code.size(), simple_mode_, preview_active_, child_pid, ghost_active_);

    if (simple_mode_) {
        Logger::debug("[HL] Simple mode feed");
        std::string output_accumulator;
        std::unique_lock<std::mutex> lock(mutex_);
        line_buffer_ += code;

        while (true) {
            size_t newline_pos = line_buffer_.find('\n');
            if (newline_pos == std::string::npos) {
                if (preview_active_ && !line_buffer_.empty()) {
                    size_t current_hash = std::hash<std::string>{}(line_buffer_);
                    if (current_hash != ghost_hash_) {
                        Logger::debug("[HL] Simple: showing ghost, hash=%zu, len=%zu",
                                     current_hash, line_buffer_.size());
                        bool is_update = ghost_active_;
                        lock.unlock();
                        show_ghost(line_buffer_, is_update);
                        lock.lock();
                        ghost_hash_ = current_hash;
                    }
                }
                break;
            }

            std::string complete_line = line_buffer_.substr(0, newline_pos + 1);
            std::string line_content = complete_line.substr(0, newline_pos);

            if (preview_active_ && !line_content.empty()) {
                Logger::debug("[HL] Simple: showing ghost for complete line (%zu bytes)", line_content.size());
                bool is_update = ghost_active_;
                lock.unlock();
                show_ghost(line_content, is_update);
                lock.lock();
            }

            std::string styled = apply_simple_ansi(complete_line);
            
            bool content_changed = (styled != complete_line);

            if (preview_active_ && ghost_active_ && !styled.empty()) {
                if (content_changed) {
                    Logger::debug("[HL] Simple: clearing ghost after styled output (content changed)");
                    lock.unlock();
                    
                    clear_ghost();
                    
                    // FIX: Calculate rendered width WITHOUT ANSI codes
                    size_t rendered_width = calculate_display_width_no_ansi(styled);
                    Logger::debug("[HL] Styled output: %zu bytes, rendered_width=%zu", styled.size(), rendered_width);
                    
                    clear_residue(rendered_width);
                    
                    lock.lock();
                    output_accumulator += styled;
                } else {
                    Logger::debug("[HL] Simple: content unchanged, keeping ghost, adding newline only");
                    output_accumulator += "\n";
                    ghost_active_ = false;
                    ghost_hash_ = 0;
                    ghost_text_width_ = 0;
                    ghost_lines_ = 0;
                    last_ghost_text_.clear();
                }
            } else {
                output_accumulator += styled;
            }

            lines_processed_++;
            line_buffer_.erase(0, newline_pos + 1);
        }

        Logger::debug("[HL] Simple mode feed complete, output=%zu bytes", output_accumulator.size());
        return output_accumulator;
    }

    if (highlight_dead_) {
        Logger::debug("[HL] FEED: highlight dead, returning raw code");
        return code;
    }

    if (child_pid == -1 || write_fd == -1) {
        Logger::debug("[HL] FEED: not active (child_pid=%d, write_fd=%d)", child_pid, write_fd);
        return code;
    }

    std::string output_accumulator;
    std::unique_lock<std::mutex> lock(mutex_);
    line_buffer_ += code;

    Logger::debug("[HL] Feed: buffer now %zu bytes, preview=%d", line_buffer_.size(), preview_active_);

    while (true) {
        if (!g_running.load(std::memory_order_relaxed)) {
            Logger::debug("[HL] Feed interrupted by g_running");
            break;
        }

        size_t newline_pos = line_buffer_.find('\n');
        if (newline_pos == std::string::npos) {
            if (preview_active_ && !line_buffer_.empty()) {
                size_t current_hash = std::hash<std::string>{}(line_buffer_);
                if (current_hash != ghost_hash_) {
                    Logger::debug("[HL] Regular: showing ghost for incomplete line, hash=%zu, len=%zu", 
                                 current_hash, line_buffer_.size());
                    lock.unlock();
                    show_ghost(line_buffer_, ghost_active_);
                    lock.lock();
                    ghost_hash_ = current_hash;
                }
            }
            Logger::debug("[HL] No complete line yet, remaining: %zu bytes", line_buffer_.size());
            break;
        }

        std::string complete_line = line_buffer_.substr(0, newline_pos + 1);
        std::string line_content = complete_line.substr(0, newline_pos);
        bool line_has_content = (line_content.find_first_not_of(" \t\r") != std::string::npos);

        Logger::debug("[HL] Complete line: has_content=%d, length=%zu", line_has_content, line_content.size());

        if (line_has_content) {
            if (preview_active_ && !line_content.empty()) {
                Logger::debug("[HL] Regular: showing ghost before highlight (%zu bytes)", line_content.size());
                lock.unlock();
                show_ghost(line_content, ghost_active_);
                lock.lock();
            }

            const char* data = complete_line.c_str();
            size_t remaining = complete_line.size();

            while (remaining > 0 && g_running.load(std::memory_order_relaxed)) {
                ssize_t written = write(write_fd, data, std::min(remaining, static_cast<size_t>(4096)));
                if (written == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        usleep(1000);
                        continue;
                    }
                    if (errno == EPIPE) {
                        Logger::warn("[HL] Broken pipe: highlight process died");
                        highlight_dead_ = true;
                        if (write_fd != -1) {
                            close(write_fd);
                            write_fd = -1;
                        }
                        output_accumulator += line_content + "\n";
                        line_buffer_.erase(0, newline_pos + 1);
                        goto line_processed;
                    }
                    Logger::error("[HL] Write error: %s", strerror(errno));
                    break;
                }
                if (written == 0) {
                    Logger::error("[HL] Write returned 0");
                    break;
                }
                data += written;
                remaining -= written;
            }

            Logger::debug("[HL] Finished writing line to highlighter");

            std::string highlight_output;
            lock.unlock();
            {
                int retries = 0;
                const int max_retries = 100;
                while (retries < max_retries && g_running.load()) {
                    struct pollfd pfd;
                    pfd.fd = read_fd;
                    pfd.events = POLLIN;
                    int ready = poll(&pfd, 1, 10);
                    if (ready > 0 && (pfd.revents & POLLIN)) {
                        char out_buf[8192];
                        ssize_t bytes_read;
                        while ((bytes_read = read(read_fd, out_buf, sizeof(out_buf))) > 0) {
                            highlight_output.append(out_buf, bytes_read);
                        }
                        if (!highlight_output.empty() && contains_newline(highlight_output)) {
                            break;
                        }
                    } else if (ready < 0 && errno != EINTR) {
                        Logger::error("[HL] Poll error: %s", strerror(errno));
                        break;
                    }
                    if (!highlight_output.empty() && ready == 0) {
                        break;
                    }
                    retries++;
                }
            }
            lock.lock();

            Logger::debug("[HL] Highlight output: %zu bytes", highlight_output.size());

            std::string processed = strip_leading_ansi_reset_newline(highlight_output);

            {
                std::lock_guard<std::recursive_mutex> term_lock(g_terminal_mutex);

                if (ghost_active_) {
                    Logger::debug("[HL] Clearing ghost BEFORE highlighted output");
                    clear_ghost();
                }

                if (!processed.empty()) {
                    output_accumulator += processed;
                    if (!ends_with_newline_ignore_ansi(processed)) {
                        output_accumulator += "\n";
                    }
                    
                    // FIX: Calculate rendered width WITHOUT ANSI codes
                    size_t rendered_width = calculate_display_width_no_ansi(processed);
                    clear_residue(rendered_width);
                } else {
                    Logger::debug("[HL] No highlight output, using raw line");
                    output_accumulator += line_content + "\n";
                }
            }
        } else {
            std::lock_guard<std::recursive_mutex> term_lock(g_terminal_mutex);
            if (ghost_active_) {
                Logger::debug("[HL] Empty line, clearing ghost");
                clear_ghost();
            }
            output_accumulator += "\n";
        }

        lines_processed_++;
        line_buffer_.erase(0, newline_pos + 1);
        Logger::debug("[HL] Line processed, remaining: %zu bytes", line_buffer_.size());

        line_processed:;
    }

    if (preview_active_ && g_running.load()) {
        if (!line_buffer_.empty()) {
            size_t current_hash = std::hash<std::string>{}(line_buffer_);
            if (current_hash != ghost_hash_) {
                Logger::debug("[HL] Post-loop: showing ghost for remaining buffer");
                lock.unlock();
                show_ghost(line_buffer_, ghost_active_);
                lock.lock();
                ghost_hash_ = current_hash;
            }
        } else if (ghost_active_) {
            Logger::debug("[HL] Post-loop: buffer empty, clearing ghost");
            lock.unlock();
            clear_ghost();
            lock.lock();
            ghost_hash_ = 0;
        }
    }

    Logger::debug("[HL] FEED complete, output=%zu bytes", output_accumulator.size());
    return output_accumulator;
}

std::string SyntaxHighlighter::StreamingHighlighter::end() {
    Logger::debug("[HL] END called, simple=%d, child_pid=%d, ghost_active=%d, ghost_text_width_=%zu",
                  simple_mode_, child_pid, ghost_active_, ghost_text_width_);

    if (simple_mode_) {
        std::string final_output;
        
        if (!line_buffer_.empty()) {
            final_output = apply_simple_ansi(line_buffer_);
            
            if (preview_active_ && ghost_active_) {
                std::lock_guard<std::recursive_mutex> term_lock(g_terminal_mutex);
                
                // FIX: Save ghost_text_width_ BEFORE clearing
                size_t saved_ghost_width = ghost_text_width_;
                
                clear_ghost();
                
                std::cout << final_output << std::flush;
                
                if (!final_output.empty() && final_output.back() != '\n') {
                    std::cout << "\n" << std::flush;
                }
                
                // FIX: Clear residue using saved width
                if (saved_ghost_width > 0) {
                    ghost_text_width_ = saved_ghost_width;
                    // FIX: Calculate rendered width WITHOUT ANSI codes
                    clear_residue(calculate_display_width_no_ansi(final_output));
                }
                
                ghost_hash_ = 0;
                ghost_length_ = 0;
                ghost_text_width_ = 0;
                ghost_lines_ = 0;
                last_ghost_text_.clear();
                ghost_active_ = false;
                
                line_buffer_.clear();
                simple_mode_ = false;
                Logger::debug("[HL] Simple mode end complete, ghost cleared, output=%zu bytes", final_output.size());
                return "";
            }
        }
        
        line_buffer_.clear();
        ghost_hash_ = 0;
        ghost_length_ = 0;
        ghost_text_width_ = 0;
        ghost_lines_ = 0;
        last_ghost_text_.clear();
        ghost_active_ = false;
        simple_mode_ = false;
        Logger::debug("[HL] Simple mode end complete, output=%zu bytes", final_output.size());
        return final_output;
    }
                
    if (child_pid == -1) {
        Logger::debug("[HL] END: child_pid=-1, returning empty");
        return "";
    }

    Logger::debug("[HL] Ending highlighter, flushing buffer (%zu bytes): '%s'",
                 line_buffer_.size(), hex_encode_string(line_buffer_, 60).c_str());

    std::unique_lock<std::mutex> lock(mutex_);

    if (!line_buffer_.empty() && write_fd != -1 && !highlight_dead_) {
        const char* data = line_buffer_.c_str();
        size_t remaining = line_buffer_.size();
        int attempts = 0;
        Logger::debug("[HL] Writing remaining buffer to highlighter");

        while (remaining > 0 && attempts < 100 && g_running.load()) {
            ssize_t written = write(write_fd, data, std::min(remaining, static_cast<size_t>(4096)));
            if (written == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000);
                    attempts++;
                    continue;
                }
                if (errno == EPIPE) {
                     Logger::warn("[HL] Broken pipe during end(). Flushing raw buffer.");
                     highlight_dead_ = true;
                     break;
                }
                Logger::error("[HL] Write error in end(): %s", strerror(errno));
                break;
            }
            if (written == 0) {
                Logger::error("[HL] Write returned 0 in end()");
                break;
            }
            data += written;
            remaining -= written;
            if (written > 0) attempts = 0;
        }

        Logger::debug("[HL] Writing final newline to flush highlighter");
        const char nl = '\n';
        write(write_fd, &nl, 1);
    } else {
        Logger::debug("[HL] Buffer empty, pipe closed, or highlight dead, not writing final newline");
    }

    if (write_fd != -1) {
        close(write_fd);
        write_fd = -1;
    }

    int flags = fcntl(read_fd, F_GETFL, 0);
    if (flags != -1 && (flags & O_NONBLOCK)) {
    }

    std::string final_output;
    char out_buf[8192];
    ssize_t bytes_read;
    int drain_attempts = 0;
    Logger::debug("[HL] Draining highlighter output");

    while (drain_attempts < 100) {
        bytes_read = read(read_fd, out_buf, sizeof(out_buf));
        if (bytes_read > 0) {
            final_output.append(out_buf, bytes_read);
            Logger::debug("[HL] Drained %zd bytes", bytes_read);
            drain_attempts = 0;
        } else if (bytes_read == 0) {
            Logger::debug("[HL] Read EOF from highlighter");
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                drain_attempts++;
            } else {
                Logger::error("[HL] Read error in end(): %s", strerror(errno));
                break;
            }
        }
    }

    Logger::debug("[HL] Final output: %zu bytes", final_output.size());

    {
        std::lock_guard<std::recursive_mutex> term_lock(g_terminal_mutex);
        if (ghost_active_) {
            Logger::debug("[HL] Clearing ghost before final output");
            clear_ghost();
        }
        
        if (!final_output.empty() && ghost_text_width_ > 0) {
            // FIX: Calculate rendered width WITHOUT ANSI codes
            size_t rendered_width = calculate_display_width_no_ansi(final_output);
            Logger::debug("[HL] Final: rendered_width=%zu, ghost_text_width_=%zu", rendered_width, ghost_text_width_);
            clear_residue(rendered_width);
        }
    }

    if (!line_buffer_.empty() && (final_output.empty() || highlight_dead_)) {
        Logger::debug("[HL] No output from pipe, appending raw buffer: '%s'",
                     hex_encode_string(line_buffer_, 60).c_str());
        final_output += line_buffer_;
        if (final_output.back() != '\n') final_output += "\n";
    }

    line_buffer_.clear();
    ghost_hash_ = 0;
    ghost_length_ = 0;
    ghost_text_width_ = 0;
    last_ghost_text_.clear();
    highlight_dead_ = false;

    int status;
    pid_t waited_pid = -1;
    for (int i = 0; i < 50; ++i) {
        waited_pid = waitpid(child_pid, &status, WNOHANG);
        if (waited_pid == child_pid) break;
        if (waited_pid == -1) {
            break;
        }
        usleep(100000);
    }

    if (waited_pid != child_pid && child_pid != -1) {
        Logger::debug("[HL] Killing stubborn child PID %d", child_pid);
        kill(child_pid, SIGKILL);
        waitpid(child_pid, nullptr, 0);
    }
    Logger::debug("[HL] Highlighter ended successfully");
    if (read_fd != -1) {
        close(read_fd);
        read_fd = -1;
    }
    child_pid = -1;

    Logger::debug("[HL] END complete, output=%zu bytes", final_output.size());
    return final_output;
}
