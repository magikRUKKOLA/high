#ifndef LOADER_HPP
#define LOADER_HPP

#include <atomic>
#include <thread>
#include <string>
#include <mutex>

class Loader {
public:
    Loader();
    ~Loader();
    
    // Singleton accessor - automatically sets global callbacks
    static Loader& get_instance();
    
    void start(); 
    void stop();
    
    void update_color();
    void update_model(const std::string& model_name); // NEW: Update model text
    
    int get_current_color() const;

private:
    std::thread spinner_thread;
    std::atomic<bool> running{false};
    bool is_tty_{false};
    
    // Atomic color cycling state
    std::atomic<int> color_idx_{0};
    std::atomic<int> color_direction_{1};  
    
    // Model name state
    std::string model_name_;
    std::mutex model_mutex_;
    
    void run_spinner();
};

#endif
