#pragma once

#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>

namespace mcp {

// Thread-safe SSE event queue with keepalive support
struct SSEStream {
    // Event queue
    mutable std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::queue<std::string> events;  // Pre-formatted SSE frames ("event: message\ndata: ...\n\n")

    // HTTP headers for the first response (set before entering SSE mode)
    std::string initial_headers_json;
    bool headers_sent = false;

    // Control flags
    std::atomic<bool> closed{false};
    std::atomic<bool> client_disconnected{false};

    // Push an SSE event to the queue
    void PushEvent(const std::string& event_data, const std::string& event_type = "message");

    // Signal stream closure
    void Close();

    // Wait for next event or timeout (for keepalive)
    // Returns: event string if available, empty string on timeout (send ping),
    // or signals closed/disconnected via the closed flag
    enum class WaitResult {
        EVENT,
        TIMEOUT,  // send keepalive ping
        CLOSED
    };
    WaitResult WaitForEvent(std::string& out_event, int timeout_seconds = 30);
};

} // namespace mcp
