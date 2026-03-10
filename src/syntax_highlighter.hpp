#ifndef SYNTAX_HIGHLIGHTER_HPP
#define SYNTAX_HIGHLIGHTER_HPP

#include <string>
#include <mutex>

class Loader;

void set_loader_for_highlighter(Loader* loader);

class SyntaxHighlighter {
public:
    class StreamingHighlighter {
    public:
        ~StreamingHighlighter() { end(); }

        void start(const std::string& lang, const std::string& theme);
        std::string feed(const std::string& code);
        std::string end();
        void reset();
        bool is_active() const { return child_pid != -1 || simple_mode_; }
        
        void set_simple_mode(bool val) { simple_mode_ = val; }
        static std::string apply_simple_ansi(const std::string& text);
        void clear_residue(size_t rendered_width);
        void clear_ghost();

    private:
        int write_fd = -1;
        int read_fd  = -1;
        pid_t child_pid = -1;

        std::string theme_;
        std::string lang_;
        
        bool simple_mode_ = false;
        bool preview_active_ = false;
        size_t lines_processed_ = 0;

        std::string line_buffer_;
        mutable std::mutex mutex_;

        bool ghost_active_ = false;
        size_t ghost_hash_ = 0;
        size_t ghost_length_ = 0;
        size_t last_term_width_ = 0;
        size_t spinner_idx_ = 0;
        size_t ghost_lines_ = 1;
        size_t ghost_text_width_ = 0;
        std::string last_ghost_text_;

        bool highlight_dead_ = false;

        size_t get_terminal_width();
        void show_ghost(const std::string& text, bool is_update = false);
    };
};

#endif
