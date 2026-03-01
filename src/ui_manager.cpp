#include "ui_manager.hpp"
#include "clipboard.hpp"
#include "logger.hpp"
#include "chat_controller.hpp"
#include "common.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <cstring>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>

struct TermiosGuard {
    struct termios old_tio;
    bool active = false;

    TermiosGuard() = default;

    void save() {
        if (isatty(STDIN_FILENO)) {
            tcgetattr(STDIN_FILENO, &old_tio);
            active = true;
        }
    }

    void restore() {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
        }
    }

    ~TermiosGuard() {
        restore();
    }
};

std::string UIManager::select_conversation_interactive(const std::vector<ConversationManager::ConversationInfo>& conv_infos) {
    if (conv_infos.empty() || !is_terminal_output()) return "";

    TermiosGuard term_guard;
    term_guard.save();

    struct termios new_tio = term_guard.old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    new_tio.c_cc[VMIN] = 1;
    new_tio.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

    hide_cursor();

    const size_t HEADER_LINES = 3;
    const size_t FOOTER_LINES = 2;
    const size_t RESERVED_LINES = HEADER_LINES + FOOTER_LINES;

    size_t term_height = get_terminal_height();
    size_t term_width = get_terminal_width();
    
    Logger::debug("[UI] Terminal: %zux%zu", term_width, term_height);
    
    size_t page_size = (term_height > RESERVED_LINES) ? (term_height - RESERVED_LINES) : 10;
    if (page_size < 5) page_size = 5;

    const size_t METADATA_WIDTH = 60;
    size_t max_title_width = (term_width > METADATA_WIDTH) ? (term_width - METADATA_WIDTH) : 20;
    if (max_title_width < 10) max_title_width = 10;

    Logger::debug("[UI] Max title width: %zu", max_title_width);

    size_t selected = 0;
    size_t start_idx = 0;

    auto render_menu = [&](size_t start_idx, size_t selected_idx) {
        clear_screen();
        
        std::cout << "\033[1m=== Select Conversation ===\033[22m\n";
        std::cout << "\033[2m(arrows/PageUp/PageDown navigate, Enter selects, q quits)\033[22m\n\n";

        size_t end_idx = std::min(start_idx + page_size, conv_infos.size());
        size_t actual_lines_rendered = 0;
        
        for (size_t i = start_idx; i < end_idx; ++i) {
            const auto& info = conv_infos[i];
            auto time_t = std::chrono::system_clock::to_time_t(info.timestamp);
            
            // Expand tabs in title just in case (though titles shouldn't have tabs)
            std::string safe_title = expand_tabs(info.title);
            std::string trimmed_title = trim_to_width(safe_title, max_title_width);
            
            char index_buf[24];
            snprintf(index_buf, sizeof(index_buf), "%3zu.", i + 1);
            
            char date_buf[32];
            strftime(date_buf, sizeof(date_buf), "(%Y-%m-%d %H:%M)", std::localtime(&time_t));
            
            std::string model_str = info.model.empty() ? "" : std::string("[") + info.model + "]";
            std::string interrupted_str = info.interrupted ? "[Interrupted]" : "";
            
            std::string line;
            line.reserve(term_width);
            line += index_buf;
            line += " ";
            line += trimmed_title;
            line += " ";
            line += date_buf;
            
            if (!model_str.empty()) {
                line += " \033[36m";
                line += model_str;
                line += "\033[0m";
            }
            if (!interrupted_str.empty()) {
                line += " \033[31m";
                line += interrupted_str;
                line += "\033[0m";
            }
            
            size_t line_display_width = calculate_display_width_no_ansi(line);
            if (line_display_width > term_width) {
                size_t excess = line_display_width - term_width;
                if (trimmed_title.size() > excess + 3) {
                    trimmed_title = trim_to_width(safe_title, max_title_width - excess - 5);
                    
                    line.clear();
                    line += index_buf;
                    line += " ";
                    line += trimmed_title;
                    line += " ";
                    line += date_buf;
                    if (!model_str.empty()) {
                        line += " \033[36m";
                        line += model_str;
                        line += "\033[0m";
                    }
                    if (!interrupted_str.empty()) {
                        line += " \033[31m";
                        line += interrupted_str;
                        line += "\033[0m";
                    }
                }
            }
            
            size_t line_count = calculate_line_count(line, term_width);
            
            if (actual_lines_rendered + line_count > page_size) {
                break;
            }
            
            if (i == selected_idx) {
                std::cout << "\033[7m\033[1m";
            } else {
                std::cout << "\033[22m\033[27m";
            }
            
            std::cout << line << "\n";
            actual_lines_rendered += line_count;
        }

        size_t remaining = page_size - actual_lines_rendered;
        for (size_t j = 0; j < remaining && j < 2; ++j) {
            std::cout << "\n";
        }

        if (conv_infos.size() > page_size) {
            std::cout << "\033[2m(" << (start_idx + 1) << "-" << end_idx << " of " 
                      << conv_infos.size() << ")\033[22m\n";
        } else {
            std::cout << "\n";
        }
        
        std::cout << "> ";
        std::cout << std::flush;
    };

    render_menu(start_idx, selected);

    std::string selected_title;
    bool running = true;
    
    while (running) {
        char ch;
        ssize_t r = read(STDIN_FILENO, &ch, 1);
        if (r != 1) {
            if (errno == EINTR) continue;
            break;
        }

        if (ch == '\033') {
            char seq[3] = {0};
            ssize_t seq_len = 0;
            
            for (int i = 0; i < 3; ++i) {
                struct pollfd fds[1];
                fds[0].fd = STDIN_FILENO;
                fds[0].events = POLLIN;
                int ret = poll(fds, 1, 50);
                if (ret > 0 && (fds[0].revents & POLLIN)) {
                    ssize_t nr = read(STDIN_FILENO, &seq[i], 1);
                    if (nr == 1) {
                        seq_len = i + 1;
                        if (seq[i] == '~' || seq[i] == 'A' || seq[i] == 'B' || 
                            seq[i] == 'C' || seq[i] == 'D' || seq[i] == 'H' || 
                            seq[i] == 'F') {
                            break;
                        }
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }

            if (seq_len >= 2 && seq[0] == '[') {
                if (seq[1] == 'A') {
                    if (selected > 0) {
                        selected--;
                        if (selected < start_idx) start_idx = selected;
                        render_menu(start_idx, selected);
                    }
                } else if (seq[1] == 'B') {
                    if (selected + 1 < conv_infos.size()) {
                        selected++;
                        if (selected >= start_idx + page_size) {
                            start_idx = selected - page_size + 1;
                        }
                        render_menu(start_idx, selected);
                    }
                } else if (seq[1] == 'H') {
                    start_idx = 0;
                    selected = 0;
                    render_menu(start_idx, selected);
                } else if (seq[1] == 'F') {
                    if (conv_infos.size() > page_size) {
                        start_idx = conv_infos.size() - page_size;
                    } else {
                        start_idx = 0;
                    }
                    selected = conv_infos.size() - 1;
                    render_menu(start_idx, selected);
                } else if (seq[1] >= '0' && seq[1] <= '9') {
                    std::string param_seq;
                    param_seq += seq[1];
                    
                    for (int i = 2; i < seq_len; ++i) {
                        param_seq += seq[i];
                        if (seq[i] == '~') break;
                    }
                    
                    while (param_seq.back() != '~' && param_seq.size() < 10) {
                        struct pollfd fds[1];
                        fds[0].fd = STDIN_FILENO;
                        fds[0].events = POLLIN;
                        int ret = poll(fds, 1, 50);
                        if (ret > 0 && (fds[0].revents & POLLIN)) {
                            char c;
                            if (read(STDIN_FILENO, &c, 1) == 1) {
                                param_seq += c;
                                if (c == '~') break;
                            } else {
                                break;
                            }
                        } else {
                            break;
                        }
                    }

                    if (param_seq == "5~") {
                        if (start_idx > 0) {
                            if (start_idx >= page_size) {
                                start_idx -= page_size;
                            } else {
                                start_idx = 0;
                            }
                            if (selected >= start_idx + page_size) {
                                selected = start_idx + page_size - 1;
                            }
                            render_menu(start_idx, selected);
                        }
                    } else if (param_seq == "6~") {
                        size_t max_start = (conv_infos.size() > page_size) ? 
                                          (conv_infos.size() - page_size) : 0;
                        if (start_idx < max_start) {
                            start_idx = std::min(max_start, start_idx + page_size);
                            if (selected < start_idx) selected = start_idx;
                            render_menu(start_idx, selected);
                        }
                    } else if (param_seq == "1~" || param_seq == "1;5A") {
                        start_idx = 0;
                        selected = 0;
                        render_menu(start_idx, selected);
                    } else if (param_seq == "4~" || param_seq == "1;5B") {
                        if (conv_infos.size() > page_size) {
                            start_idx = conv_infos.size() - page_size;
                        } else {
                            start_idx = 0;
                        }
                        selected = conv_infos.size() - 1;
                        render_menu(start_idx, selected);
                    }
                }
            } else if (seq_len >= 2 && seq[0] == 'O') {
                if (seq[1] == 'H') {
                    start_idx = 0;
                    selected = 0;
                    render_menu(start_idx, selected);
                } else if (seq[1] == 'F') {
                    if (conv_infos.size() > page_size) {
                        start_idx = conv_infos.size() - page_size;
                    } else {
                        start_idx = 0;
                    }
                    selected = conv_infos.size() - 1;
                    render_menu(start_idx, selected);
                }
            }
        } else if (ch == '\n' || ch == '\r') {
            selected_title = conv_infos[selected].title;
            running = false;
        } else if (ch == 'q' || ch == 'Q') {
            running = false;
        } else if (ch == 'j' || ch == 'J') {
            if (selected + 1 < conv_infos.size()) {
                selected++;
                if (selected >= start_idx + page_size) {
                    start_idx = selected - page_size + 1;
                }
                render_menu(start_idx, selected);
            }
        } else if (ch == 'k' || ch == 'K') {
            if (selected > 0) {
                selected--;
                if (selected < start_idx) start_idx = selected;
                render_menu(start_idx, selected);
            }
        } else if (ch == 'g' || ch == 'G') {
            if (ch == 'g') {
                start_idx = 0;
                selected = 0;
            } else {
                if (conv_infos.size() > page_size) {
                    start_idx = conv_infos.size() - page_size;
                } else {
                    start_idx = 0;
                }
                selected = conv_infos.size() - 1;
            }
            render_menu(start_idx, selected);
        }
    }

    show_cursor();
    clear_screen();
    
    return selected_title;
}

bool UIManager::prompt_save_interrupted() {
    g_interrupted.store(false);

    int tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd == -1) {
        Logger::warn("[UI] Save prompt skipped: could not open /dev/tty");
        return false;
    }

    struct termios orig_tio;
    if (tcgetattr(tty_fd, &orig_tio) == -1) {
        close(tty_fd);
        return false;
    }

    struct termios new_tio = orig_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    new_tio.c_cc[VMIN] = 0;
    new_tio.c_cc[VTIME] = 0;
    tcsetattr(tty_fd, TCSANOW, &new_tio);

    write(tty_fd, "\n[Interrupted] Save conversation? [y/N]: ", 41);

    char response_buf[4] = {0};
    int buf_idx = 0;
    bool decision_made = false;
    bool save = false;

    struct pollfd fds[1];
    fds[0].fd = tty_fd;
    fds[0].events = POLLIN;

    while (!decision_made) {
        if (g_interrupted.load()) {
            Logger::debug("[UI] Save prompt aborted");
            write(tty_fd, "Aborted.\n", 9);
            decision_made = true;
            break;
        }

        int ret = poll(fds, 1, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            Logger::error("[UI] Poll error: %s", strerror(errno));
            break;
        }

        if (ret > 0) {
            if (!(fds[0].revents & POLLIN) && (fds[0].revents & (POLLHUP | POLLERR))) {
                Logger::error("[UI] Poll hangup/error");
                break;
            }

            if (fds[0].revents & POLLIN) {
                char ch;
                ssize_t r = read(tty_fd, &ch, 1);
                if (r == 1) {
                    if (ch == '\n' || ch == '\r') {
                        if (buf_idx == 0) {
                            save = true;
                        } else {
                            save = (response_buf[0] == 'y' || response_buf[0] == 'Y');
                        }
                        decision_made = true;
                    } else if (ch == '\033') {
                        char seq[2];
                        read(tty_fd, seq, 2);
                    } else if (ch == ' ') {
                    } else {
                        if (buf_idx < 3) {
                            response_buf[buf_idx++] = ch;
                            if (buf_idx == 1 && (ch == 'n' || ch == 'N')) {
                                save = false;
                                decision_made = true;
                            }
                            if (buf_idx == 1 && (ch == 'y' || ch == 'Y')) {
                                save = true;
                                decision_made = true;
                            }
                        }
                    }
                } else if (r == 0) {
                    break;
                } else if (r == -1) {
                    if (errno == EINTR) continue;
                    Logger::error("[UI] Read error: %s", strerror(errno));
                    break;
                }
            }
        }
    }

    if (decision_made) {
        const char* res_str = save ? "y\n" : "n\n";
        write(tty_fd, res_str, 2);
    }

    tcsetattr(tty_fd, TCSANOW, &orig_tio);
    close(tty_fd);

    Logger::debug("[UI] Save decision: %s", save ? "YES" : "NO");
    return save;
}
