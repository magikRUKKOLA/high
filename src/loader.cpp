#include "loader.hpp"
#include "output_formatter.hpp"
#include "syntax_highlighter.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <unistd.h>
#include <thread>
#include <mutex>

extern std::atomic<bool> g_running;
extern std::atomic<bool> g_interrupted;

// Green color codes (256 palette) - cycling range
static const std::vector<int> GREEN_COLORS = {
    28, 29, 30, 34, 35, 36, 40, 41, 42, 46, 47, 48, 70, 71, 72, 76, 77, 78
};

Loader::Loader() : is_tty_(isatty(STDERR_FILENO)), color_idx_(0), color_direction_(1) {}

Loader::~Loader() {
    stop();
}

Loader& Loader::get_instance() {
    static Loader instance;
    // Set global callbacks so highlighters can update our color
    static bool callbacks_set = false;
    if (!callbacks_set) {
        set_loader_instance(&instance);
        set_loader_for_highlighter(&instance);
        callbacks_set = true;
    }
    return instance;
}

void Loader::update_color() {
    // Cycle through green colors: up to 78, then down to 28
    int current = color_idx_.load(std::memory_order_relaxed);
    int direction = color_direction_.load(std::memory_order_relaxed);
    
    int next_idx = current + direction;
    int next_dir = direction;

    if (next_idx >= static_cast<int>(GREEN_COLORS.size())) {
        next_idx = static_cast<int>(GREEN_COLORS.size()) - 1;
        next_dir = -1;
    } else if (next_idx < 0) {
        next_idx = 0;
        next_dir = 1;
    }

    color_idx_.store(next_idx, std::memory_order_relaxed);
    color_direction_.store(next_dir, std::memory_order_relaxed);
}

int Loader::get_current_color() const {
    if (GREEN_COLORS.empty()) return 0;
    int idx = color_idx_.load(std::memory_order_relaxed);
    return GREEN_COLORS[idx];
}

void Loader::start() {
    if (!is_tty_) return;
    if (running.load(std::memory_order_relaxed)) return;
    
    running.store(true, std::memory_order_release);
    
    // Initialize with resolving text
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_name_ = "Resolving...";
    }
    
    spinner_thread = std::thread(&Loader::run_spinner, this);
}

void Loader::update_model(const std::string& model_name) {
    if (!running.load(std::memory_order_acquire)) return;
    
    std::lock_guard<std::mutex> lock(model_mutex_);
    update_color(); // Update color when model changes
    model_name_ = model_name.empty() ? "Unknown" : model_name;
}

void Loader::stop() {
    bool expected = true;
    if (!running.compare_exchange_strong(expected, false)) return;
    
    if (spinner_thread.joinable()) {
        spinner_thread.join();
    }

    if (is_tty_) {
        std::cerr << "\n" << std::flush;
    }
}

void Loader::run_spinner() {
    const char* spinners[] = {"⠋", "⠙", "⠹", "⢸", "⣰", "⣤", "⣆", "⣇", "⡇", "⣗"};
    size_t idx = 0;
    
    std::string initial_model;
    
    while (running.load(std::memory_order_relaxed)) {
        // FIX: Check g_running too
        if (!g_running.load(std::memory_order_relaxed)) {
            break;
        }
        
        // Get current color for this frame
        int current_color = get_current_color();
        
        // Reset any inherited terminal state, apply color, show spinner only
        std::string reset_start = "\033[0m";
        std::string color_code = current_color > 0 ? 
            "\033[38;5;" + std::to_string(current_color) + "m" : "";
        std::string reset_code = current_color > 0 ? "\033[0m" : "";
        
        // Advance spinner index
        idx = (idx + 1) % 10;
        
        // Update model name at the start of the frame
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            initial_model = model_name_;
        }
        
        // Output spinner + model name (from BAK logic)
        if (!initial_model.empty()) {
            std::cerr << "\r" 
                      << reset_start
                      << color_code
                      << spinners[idx] << "  " 
                      << reset_code
                      << initial_model 
                      << "\033[K" << std::flush;
        } else {
            std::cerr << "\r" 
                      << reset_start
                      << color_code
                      << spinners[idx] 
                      << reset_code
                      << "\033[K" << std::flush;
        }

        // Sleep with periodic wake-up to check running flag
        for (int i = 0; i < 10 && running.load(std::memory_order_relaxed); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Clear the spinner line
    std::cerr << "\r\033[K" << std::flush;
}
