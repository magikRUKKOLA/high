#include "api_client.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "json_utils.hpp"
#include <curl/curl.h>
#include <memory>
#include <thread>
#include <chrono>

// Unified deleters for CURL
struct CURLDeleter { 
    void operator()(CURL* curl) const noexcept { 
        if (curl) curl_easy_cleanup(curl); 
    } 
};
struct CURLSListDeleter { 
    void operator()(curl_slist* slist) const noexcept { 
        if (slist) curl_slist_free_all(slist); 
    } 
};

using UniqueCURL = std::unique_ptr<CURL, CURLDeleter>;
using UniqueSList = std::unique_ptr<curl_slist, CURLSListDeleter>;

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!ptr || !userdata) return 0;
    auto* response = static_cast<std::string*>(userdata);
    size_t total_size = size * nmemb;
    response->append(ptr, total_size);
    return total_size;
}

size_t stream_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!ptr || !userdata) return 0;
    auto* parser = static_cast<SSEParser*>(userdata);
    size_t total_size = size * nmemb;

    Logger::debug("[SSE] stream_callback: %zu bytes", total_size);

    parser->feed(ptr, total_size);
    return total_size;
}

// Consolidated CURL setup for common options
static void setup_curl_common(UniqueCURL& curl, const std::string& url, char* error_buffer) {
    const Config& config = Config::instance();
    
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "high/1.0");
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buffer);
    
    if (!config.get_http_proxy().empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, config.get_http_proxy().c_str());
    }
}

// Consolidated retry logic check
static bool is_retryable_error(CURLcode code) {
    return code == CURLE_OPERATION_TIMEDOUT ||
           code == CURLE_RECV_ERROR ||
           code == CURLE_SEND_ERROR ||
           code == CURLE_COULDNT_CONNECT ||
           code == CURLE_PARTIAL_FILE ||
           code == CURLE_READ_ERROR ||
           code == CURLE_WRITE_ERROR;
}

std::vector<std::string> APIClient::fetch_models() {
    const Config& config = Config::instance();
    std::string url = config.get_api_base() + "/models";
    std::string response_data;
    
    const int max_retries = 2;
    const std::chrono::milliseconds retry_delay(200);
    
    CURLcode res = CURLE_OK;
    long http_code = 0;
    
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        UniqueCURL curl(curl_easy_init());
        if (!curl) return {};

        char error_buffer[CURL_ERROR_SIZE] = {0};
        
        setup_curl_common(curl, url, error_buffer);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_data);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);

        Logger::debug("[API] Fetch attempt %d/%d: %s", attempt + 1, max_retries, url.c_str());
        
        res = curl_easy_perform(curl.get());
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
        
        if (res == CURLE_OK) break;
        
        Logger::debug("[API] Attempt %d failed: %s (detail: %s)", 
                      attempt + 1, curl_easy_strerror(res), 
                      error_buffer[0] ? error_buffer : "none");
        
        if (attempt < max_retries - 1 && (res == CURLE_COULDNT_CONNECT || res == CURLE_COULDNT_RESOLVE_HOST)) {
            Logger::warn("[API] Connection failed, waiting for mDNS cache...");
            std::this_thread::sleep_for(retry_delay);
            response_data.clear();
            continue;
        }
        break;
    }
    
    if (res != CURLE_OK) {
        Logger::error("Model fetch failed after retries: %s", curl_easy_strerror(res));
        return {};
    }

    if (http_code != 200) {
        Logger::error("Model fetch HTTP error: code=%ld, response=%s", http_code, response_data.c_str());
        return {};
    }
    
    if (response_data.empty()) {
        Logger::error("Empty response from API");
        return {};
    }

    JsonPtr root(json_loads(response_data.c_str(), 0, nullptr));
    if (!root) {
        Logger::error("Failed to parse JSON response from /models");
        return {};
    }

    std::vector<std::string> models;
    json_t* data = json_object_get(root.get(), "data");
    
    if (json_is_array(data)) {
        size_t idx;
        json_t* value;
        json_array_foreach(data, idx, value) {
            const char* id = nullptr;
            json_t* id_obj = json_object_get(value, "id");
            if (json_is_string(id_obj)) id = json_string_value(id_obj);
            else if (json_is_string(value)) id = json_string_value(value);
            if (id) models.emplace_back(id);
        }
    } else {
        Logger::error("Invalid /models response format: 'data' field missing or not array");
    }
    
    return models;
}

static int xferinfo_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, 
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    if (!clientp) return 0;
    auto* running_flag = static_cast<std::atomic<bool>*>(clientp);
    
    if (!(*running_flag)) {
        Logger::debug("[API] Transfer callback detected abort request");
    }
    
    return (*running_flag) ? 0 : 1;
}

bool APIClient::send_chat_request(const std::string& model,
                                 const ConversationHistory& history,
                                 SSEParser& parser,
                                 std::atomic<bool>& running) {
    const Config& config = Config::instance();

    const int max_retries = 3;
    const std::chrono::milliseconds retry_delay(1000);

    bool success = false;
    CURLcode res = CURLE_OK;

    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        UniqueCURL curl(curl_easy_init());
        if (!curl) return false;

        JsonPtr root(json_object());
        JsonPtr messages(json_array());

        if (!messages) {
            Logger::error("Failed to allocate messages array");
            return false;
        }

        auto safe_append = [&](JsonPtr& msg_ptr) {
            if (json_array_append_new(messages.get(), msg_ptr.get()) == 0) {
                msg_ptr.release();
            } else {
                Logger::error("Failed to append message to JSON array");
            }
        };

        if (!config.get_system_role().empty()) {
            JsonPtr sys_msg(json_object());
            if (sys_msg) {
                json_object_set_new(sys_msg.get(), "role", json_string("system"));
                json_object_set_new(sys_msg.get(), "content", json_string(config.get_system_role().c_str()));
                safe_append(sys_msg);
            }
        }

        for (const auto& msg : history) {
            JsonPtr j_msg(json_object());
            if (j_msg) {
                json_object_set_new(j_msg.get(), "role", json_string(msg.role.c_str()));
                json_object_set_new(j_msg.get(), "content", json_string(msg.content.c_str()));
                safe_append(j_msg);
            }
        }

        json_object_set_new(root.get(), "model", json_string(model.c_str()));

        if (json_object_set_new(root.get(), "messages", messages.get()) != 0) {
            Logger::error("Failed to attach messages to JSON root");
            return false;
        }
        messages.release();

        json_object_set_new(root.get(), "stream", json_true());
        if (config.get_temperature() != 1.0) {
            json_object_set_new(root.get(), "temperature", json_real(config.get_temperature()));
        }

        if (config.get_max_tokens() > 0) {
            json_object_set_new(root.get(), "max_tokens", json_integer(config.get_max_tokens()));
        }

        JsonStrPtr json_payload(json_dumps(root.get(), JSON_COMPACT));
        if (!json_payload) return false;

        std::string payload_str(json_payload.get());
        std::string endpoint = config.get_api_base() + "/chat/completions";

        curl_slist* raw_headers = curl_slist_append(nullptr, "Content-Type: application/json");
        if (!raw_headers) return false;
        if (!curl_slist_append(raw_headers, "Accept: text/event-stream")) {
            curl_slist_free_all(raw_headers);
            return false;
        }
        UniqueSList headers(raw_headers);

        char error_buffer[CURL_ERROR_SIZE] = {0};
        setup_curl_common(curl, endpoint, error_buffer);
        
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_COPYPOSTFIELDS, payload_str.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, stream_callback);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &parser);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(config.get_timeout()));
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &running);

        Logger::debug("[API] Sending chat request to %s (Attempt %d/%d)", endpoint.c_str(), attempt + 1, max_retries + 1);

        if (attempt > 0) {
            // Note: In a real scenario, you might want to clear the parser buffer
            // or handle partial state, but for simplicity we assume fresh request or server handles idempotency.
        }

        res = curl_easy_perform(curl.get());

        Logger::debug("[API] CURL completed, flushing SSE parser buffer");
        parser.flush();

        if (res == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code != 200) {
                char* content_type = nullptr;
                curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_TYPE, &content_type);
                Logger::error("Chat request HTTP error: code=%ld, content-type=%s",
                             http_code, content_type ? content_type : "unknown");
                if (http_code == 503 || http_code == 502 || http_code == 504) {
                    // Retryable gateway errors
                } else {
                    return false;
                }
            } else {
                success = true;
                break;
            }
        } else {
            if (res == CURLE_ABORTED_BY_CALLBACK) {
                Logger::debug("[API] Chat request aborted by user (callback)");
                return false;
            }

            if (attempt < max_retries && is_retryable_error(res)) {
                Logger::warn("[API] Request failed (%s). Retrying in %ld ms... (%d/%d)",
                             curl_easy_strerror(res), retry_delay.count(), attempt + 1, max_retries);
                std::this_thread::sleep_for(retry_delay);
                continue;
            } else {
                Logger::error("Chat request failed after %d attempts: %s",
                             attempt + 1, curl_easy_strerror(res));
                return false;
            }
        }
    }

    return success;
}
