#include "banner.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <unistd.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdlib>

namespace ANSI {
    constexpr const char* RESET = "\033[0m";
    constexpr const char* HIDE_CURSOR = "\033[?25l";
    constexpr const char* SHOW_CURSOR = "\033[?25h";
    constexpr const char* CLEAR_SCREEN = "\033[2J";
    constexpr const char* HOME = "\033[H";
    constexpr const char* ALT_BUFFER = "\033[?1049h";
    constexpr const char* NORM_BUFFER = "\033[?1049l";
    
    inline std::string rgb_fg(int r, int g, int b) {
        return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
    }
    
    inline std::string rgb_bg(int r, int g, int b) {
        return "\033[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
    }
}

struct Color {
    int r, g, b;
    constexpr Color(int r=0, int g=0, int b=0) : r(r), g(g), b(b) {}
    
    Color lerp(const Color& o, float t) const {
        t = std::clamp(t, 0.0f, 1.0f);
        return Color(r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t);
    }
    
    Color multiply(float s) const {
        return Color(std::clamp(r * s, 0.0f, 255.0f), std::clamp(g * s, 0.0f, 255.0f), std::clamp(b * s, 0.0f, 255.0f));
    }
    
    static Color hsv(float h, float s, float v) {
        int i = (int)(h * 6); float f = h * 6 - i;
        float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
        switch(i % 6) {
            case 0: return Color(v*255, t*255, p*255);
            case 1: return Color(q*255, v*255, p*255);
            case 2: return Color(p*255, v*255, t*255);
            case 3: return Color(p*255, q*255, v*255);
            case 4: return Color(t*255, p*255, v*255);
            case 5: return Color(v*255, p*255, q*255);
        }
        return Color(0, 0, 0);
    }
};

namespace Theme {
    const Color BG_DARK(5, 5, 12), BG_DARKER(2, 2, 8);
    const Color NEON_GREEN(50, 255, 150), NEON_GREEN_BRIGHT(100, 255, 180), NEON_GREEN_DIM(30, 180, 100);
    const Color CYAN_BRIGHT(100, 255, 255);
    const Color PURPLE_BRIGHT(200, 100, 255);
    const Color YELLOW(255, 255, 100);
    const Color HALO_WHITE(200, 200, 255);
}

struct TerminalSize {
    int rows, cols;
    static TerminalSize get() {
        struct winsize ws;
        return (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) ? TerminalSize{ws.ws_row, ws.ws_col} : TerminalSize{24, 80};
    }
};

struct Pixel {
    char ch = ' ';
    Color color = Theme::BG_DARK, bg = Theme::BG_DARKER;
    bool dirty = true;
    float brightness = 1.0f;
    bool has_halo = false;
};

class Framebuffer {
public:
    int width, height;
    std::vector<std::vector<Pixel>> pixels, prev_pixels;
    
    Framebuffer(int w, int h) : width(w), height(h) {
        pixels.resize(h, std::vector<Pixel>(w));
        prev_pixels.resize(h, std::vector<Pixel>(w));
        clear();
    }
    
    void clear() {
        for (auto& row : pixels)
            for (auto& p : row)
                p = Pixel(), p.color = Theme::BG_DARK, p.bg = Theme::BG_DARKER, p.ch = ' ', p.brightness = 1.0f, p.has_halo = false;
    }
    
    void swap_buffers() { prev_pixels = pixels; }
    
    void set_pixel(int x, int y, char ch, const Color& fg, const Color& bg = Theme::BG_DARKER, float brightness = 1.0f) {
        if (x >= 0 && x < width && y >= 0 && y < height) {
            auto& p = pixels[y][x];
            p.ch = ch; p.color = fg.multiply(brightness); p.bg = bg; p.brightness = brightness; p.dirty = true;
        }
    }
    
    void blend_pixel(int x, int y, char ch, const Color& fg, float alpha) {
        if (x >= 0 && x < width && y >= 0 && y < height) {
            auto& p = pixels[y][x];
            p.ch = ch; p.color = p.color.lerp(fg, alpha); p.dirty = true;
        }
    }
    
    void draw_circle(int cx, int cy, int radius, const Color& color, float opacity = 1.0f, const char* ch_str = "o", float thickness = 1.0f) {
        char ch = ch_str[0];
        for (int y = std::max(0, cy - radius - 2); y <= std::min(height-1, cy + radius + 2); y++)
            for (int x = std::max(0, cx - radius - 2); x <= std::min(width-1, cx + radius + 2); x++) {
                float dx = x - cx, dy = (y - cy) * 2.0f, dist = std::sqrt(dx*dx + dy*dy);
                if (dist >= radius - thickness && dist <= radius + thickness)
                    blend_pixel(x, y, ch, color, opacity * (1.0f - std::abs(dist - radius) / thickness));
            }
    }
    
    void draw_multi_wave(int start_x, int end_x, int base_y, float phase, const Color& color, float opacity = 1.0f, int num_waves = 3, float wave_spacing = 2) {
        const char* chars[] = {"-", ".", ":"};
        for (int w = 0; w < num_waves; w++) {
            float wave_phase = phase + w * 0.5f, wave_amplitude = 2.0f - w * 0.5f, wave_freq = 1.0f + w * 0.3f;
            int wave_y = base_y + (int)((w - num_waves/2.0f) * wave_spacing);
            Color wave_color = color.lerp(Theme::CYAN_BRIGHT, (float)w / num_waves);
            for (int x = start_x; x <= end_x && x < width; x++) {
                float t = (float)(x - start_x) / std::max(1, end_x - start_x);
                float y_offset = std::sin(t * 3.14159f * 2.0f * wave_freq + wave_phase)
                               + std::sin(t * 3.14159f * 4.0f + wave_phase * 1.5f) * 0.5f
                               + std::sin(t * 3.14159f * 6.0f + wave_phase * 0.5f) * 0.25f;
                int y = wave_y + (int)(y_offset * wave_amplitude);
                if (y >= 0 && y < height)
                    blend_pixel(x, y, chars[w % 3][0], wave_color, opacity * (0.3f + 0.7f * std::sin(t * 3.14159f + phase)) * (1.0f - w * 0.2f));
            }
        }
    }
    
    void draw_line(int x1, int y1, int x2, int y2, const Color& color, const char* ch_str = "|", float opacity = 1.0f) {
        char ch = ch_str[0];
        int dx = std::abs(x2 - x1), dy = std::abs(y2 - y1), sx = (x1 < x2) ? 1 : -1, sy = (y1 < y2) ? 1 : -1, err = dx - dy;
        while (true) {
            blend_pixel(x1, y1, ch, color, opacity);
            if (x1 == x2 && y1 == y2) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x1 += sx; }
            if (e2 < dx) { err += dx; y1 += sy; }
        }
    }
    
    void draw_glow(int cx, int cy, int radius, const Color& color, float intensity = 0.3f) {
        for (int r = radius; r > 0; r--)
            draw_circle(cx, cy, r, color, intensity * (1.0f - (float)r / radius), " ", 1.5f);
    }
    
    void draw_grid(int start_x, int start_y, int width, int height, int cell_size, const Color& color, float opacity = 0.15f) {
        for (int x = start_x; x <= start_x + width; x += cell_size)
            draw_line(x, start_y, x, start_y + height, color, "|", opacity * 0.5f);
        for (int y = start_y; y <= start_y + height; y += cell_size)
            draw_line(start_x, y, start_x + width, y, color, "-", opacity * 0.5f);
    }
    
    void draw_sparkle(int cx, int cy, int size, const Color& color, float phase) {
        for (int i = 0; i < 8; i++) {
            float angle = phase + (2.0f * 3.14159f * i / 8);
            float dist = size * (0.5f + 0.5f * std::sin(phase * 3.0f + i));
            int x = cx + (int)(std::cos(angle) * dist), y = cy + (int)(std::sin(angle) * dist / 2.0f);
            float brightness = 0.5f + 0.5f * std::sin(phase * 5.0f + i * 0.5f);
            if (x >= 0 && x < width && y >= 0 && y < height)
                set_pixel(x, y, '*', color, Theme::BG_DARKER, brightness * 1.3f);
        }
        if (cx >= 0 && cx < width && cy >= 0 && cy < height)
            set_pixel(cx, cy, '+', color.multiply(1.5f), Theme::BG_DARKER, 1.3f);
    }
    
    void draw_hexagon(int cx, int cy, int size, const Color& color, float opacity = 0.5f) {
        for (int i = 0; i < 6; i++) {
            float angle1 = 2.0f * 3.14159f * i / 6, angle2 = 2.0f * 3.14159f * (i + 1) / 6;
            int x1 = cx + (int)(std::cos(angle1) * size), y1 = cy + (int)(std::sin(angle1) * size / 2.0f);
            int x2 = cx + (int)(std::cos(angle2) * size), y2 = cy + (int)(std::sin(angle2) * size / 2.0f);
            draw_line(x1, y1, x2, y2, color, "-", opacity);
        }
    }
    
    void render() {
        std::string output;
        output.reserve(width * height * 30);
        output += ANSI::HOME;
        Color last_fg = {-1, -1, -1}, last_bg = {-1, -1, -1};
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const Pixel& p = pixels[y][x];
                if (p.color.r != last_fg.r || p.color.g != last_fg.g || p.color.b != last_fg.b) {
                    output += ANSI::rgb_fg(p.color.r, p.color.g, p.color.b);
                    last_fg = p.color;
                }
                if (p.bg.r != last_bg.r || p.bg.g != last_bg.g || p.bg.b != last_bg.b) {
                    output += ANSI::rgb_bg(p.bg.r, p.bg.g, p.bg.b);
                    last_bg = p.bg;
                }
                output += p.ch;
            }
            if (y < height - 1) output += "\n";
        }
        output += ANSI::RESET;
        std::cout << output << std::flush;
        swap_buffers();
    }
};

struct SmoothPosition {
    float current = 0, target = 0, velocity = 0, min_change_interval = 0.08f, last_change_time = 0;
    bool initialized = false;
    
    void set_target(float new_target, float current_time) {
        if (!initialized) { current = target = new_target; initialized = true; return; }
        if (current_time - last_change_time >= min_change_interval) { target = new_target; last_change_time = current_time; }
    }
    void update(float dt) {
        if (!initialized) return;
        float diff = target - current;
        if (std::abs(diff) > 0.001f) {
            velocity += (diff * 0.1f - velocity * 0.85f) * dt * 60.0f;
            current += velocity * dt;
        }
    }
    float get() const { return current; }
    int get_int() const { return (int)(current + 0.5f); }
};

enum class ParticleType { SPARK, GLOW, STAR };

struct Particle {
    float x = 0, y = 0, vx = 0, vy = 0, life = 0, max_life = 0, size = 1.0f, rotation = 0;
    Color color;
    char ch = ' ';
    bool active = false;
    ParticleType type = ParticleType::SPARK;
    
    void spawn(float start_x, float start_y, const Color& col, ParticleType ptype = ParticleType::SPARK, char character = '*') {
        x = start_x; y = start_y;
        switch(ptype) {
            case ParticleType::SPARK: vx = ((float)(rand() % 200) - 100) / 50.0f; vy = ((float)(rand() % 200) - 100) / 50.0f; ch = character; size = 1.0f; break;
            case ParticleType::GLOW: vx = vy = 0; ch = ' '; size = 3.0f; break;
            case ParticleType::STAR: vx = vy = ((float)(rand() % 100) - 50) / 100.0f; ch = '*'; size = 1.0f; break;
        }
        life = 0; max_life = 0.5f + (float)(rand() % 100) / 100.0f;
        color = col; type = ptype; active = true;
        rotation = (float)(rand() % 360) / 360.0f * 2.0f * 3.14159f;
    }
    
    void update(float dt) {
        if (!active) return;
        x += vx * dt * 60.0f; y += vy * dt * 60.0f;
        if (type == ParticleType::SPARK || type == ParticleType::STAR) vy += 2.0f * dt;
        vx *= 0.98f; vy *= 0.98f;
        life += dt; rotation += dt * 2.0f;
        if (life >= max_life) active = false;
    }
    
    float get_alpha() const { return active ? pow(1.0f - (life / max_life), 2) : 0.0f; }
};

class ParticleSystem {
public:
    std::vector<Particle> particles;
    int max_particles;
    
    ParticleSystem(int max = 500) : max_particles(max) { particles.resize(max); }
    
    void emit(float x, float y, const Color& color, int count = 5, ParticleType type = ParticleType::SPARK, char ch = '*') {
        for (int i = 0; i < count && i < max_particles; i++)
            for (auto& p : particles)
                if (!p.active) { p.spawn(x, y, color, type, ch); break; }
    }
    
    void emit_burst(float x, float y, const Color& color, int count = 20) {
        for (int i = 0; i < count; i++) {
            float angle = (2.0f * 3.14159f * i) / count;
            float speed = 2.0f + (float)(rand() % 100) / 50.0f;
            for (auto& p : particles)
                if (!p.active) { p.spawn(x, y, color, ParticleType::SPARK, '*'); p.vx = std::cos(angle) * speed; p.vy = std::sin(angle) * speed; break; }
        }
    }
    
    void update(float dt) { for (auto& p : particles) p.update(dt); }
    
    void render(Framebuffer& fb) {
        for (const auto& p : particles)
            if (p.active && p.get_alpha() > 0.01f) {
                if (p.type == ParticleType::GLOW)
                    fb.draw_glow((int)p.x, (int)p.y, (int)(p.size * 3), p.color, p.get_alpha() * 0.3f);
                else
                    fb.blend_pixel((int)p.x, (int)p.y, p.ch, p.color, p.get_alpha() * p.size * 1.2f);
            }
    }
};

struct RotatingRing {
    int cx, cy, radius, segments;
    float angle = 0, speed, pulse_phase = 0;
    bool reverse;
    Color color;

    RotatingRing(int center_x, int center_y, int r, const Color& col, float spd = 0.5f, int segs = 12, bool rev = false)
        : cx(center_x), cy(center_y), radius(r), segments(segs), speed(spd), reverse(rev), color(col) {}

    void update(float dt) { angle += speed * dt * (reverse ? -1.0f : 1.0f); pulse_phase += dt * 2.0f; }

    void render(Framebuffer& fb) {
        const char* chars[] = {"+", "o"};
        for (int i = 0; i < segments; i++) {
            float segment_angle = angle + (2.0f * 3.14159f * i / segments);
            float pulse = 1.0f + 0.2f * std::sin(pulse_phase + i * 0.5f);
            int x = cx + (int)(std::cos(segment_angle) * radius * pulse);
            int y = cy + (int)(std::sin(segment_angle) * radius / 2.0f * pulse);
            float brightness = 0.7f + 0.3f * std::sin(angle * 3.0f + i + pulse_phase);
            fb.blend_pixel(x, y, chars[i % 2][0], color.multiply(brightness * 1.2f), brightness);
        }
    }
};

struct ElectricArc {
    int x1, y1, x2, y2; float life = 0, max_life = 0.3f; Color color; bool active = false;
    std::vector<std::pair<int,int>> points;
    
    void spawn(int sx, int sy, int ex, int ey, const Color& col) {
        x1 = sx; y1 = sy; x2 = ex; y2 = ey; color = col; active = true; life = 0; points.clear();
        points.push_back({x1, y1});
        int segments = 8; float dx = (float)(x2 - x1) / segments, dy = (float)(y2 - y1) / segments;
        for (int i = 1; i < segments; i++)
            points.push_back({(int)(x1 + dx * i + (rand() % 10) - 5), (int)(y1 + dy * i + (rand() % 10) - 5)});
        points.push_back({x2, y2});
    }
    void update(float dt) { life += dt; if (life >= max_life) active = false; }
    float get_alpha() const { return 1.0f - (life / max_life); }
    void render(Framebuffer& fb) {
        if (!active) return;
        float alpha = get_alpha();
        for (size_t i = 1; i < points.size(); i++)
            fb.draw_line(points[i-1].first, points[i-1].second, points[i].first, points[i].second, color, "/", alpha);
        fb.draw_glow(x1, y1, 3, color, alpha * 0.3f);
        fb.draw_glow(x2, y2, 3, color, alpha * 0.3f);
    }
};

void draw_exquisite_high_title(Framebuffer& fb, int cx, int cy, float time, SmoothPosition& title_y_pos) {
    const char* h_letter[] = {" #     ", " #     ", " ####. ", " #  *# ", " #   # ", "       ", "       "};
    const char* i_letter[] = {" # ", "   ", " # ", " # ", " # ", "   ", "   "};
    const char* g_letter[] = {"       ", "       ", " ,###. ", " #   # ", " *#### ", "     # ", " *###  "};
    const char* h2_letter[] = {" #     ", " #     ", " ####. ", " #  *# ", " #   # ", "       ", "       "};
    const char** letters[] = {h_letter, i_letter, g_letter, h2_letter};
    int letter_widths[] = {7, 3, 7, 7}, letter_height = 7;
    int total_width = 7 + 1 + 3 + 1 + 7 + 1 + 7;
    
    float base_y = cy - 4;
    title_y_pos.set_target(base_y + 1.5f * std::sin(time * 0.4f), time);
    title_y_pos.update(0.03f);
    int title_y = title_y_pos.get_int();
    int start_x = cx - total_width / 2;
    
    for (int layer = 4; layer > 0; layer--) {
        Color glow_color = Theme::NEON_GREEN.lerp(Theme::CYAN_BRIGHT, 0.3f + 0.7f * std::sin(time * 0.6f));
        fb.draw_glow(cx, title_y + 3, 10 + layer * 2, glow_color, 0.2f / layer * 1.5f);
    }
    
    const char shading[] = {'#', '=', '+', '*'};
    int current_x = start_x;
    for (int l = 0; l < 4; l++) {
        float letter_hue = 0.35f + l * 0.025f + 0.04f * std::sin(time * 0.25f + l);
        Color base_color = Color::hsv(letter_hue, 0.85f, 1.0f);
        for (int row = 0; row < letter_height; row++) {
            const char* line = letters[l][row];
            for (int col = 0; line[col]; col++)
                if (line[col] == '#') {
                    float brightness = 0.8f + 0.2f * std::sin(time * 1.8f + row * 0.25f + col * 0.15f + l * 0.4f);
                    int shade_idx = std::clamp((int)(brightness * 3.5f), 0, 3);
                    int px = current_x + col, py = title_y + row;
                    if (px >= 0 && px < fb.width && py >= 0 && py < fb.height)
                        fb.set_pixel(px, py, shading[shade_idx], base_color.multiply(brightness * 1.3f), Theme::BG_DARKER, brightness * 1.3f);
                }
        }
        current_x += letter_widths[l] + 1;
    }
    
    int underline_y = title_y + letter_height + 1;
    float underline_width = 32.0f + 6.0f * std::sin(time * 1.2f);
    for (int x = 0; x < (int)underline_width; x++) {
        float t = (float)x / underline_width;
        float wave = 0.3f * std::sin(t * 3.14159f * 4.0f + time * 1.8f);
        int ux = start_x + x, uy = underline_y + (int)wave;
        if (ux >= 0 && ux < fb.width && uy >= 0 && uy < fb.height) {
            float alpha = 0.6f + 0.4f * std::sin(t * 3.14159f * 2.0f + time * 0.8f);
            Color line_color = Theme::NEON_GREEN.lerp(Theme::CYAN_BRIGHT, t).multiply(alpha * 1.2f);
            fb.set_pixel(ux, uy, '=', line_color, Theme::BG_DARKER, alpha * 1.2f);
            if (ux > 0 && ux < fb.width - 1) {
                fb.blend_pixel(ux, uy - 1, ' ', line_color, alpha * 0.3f);
                fb.blend_pixel(ux, uy + 1, ' ', line_color, alpha * 0.3f);
            }
        }
    }
}

class BannerAnimator {
public:
    bool running = true;
    float phase = 0, global_time = 0;
    int frame = 0;
    Framebuffer* fb = nullptr;
    ParticleSystem particles{600};
    std::vector<RotatingRing> rings;
    std::vector<ElectricArc> arcs;
    SmoothPosition title_y;
    
    BannerAnimator() { rings.reserve(6); }
    
    void init() {
        std::cout << ANSI::ALT_BUFFER << ANSI::HIDE_CURSOR << ANSI::CLEAR_SCREEN;
        std::signal(SIGINT, [](int) { std::cout << ANSI::NORM_BUFFER << ANSI::SHOW_CURSOR << ANSI::RESET << ANSI::CLEAR_SCREEN; exit(0); });
        std::signal(SIGTERM, [](int) { std::cout << ANSI::NORM_BUFFER << ANSI::SHOW_CURSOR << ANSI::RESET << ANSI::CLEAR_SCREEN; exit(0); });
    }
    
    void cleanup() { std::cout << ANSI::NORM_BUFFER << ANSI::SHOW_CURSOR << ANSI::RESET << ANSI::CLEAR_SCREEN; }
    
    void draw_background(Framebuffer& fb) {
        for (auto& row : fb.pixels)
            for (auto& p : row) p.bg = Theme::BG_DARKER, p.dirty = false;
    }
    
    void draw_concentric_circles(Framebuffer& fb, int cx, int cy, float time) {
        constexpr int radii[] = {15, 28, 42, 56, 70, 85};
        constexpr float opacities[] = {0.7f, 0.5f, 0.4f, 0.3f, 0.2f, 0.15f};
        constexpr float phases[] = {0.0f, 0.7f, 1.4f, 2.1f, 2.8f, 3.5f};
        const Color colors[] = {Theme::NEON_GREEN, Theme::CYAN_BRIGHT, Theme::NEON_GREEN, Theme::PURPLE_BRIGHT, Theme::CYAN_BRIGHT, Theme::NEON_GREEN_BRIGHT};
        const char* chars[] = {"o", "."};
        for (size_t i = 0; i < 6; i++) {
            float pulse = 1.0f + 0.08f * std::sin(time * 2.5f + phases[i]);
            fb.draw_circle(cx, cy, (int)(radii[i] * pulse), colors[i], opacities[i] * (0.5f + 0.5f * std::sin(time + phases[i])), chars[i % 2], 1.0f);
        }
    }
    
    void draw_flow_waves(Framebuffer& fb, int cx, int cy, float time) {
        int wave_width = std::min(50, fb.width / 3);
        int start_x = cx - wave_width, end_x = cx + wave_width;
        struct WaveConfig { int offset_y; float amplitude, frequency, phase_offset; Color color; int num_lines; };
        const WaveConfig waves[] = {
            {0, 3.0f, 1.0f, 0.0f, Theme::NEON_GREEN_BRIGHT, 3},
            {1, 2.0f, 1.3f, 0.5f, Theme::NEON_GREEN, 2},
            {-1, 2.0f, 1.3f, 1.0f, Theme::CYAN_BRIGHT, 2},
            {2, 1.5f, 1.8f, 1.5f, Theme::NEON_GREEN, 1},
            {-2, 1.5f, 1.8f, 2.0f, Theme::PURPLE_BRIGHT, 1}
        };
        for (const auto& wave : waves)
            fb.draw_multi_wave(start_x, end_x, cy + wave.offset_y, time + wave.phase_offset, wave.color, 0.8f, wave.num_lines, 1.5f);
    }
    
    void draw_decorative_elements(Framebuffer& fb, int cx, int cy, float time) {
        int margin = 2;
        Color corner_color = Theme::NEON_GREEN_BRIGHT.multiply(0.5f);
        int corner_size = 4;
        float corner_pulse = 0.5f + 0.5f * std::sin(time * 2.0f);
        for (int i = 0; i < corner_size; i++) {
            fb.set_pixel(margin + i, margin, '+', corner_color.multiply(corner_pulse * 1.2f), Theme::BG_DARKER, corner_pulse * 1.2f);
            fb.set_pixel(margin, margin + i, '|', corner_color.multiply(corner_pulse * 1.2f), Theme::BG_DARKER, corner_pulse * 1.2f);
            fb.set_pixel(fb.width - margin - 1 - i, margin, '+', corner_color.multiply(corner_pulse * 1.2f), Theme::BG_DARKER, corner_pulse * 1.2f);
            fb.set_pixel(fb.width - margin - 1, margin + i, '|', corner_color.multiply(corner_pulse * 1.2f), Theme::BG_DARKER, corner_pulse * 1.2f);
            fb.set_pixel(margin + i, fb.height - margin - 1, '+', corner_color.multiply(corner_pulse * 1.2f), Theme::BG_DARKER, corner_pulse * 1.2f);
            fb.set_pixel(margin, fb.height - margin - 1 - i, '|', corner_color.multiply(corner_pulse * 1.2f), Theme::BG_DARKER, corner_pulse * 1.2f);
            fb.set_pixel(fb.width - margin - 1 - i, fb.height - margin - 1, '+', corner_color.multiply(corner_pulse * 1.2f), Theme::BG_DARKER, corner_pulse * 1.2f);
            fb.set_pixel(fb.width - margin - 1, fb.height - margin - 1 - i, '|', corner_color.multiply(corner_pulse * 1.2f), Theme::BG_DARKER, corner_pulse * 1.2f);
        }
        const char* chars[] = {"o", "+", "*"};
        for (int i = 0; i < 12; i++) {
            float angle = time * 0.3f + (2.0f * 3.14159f * i / 12);
            int dist = 20 + (int)(5.0f * std::sin(time * 2.0f + i));
            int dx = cx + (int)(std::cos(angle) * dist), dy = cy + (int)(std::sin(angle) * dist / 2.0f);
            float brightness = 0.6f + 0.4f * std::sin(time * 3.0f + i * 0.5f);
            fb.blend_pixel(dx, dy, chars[i % 3][0], Theme::NEON_GREEN_BRIGHT.lerp(Theme::CYAN_BRIGHT, (float)i / 12).multiply(brightness * 1.2f), brightness * 1.2f);
        }
        fb.draw_hexagon(margin + 10, margin + 5, 4, Theme::PURPLE_BRIGHT.multiply(0.4f), 0.5f);
        fb.draw_hexagon(fb.width - margin - 14, margin + 5, 4, Theme::CYAN_BRIGHT.multiply(0.4f), 0.5f);
    }
    
    void spawn_electric_arcs(int cx, int cy) {
        if (frame % 20 == 0)
            for (int i = 0; i < 3; i++)
                for (auto& arc : arcs)
                    if (!arc.active) {
                        int offset_x = (rand() % 40) - 20, offset_y = (rand() % 30) - 15;
                        arc.spawn(cx + offset_x - 30, cy + offset_y, cx + offset_x + 30, cy + offset_y, Theme::CYAN_BRIGHT);
                        break;
                    }
    }
    
    void run(float duration_seconds) {
        init();
        auto start_time = std::chrono::high_resolution_clock::now();
        auto last_time = start_time;
        
        constexpr float target_frame_time = 1.0f / 30.0f;
        
        while (running) {
            auto current_time = std::chrono::high_resolution_clock::now();
            float elapsed = std::chrono::duration<float>(current_time - start_time).count();
            if (duration_seconds > 0 && elapsed >= duration_seconds) break;
            
            auto frame_start = std::chrono::high_resolution_clock::now();
            float dt = std::min(std::chrono::duration<float>(frame_start - last_time).count(), 0.1f);
            last_time = frame_start;
            
            TerminalSize term = TerminalSize::get();
            if (term.cols < 60) term.cols = 80;
            if (term.rows < 20) term.rows = 30;
            if (!fb || std::abs(fb->width - term.cols) > 2 || std::abs(fb->height - term.rows) > 2) {
                delete fb;
                fb = new Framebuffer(term.cols, term.rows);
                rings.clear();
                rings.emplace_back(term.cols / 2, term.rows / 2 - 2, 20, Theme::NEON_GREEN_BRIGHT, 0.4f, 16, false);
                rings.emplace_back(term.cols / 2, term.rows / 2 - 2, 35, Theme::CYAN_BRIGHT, -0.3f, 12, true);
                rings.emplace_back(term.cols / 2, term.rows / 2 - 2, 50, Theme::PURPLE_BRIGHT, 0.2f, 10, false);
                rings.emplace_back(term.cols / 2, term.rows / 2 - 2, 65, Theme::NEON_GREEN, -0.15f, 8, true);
            }
            
            int cx = term.cols / 2, cy = term.rows / 2 - 2;
            
            fb->clear();
            draw_background(*fb);
            fb->draw_grid(5, 5, fb->width - 10, fb->height - 10, 20, Theme::NEON_GREEN_DIM, 0.1f);
            draw_concentric_circles(*fb, cx, cy, global_time);
            for (auto& ring : rings) { ring.update(dt); ring.render(*fb); }
            draw_flow_waves(*fb, cx, cy, global_time);
            draw_exquisite_high_title(*fb, cx, cy, global_time, title_y);
            draw_decorative_elements(*fb, cx, cy, global_time);
            spawn_electric_arcs(cx, cy);
            for (auto& arc : arcs) { arc.update(dt); arc.render(*fb); }
            
            if (frame % 20 == 0) {
                particles.emit_burst(cx, cy, Theme::NEON_GREEN_BRIGHT, 8);
                particles.emit(cx - 15, cy + 8, Theme::CYAN_BRIGHT, 3, ParticleType::STAR, '*');
                particles.emit(cx + 15, cy + 8, Theme::PURPLE_BRIGHT, 3, ParticleType::STAR, '*');
            }
            particles.update(dt); particles.render(*fb);
            
            if (frame % 15 == 0) fb->draw_sparkle(10 + rand() % (fb->width - 20), 10 + rand() % (fb->height - 20), 3, Theme::YELLOW, global_time);
            
            fb->render();
            
            global_time += dt; phase += dt * 2.0f; frame++;
            
            auto frame_end = std::chrono::high_resolution_clock::now();
            float frame_duration = std::chrono::duration<float>(frame_end - frame_start).count();
            if (frame_duration < target_frame_time)
                std::this_thread::sleep_for(std::chrono::duration<float>(target_frame_time - frame_duration));
        }
        
        cleanup();
        delete fb;
    }
};

namespace Banner {
    void run(float duration_seconds) {
        BannerAnimator animator;
        animator.run(duration_seconds);
    }
}
