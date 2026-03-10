#include "output_formatter.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "loader.hpp"
#include "common.hpp"
#include <iostream>
#include <algorithm>

static Loader* g_loader = nullptr;

void set_loader_instance(Loader* loader) {
    g_loader = loader;
    set_common_loader(loader);
}

FormatContext::FormatContext() {
    Logger::debug("[OF] FormatContext created");
}

FormatContext::~FormatContext() {
    Logger::debug("[OF] FormatContext destroyed");
}

static bool is_likely_language(const std::string& str) {
    if (str.empty() || str.size() > 20) return false;
    for (char c : str) {
        if (!isalnum(c) && c != '-' && c != '_' && c != '+') return false;
    }
    return true;
}

static std::string debug_ansi(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '\x1b') result += "\\e";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result;
}

static void clear_ghost_text(FormatContext& ctx) {
    if (!ctx.ghost_active) {
        Logger::debug("[GHOST] clear_ghost_text: not active, skipping");
        return;
    }

    Logger::debug("[GHOST] >>> CLEARING ghost text: lines=%zu, length=%zu", 
                  ctx.ghost_lines, ctx.ghost_length);

    if (ctx.ghost_lines > 1) {
        Logger::debug("[GHOST] >>> Moving cursor up %zu lines: \\033[%zuF", 
                     ctx.ghost_lines - 1, ctx.ghost_lines - 1);
        std::cout << "\033[" << (ctx.ghost_lines - 1) << "F";
    } else {
        Logger::debug("[GHOST] >>> Moving to beginning of line: \\r");
        std::cout << "\r";
    }

    std::cout << std::flush;
    Logger::debug("[GHOST] Ghost text cleared");

    ctx.ghost_active = false;
    ctx.ghost_lines = 0;
    ctx.ghost_length = 0;
    ctx.last_rendered_length = 0;
    ctx.ghost_text_length = 0;
}

static void show_ghost_text(const std::string& text, FormatContext& ctx) {
    if (text.empty()) return;

    size_t term_width = get_terminal_width();

    if (ctx.ghost_active && ctx.last_term_width != term_width) {
        Logger::debug("[GHOST] Terminal width changed, clearing ghost");
        clear_ghost_text(ctx);
    }

    size_t text_width = calculate_display_width_no_ansi(text);
    size_t lines_needed = (text_width + term_width - 1) / term_width;
    if (lines_needed == 0) lines_needed = 1;

    if (ctx.ghost_active) {
        Logger::debug("[GHOST] Clearing existing ghost before showing new");
        clear_ghost_text(ctx);
    }

    Logger::debug("[GHOST] >>> SHOWING ghost: %zu bytes, %zu lines", text.size(), lines_needed);
    Logger::debug("[GHOST] Ghost content: '%s'", debug_ansi(text).c_str());
    std::cout << text << std::flush;

    ctx.ghost_active = true;
    ctx.ghost_lines = lines_needed;
    ctx.last_term_width = term_width;
    ctx.ghost_length = text_width % term_width;
    ctx.ghost_text_length = text_width;
}

void process_format_buffer(const std::string& input,
                          FormatContext& ctx,
                          const std::string& theme,
                          bool is_final) {
    Logger::debug("[OF] ========== process_format_buffer START ==========");
    Logger::debug("[OF] input_len=%zu, is_final=%d, buffer_size=%zu",
                  input.size(), is_final, ctx.parse_buffer.size());
    Logger::debug("[OF] cb_state: type=%d, fence_indent=%zu, at_line_start=%d",
                  ctx.cb_state.type, ctx.cb_state.fence_indent, ctx.cb_state.at_line_start);
    Logger::debug("[OF] ghost_active=%d, ghost_lines=%zu, last_rendered_length=%zu",
                  ctx.ghost_active, ctx.ghost_lines, ctx.last_rendered_length);
    
    ctx.parse_buffer += expand_tabs(input);
    
    bool preview_enabled = Config::instance().preview_enabled();
    bool md_enabled = Config::instance().markdown_enabled();
    Logger::debug("[OF] md_enabled=%d, preview_enabled=%d", md_enabled, preview_enabled);

    if (preview_enabled && ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK) {
        size_t last_newline = ctx.parse_buffer.rfind('\n');
        std::string current_line = (last_newline == std::string::npos) ? 
                                   ctx.parse_buffer : 
                                   ctx.parse_buffer.substr(last_newline + 1);
        
        if (!current_line.empty()) {
            size_t hash = std::hash<std::string>{}(current_line);
            if (hash != ctx.ghost_hash) {
                Logger::debug("[OF] >>> SHOWING GHOST TEXT: %zu bytes", current_line.size());
                Logger::debug("[OF] Ghost content: '%s'", current_line.c_str());
                show_ghost_text(current_line, ctx);
                ctx.ghost_hash = hash;
                
                if (g_loader) {
                    g_loader->update_color();
                }
            }
        }
    }
    
    bool made_progress = false;

    do {
        made_progress = false;
        auto result = CodeBlockParser::parse_next(ctx.parse_buffer, 0, ctx.cb_state, is_final);

        Logger::debug("[OF] Parse result: type=%d, advance=%zu, lang='%s'",
                     result.type, result.advance_by, result.language.c_str());

        switch (result.type) {
            case CodeBlockParser::ParseResult::OPEN_FENCE_COMPLETE: {
                Logger::debug("[OF] >>> OPEN_FENCE_COMPLETE: lang='%s', indent=%zu",
                             result.language.c_str(), ctx.cb_state.fence_indent);
                Logger::debug("[OF] Current cursor state: ghost_active=%d, ghost_lines=%zu",
                             ctx.ghost_active, ctx.ghost_lines);
                
                if (ctx.ghost_active) {
                    Logger::debug("[OF] >>> CLEARING ghost text (%zu lines)", ctx.ghost_lines);
                    clear_ghost_text(ctx);
                    ctx.last_rendered_length = 0;
                }
                
                if (md_enabled && ctx.md_highlighter.is_active()) {
                    Logger::debug("[OF] >>> CLEARING md_highlighter ghost");
                    ctx.md_highlighter.clear_ghost();
                }
                
                if (ctx.last_rendered_length > 0) {
                    Logger::debug("[OF] >>> CLEARING residue: %zu cols", ctx.last_rendered_length);
                    ctx.md_highlighter.clear_residue(ctx.last_rendered_length);
                    ctx.last_rendered_length = 0;
                }
                
                if (md_enabled && ctx.md_highlighter.is_active()) {
                    Logger::debug("[OF] >>> ENDING md_highlighter");
                    std::string remaining = ctx.md_highlighter.end();
                    Logger::debug("[OF] md_highlighter.end() returned %zu bytes: '%s'", 
                                 remaining.size(), debug_ansi(remaining).c_str());
                    if (!remaining.empty()) {
                        Logger::debug("[OF] >>> OUTPUTTING md_highlighter remaining");
                        std::cout << remaining << std::flush;
                    }
                }
                
                if (md_enabled && !ctx.md_buffer.empty()) {
                    Logger::debug("[OF] >>> FLUSHING md_buffer: %zu bytes", ctx.md_buffer.size());
                    std::cout << ctx.md_buffer << std::flush;
                    ctx.md_buffer.clear();
                }

                ctx.code_lang = result.language;
                std::string fence_line = "```" + ctx.code_lang + "\n";
                Logger::debug("[OF] >>> OUTPUTTING fence: '%s'", debug_ansi(fence_line).c_str());
                std::cout << fence_line << std::flush;
                ctx.cb_state.type = CodeBlockParser::State::IN_BLOCK;
                
                ctx.highlighter.set_simple_mode(false);
                if (!ctx.code_lang.empty() && CodeBlockParser::is_language_supported(ctx.code_lang)) {
                    Logger::debug("[OF] >>> STARTING FORKED highlighter for lang='%s'", ctx.code_lang.c_str());
                    ctx.highlighter.start(ctx.code_lang, theme);
                    Logger::debug("[OF] highlighter.is_active()=%d", ctx.highlighter.is_active());
                } else {
                    Logger::debug("[OF] Raw code block (no lang or unsupported): lang='%s'", 
                                 ctx.code_lang.c_str());
                }
                ctx.is_first_chunk = true;
                ctx.parse_buffer.erase(0, result.advance_by);
                made_progress = true;
                break;
            }

            case CodeBlockParser::ParseResult::TEXT_CHUNK: {
                Logger::debug("[OF] >>> TEXT_CHUNK: %zu bytes, in_block=%d, at_line_start=%d",
                             result.content.size(), 
                             ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK,
                             ctx.cb_state.at_line_start);
                Logger::debug("[OF] Content: '%s'", debug_ansi(result.content.substr(0, std::min(result.content.size(), (size_t)50))).c_str());
                
                if (ctx.ghost_active && ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK) {
                    Logger::debug("[OF] >>> CLEARING ghost before codeblock text");
                    clear_ghost_text(ctx);
                    ctx.last_rendered_length = 0;
                }
                
                if (ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK) {
                    if (ctx.highlighter.is_active()) {
                        Logger::debug("[OF] >>> FEEDING %zu bytes to highlighter", result.content.size());
                        Logger::debug("[OF] Highlighter state: is_active=%d", ctx.highlighter.is_active());
                        std::string processed = ctx.highlighter.feed(result.content);
                        Logger::debug("[OF] <<< Highlighter returned %zu bytes", processed.size());
                        Logger::debug("[OF] Highlighted output: '%s'", debug_ansi(processed.substr(0, std::min(processed.size(), (size_t)50))).c_str());
                        if (!processed.empty()) {
                            Logger::debug("[OF] >>> OUTPUTTING highlighted text (%zu bytes)", processed.size());
                            std::cout << processed << std::flush;
                        }
                    } else {
                        Logger::debug("[OF] >>> RAW code block output: %zu bytes", result.content.size());
                        std::cout << result.content << std::flush;
                    }
                } else {
                    if (md_enabled) {
                        Logger::debug("[OF] >>> Markdown mode, using simple highlighter");
                        if (!ctx.md_highlighter.is_active()) {
                            ctx.md_highlighter.set_simple_mode(true);
                            ctx.md_highlighter.start("txt", theme);
                        }

                        std::string processed = ctx.md_highlighter.feed(result.content);
                        Logger::debug("[OF] <<< md_highlighter returned %zu bytes", processed.size());

                        if (!processed.empty()) {
                            Logger::debug("[OF] >>> OUTPUTTING md_highlighted text");
                            std::cout << processed << std::flush;

                            size_t rendered_width = calculate_display_width_no_ansi(processed);
                            Logger::debug("[OF] rendered_width=%zu, term_width=%zu", 
                                         rendered_width, get_terminal_width());
                            
                            ctx.md_highlighter.clear_residue(rendered_width);
                        }

                        ctx.last_rendered_length = calculate_display_width_no_ansi(processed);
                        Logger::debug("[OF] last_rendered_length updated to %zu", ctx.last_rendered_length);
                    } else {
                        Logger::debug("[OF] >>> Raw output (md disabled): %zu bytes", result.content.size());
                        std::cout << result.content << std::flush;
                        ctx.last_rendered_length = calculate_display_width_no_ansi(result.content);
                    }
                }
                ctx.parse_buffer.erase(0, result.advance_by);
                made_progress = true;
                break;
            }

            case CodeBlockParser::ParseResult::CLOSE_FENCE: {
                Logger::debug("[OF] >>> CLOSE_FENCE: advance=%zu, new_lang='%s'",
                             result.advance_by, result.language.c_str());
                
                if (ctx.ghost_active) {
                    Logger::debug("[OF] >>> CLEARING ghost before close fence");
                    clear_ghost_text(ctx);
                    ctx.last_rendered_length = 0;
                }
                
                if (ctx.highlighter.is_active()) {
                    Logger::debug("[OF] >>> ENDING code highlighter");
                    std::string remaining = ctx.highlighter.end();
                    Logger::debug("[OF] Highlighter end returned %zu bytes", remaining.size());
                    Logger::debug("[OF] Remaining output: '%s'", debug_ansi(remaining.substr(0, std::min(remaining.size(), (size_t)50))).c_str());
                    if (!remaining.empty()) {
                        Logger::debug("[OF] >>> OUTPUTTING remaining highlighted text");
                        std::cout << remaining << std::flush;
                    }
                }
                
                Logger::debug("[OF] >>> OUTPUTTING cursor move: \\033[1F (up one line)");
                std::cout << "\033[1F";
                Logger::debug("[OF] >>> OUTPUTTING fence close: '```\\033[3D'");
                std::cout << "```\033[3D" << std::flush;
                
                if (!result.language.empty()) {
                    Logger::debug("[OF] >>> NESTED codeblock: lang='%s'", result.language.c_str());
                    ctx.code_lang = result.language;
                    std::string nested_fence = "```" + ctx.code_lang + "\n";
                    Logger::debug("[OF] >>> OUTPUTTING nested fence: '%s'", debug_ansi(nested_fence).c_str());
                    std::cout << nested_fence << std::flush;
                    ctx.cb_state.type = CodeBlockParser::State::IN_BLOCK;
                    ctx.highlighter.set_simple_mode(false);
                    if (CodeBlockParser::is_language_supported(ctx.code_lang)) {
                        ctx.highlighter.start(ctx.code_lang, theme);
                    }
                    ctx.is_first_chunk = true;
                } else {
                    ctx.cb_state.type = CodeBlockParser::State::NONE;
                    ctx.cb_state.fence_indent = 0;
                    ctx.code_lang.clear();
                    ctx.is_first_chunk = false;

                    if (md_enabled) {
                        Logger::debug("[OF] >>> Restarting md_highlighter");
                        ctx.md_highlighter.set_simple_mode(true);
                        ctx.md_highlighter.start("txt", theme);
                    }
                }

                ctx.parse_buffer.erase(0, result.advance_by);
                made_progress = true;
                break;
            }

            case CodeBlockParser::ParseResult::NEED_MORE_DATA:
                Logger::debug("[OF] >>> NEED_MORE_DATA");
                break;
        }
    } while (made_progress && !ctx.parse_buffer.empty());

    if (is_final) {
        Logger::debug("[OF] ========== FINAL FLUSH ==========");
        Logger::debug("[OF] buffer_size=%zu, in_block=%d",
                     ctx.parse_buffer.size(), ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK);
        Logger::debug("[OF] ghost_active=%d, ghost_lines=%zu", ctx.ghost_active, ctx.ghost_lines);
        
        if (ctx.ghost_active) {
            Logger::debug("[OF] >>> CLEARING ghost (final)");
            clear_ghost_text(ctx);
            ctx.last_rendered_length = 0;
        }
        
        if (!ctx.parse_buffer.empty()) {
            Logger::debug("[OF] >>> FLUSHING remaining buffer: %zu bytes", ctx.parse_buffer.size());
            if (ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK) {
                if (ctx.highlighter.is_active()) {
                    std::string processed = ctx.highlighter.feed(ctx.parse_buffer);
                    Logger::debug("[OF] Highlighter returned %zu bytes", processed.size());
                    if (!processed.empty()) {
                        Logger::debug("[OF] >>> OUTPUTTING remaining highlighted");
                        std::cout << processed << std::flush;
                    }
                } else {
                    Logger::debug("[OF] >>> OUTPUTTING remaining raw");
                    std::cout << ctx.parse_buffer << std::flush;
                }
            } else {
                if (md_enabled && ctx.md_highlighter.is_active()) {
                    std::string processed = ctx.md_highlighter.feed(ctx.parse_buffer);
                    Logger::debug("[OF] >>> OUTPUTTING remaining md_highlighted");
                    std::cout << processed << std::flush;
                } else {
                    Logger::debug("[OF] >>> OUTPUTTING remaining raw");
                    std::cout << ctx.parse_buffer << std::flush;
                }
            }
            ctx.parse_buffer.clear();
        }

        if (ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK) {
            Logger::debug("[OF] >>> FORCE CLOSING unclosed codeblock");
            if (ctx.highlighter.is_active()) {
                std::string remaining = ctx.highlighter.end();
                Logger::debug("[OF] Highlighter end returned %zu bytes", remaining.size());
                if (!remaining.empty()) {
                    Logger::debug("[OF] >>> OUTPUTTING highlighter remaining");
                    std::cout << remaining << std::flush;
                }
            }
            Logger::debug("[OF] >>> OUTPUTTING closing fence: '```\\n'");
            std::cout << "```\n" << std::flush;
            ctx.cb_state.type = CodeBlockParser::State::NONE;
            ctx.cb_state.fence_indent = 0;
        }

        if (md_enabled && ctx.md_highlighter.is_active()) {
            Logger::debug("[OF] >>> ENDING md_highlighter (final)");
            std::string remaining = ctx.md_highlighter.end();
            if (!remaining.empty()) {
                Logger::debug("[OF] md_highlighter.end() returned %zu bytes", remaining.size());
                Logger::debug("[OF] >>> OUTPUTTING md_highlighter remaining");
                std::cout << remaining << std::flush;
            }
        }
    }
    
    Logger::debug("[OF] ========== process_format_buffer END ==========");
    Logger::debug("[OF] Final state: ghost_active=%d, ghost_lines=%zu, last_rendered_length=%zu",
                  ctx.ghost_active, ctx.ghost_lines, ctx.last_rendered_length);
}
