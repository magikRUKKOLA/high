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

static void clear_ghost_text(FormatContext& ctx) {
    if (!ctx.ghost_active) return;

    if (ctx.ghost_lines > 1) {
        std::cout << "\033[" << (ctx.ghost_lines - 1) << "F";
    } else {
        std::cout << "\r";
    }

    std::cout << std::flush;

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
        clear_ghost_text(ctx);
    }

    size_t text_width = calculate_display_width_no_ansi(text);
    size_t lines_needed = (text_width + term_width - 1) / term_width;
    if (lines_needed == 0) lines_needed = 1;

    if (ctx.ghost_active) {
        clear_ghost_text(ctx);
    }

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
    Logger::debug("[OF] process_format_buffer: input_len=%zu, is_final=%d, buffer_size=%zu",
                  input.size(), is_final, ctx.parse_buffer.size());
    
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
                Logger::debug("[OF] Ghost text for incomplete line: %zu bytes", current_line.size());
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
                Logger::debug("[OF] OPEN_FENCE_COMPLETE: lang='%s', indent=%zu",
                             result.language.c_str(), ctx.cb_state.fence_indent);
                
                if (ctx.ghost_active) {
                    clear_ghost_text(ctx);
                    ctx.last_rendered_length = 0;
                }
                
                if (md_enabled && !ctx.md_buffer.empty()) {
                    Logger::debug("[OF] Flushing md_buffer: %zu bytes", ctx.md_buffer.size());
                    std::cout << ctx.md_buffer << std::flush;
                    ctx.md_buffer.clear();
                }

                if (md_enabled && ctx.md_highlighter.is_active()) {
                    Logger::debug("[OF] Ending md_highlighter (final)");
                    std::string remaining = ctx.md_highlighter.end();
                    if (!remaining.empty()) {
                        std::cout << remaining << std::flush;
                    }
                }

                ctx.code_lang = result.language;
                std::cout << "```" << ctx.code_lang << "\n" << std::flush;
                ctx.cb_state.type = CodeBlockParser::State::IN_BLOCK;
                
                ctx.highlighter.set_simple_mode(false);
                if (!ctx.code_lang.empty() && CodeBlockParser::is_language_supported(ctx.code_lang)) {
                    Logger::debug("[OF] Starting FORKED highlighter for lang='%s'", ctx.code_lang.c_str());
                    ctx.highlighter.start(ctx.code_lang, theme);
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
                Logger::debug("[OF] TEXT_CHUNK: %zu bytes, in_block=%d, at_line_start=%d",
                             result.content.size(), 
                             ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK,
                             ctx.cb_state.at_line_start);
                
                if (ctx.ghost_active && ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK) {
                    clear_ghost_text(ctx);
                    ctx.last_rendered_length = 0;
                }
                
                if (ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK &&
                    !ctx.highlighter.is_active() && ctx.code_lang.empty() &&
                    ctx.is_first_chunk) {
                    size_t nl = result.content.find('\n');
                    if (nl != std::string::npos || is_final) {
                        std::string first = (nl != std::string::npos) ?
                                          result.content.substr(0, nl) : result.content;
                        if (!first.empty() && is_likely_language(first)) {
                            Logger::debug("[OF] Detected language: '%s'", first.c_str());
                            ctx.code_lang = first;
                            if (CodeBlockParser::is_language_supported(ctx.code_lang)) {
                                ctx.highlighter.set_simple_mode(false);
                                ctx.highlighter.start(ctx.code_lang, theme);
                                size_t skip = (nl != std::string::npos) ? nl + 1 : result.advance_by;
                                ctx.parse_buffer.erase(0, skip);
                                result.content = result.content.substr(skip);
                                result.advance_by = (result.advance_by > skip) ? result.advance_by - skip : 0;
                            }
                        }
                    }
                    ctx.is_first_chunk = false;
                } else if (ctx.is_first_chunk) {
                    ctx.is_first_chunk = false;
                }

                if (ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK) {
                    if (ctx.highlighter.is_active()) {
                        Logger::debug("[OF] Feeding %zu bytes to highlighter", result.content.size());
                        std::string processed = ctx.highlighter.feed(result.content);
                        Logger::debug("[OF] Highlighter returned %zu bytes", processed.size());
                        std::cout << processed << std::flush;
                    } else {
                        Logger::debug("[OF] Raw code block output: %zu bytes", result.content.size());
                        std::cout << result.content << std::flush;
                    }
                } else {
                    if (md_enabled) {
                        Logger::debug("[OF] Markdown mode, using simple highlighter");
                        if (!ctx.md_highlighter.is_active()) {
                            ctx.md_highlighter.set_simple_mode(true);
                            ctx.md_highlighter.start("txt", theme);
                        }

                        std::string processed = ctx.md_highlighter.feed(result.content);
                        Logger::debug("[OF] md_highlighter returned %zu bytes", processed.size());

                        if (!processed.empty()) {
                            std::cout << processed << std::flush;

                            size_t rendered_width = calculate_display_width_no_ansi(processed);
                            if (rendered_width > 0 && rendered_width < 1000) {
                                size_t term_width = get_terminal_width();
                                size_t cols_used = rendered_width % term_width;
                                if (cols_used < term_width) {
                                }
                            }
                            
                            // FIX: Clear residue after markdown output (was missing)
                            ctx.md_highlighter.clear_residue(rendered_width);
                        }

                        ctx.last_rendered_length = calculate_display_width_no_ansi(processed);
                        Logger::debug("[OF] last_rendered_length updated to %zu", ctx.last_rendered_length);
                    } else {
                        Logger::debug("[OF] Raw output (md disabled): %zu bytes", result.content.size());
                        std::cout << result.content << std::flush;
                        ctx.last_rendered_length = calculate_display_width_no_ansi(result.content);
                    }
                }
                ctx.parse_buffer.erase(0, result.advance_by);
                made_progress = true;
                break;
            }

            case CodeBlockParser::ParseResult::CLOSE_FENCE: {
                Logger::debug("[OF] CLOSE_FENCE: advance=%zu, new_lang='%s'",
                             result.advance_by, result.language.c_str());
                
                if (ctx.ghost_active) {
                    clear_ghost_text(ctx);
                    ctx.last_rendered_length = 0;
                }
                
                if (ctx.highlighter.is_active()) {
                    Logger::debug("[OF] Ending code highlighter");
                    std::string remaining = ctx.highlighter.end();
                    Logger::debug("[OF] Highlighter end returned %zu bytes", remaining.size());
                    std::cout << remaining << std::flush;
                }
                //std::cout << "\033[1A\r";
                std::cout << "\033[1F";
                //std::cout << "```\n" << std::flush;
                std::cout << "```\033[3D" << std::flush;
                
                if (!result.language.empty()) {
                    Logger::debug("[OF] Nested codeblock: lang='%s'", result.language.c_str());
                    ctx.code_lang = result.language;
                    std::cout << "```" << ctx.code_lang << "\n" << std::flush;
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
                        Logger::debug("[OF] Restarting md_highlighter");
                        ctx.md_highlighter.set_simple_mode(true);
                        ctx.md_highlighter.start("txt", theme);
                    }
                }

                ctx.parse_buffer.erase(0, result.advance_by);
                made_progress = true;
                break;
            }

            case CodeBlockParser::ParseResult::NEED_MORE_DATA:
                Logger::debug("[OF] NEED_MORE_DATA");
                break;
        }
    } while (made_progress && !ctx.parse_buffer.empty());

    if (is_final) {
        Logger::debug("[OF] FINAL flush, buffer_size=%zu, in_block=%d",
                     ctx.parse_buffer.size(), ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK);
        
        if (ctx.ghost_active) {
            clear_ghost_text(ctx);
            ctx.last_rendered_length = 0;
        }
        
        if (!ctx.parse_buffer.empty()) {
            if (ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK) {
                if (ctx.highlighter.is_active()) {
                    std::string processed = ctx.highlighter.feed(ctx.parse_buffer);
                    std::cout << processed << std::flush;
                } else {
                    std::cout << ctx.parse_buffer << std::flush;
                }
            } else {
                if (md_enabled && ctx.md_highlighter.is_active()) {
                    std::string processed = ctx.md_highlighter.feed(ctx.parse_buffer);
                    std::cout << processed << std::flush;
                } else {
                    std::cout << ctx.parse_buffer << std::flush;
                }
            }
            ctx.parse_buffer.clear();
        }

        if (ctx.cb_state.type == CodeBlockParser::State::IN_BLOCK) {
            Logger::debug("[OF] Force closing unclosed codeblock");
            if (ctx.highlighter.is_active()) {
                std::string remaining = ctx.highlighter.end();
                std::cout << remaining << std::flush;
            }
            std::cout << "```\n" << std::flush;
            ctx.cb_state.type = CodeBlockParser::State::NONE;
            ctx.cb_state.fence_indent = 0;
        }

        if (md_enabled && ctx.md_highlighter.is_active()) {
            Logger::debug("[OF] Ending md_highlighter (final)");
            std::string remaining = ctx.md_highlighter.end();
            if (!remaining.empty()) {
                std::cout << remaining << std::flush;
            }
        }
    }
}
