#ifndef OUTPUT_FORMATTER_HPP
#define OUTPUT_FORMATTER_HPP

#include "codeblock_parser.hpp"
#include "syntax_highlighter.hpp"
#include <string>
#include <vector>

// Forward declaration
class Loader;

// Set global loader instance for preview overflow color cycling
void set_loader_instance(Loader* loader);

struct FormatContext {
    CodeBlockParser::State cb_state;
    std::string code_lang;
    std::string parse_buffer;
    SyntaxHighlighter::StreamingHighlighter highlighter;  // Current code block highlighter
    SyntaxHighlighter::StreamingHighlighter md_highlighter;  // Markdown text (simple mode)
    bool is_first_chunk = true;
    std::string md_buffer;
    
    // Stack of enclosing codeblock languages. When a nested block ends we
    // restart the highlighter for the language on the top of this stack.
    std::vector<std::string> code_block_stack;
    
    // Ghost text state for preview mode
    bool ghost_active = false;
    size_t ghost_length = 0;
    size_t ghost_hash = 0;
    size_t ghost_lines = 1;
    size_t last_term_width = 0;
    
    // Track display width of last rendered output (for clearing artifacts)
    size_t last_rendered_length = 0;
    
    // Track ghost text length for residue clearing when --md is enabled
    size_t ghost_text_length = 0;

    FormatContext();
    ~FormatContext();
};

void process_format_buffer(const std::string& input,
                          FormatContext& ctx,
                          const std::string& theme,
                          bool is_final);

#endif
