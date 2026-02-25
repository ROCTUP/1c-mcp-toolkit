#include "sse_stream.h"

namespace mcp {

void SSEStream::PushEvent(const std::string& event_data, const std::string& event_type) {
    if (closed.load() || client_disconnected.load()) return;

    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        // Format as SSE frame — split multi-line data per W3C EventSource spec
        std::string frame = "event: " + event_type + "\n";
        size_t start = 0;
        while (start < event_data.size()) {
            size_t nl = event_data.find('\n', start);
            if (nl == std::string::npos) {
                frame += "data: " + event_data.substr(start) + "\n";
                break;
            }
            frame += "data: " + event_data.substr(start, nl - start) + "\n";
            start = nl + 1;
        }
        if (event_data.empty()) {
            frame += "data: \n";
        }
        frame += "\n";
        events.push(std::move(frame));
    }
    queue_cv.notify_one();
}

void SSEStream::Close() {
    closed.store(true);
    queue_cv.notify_all();
}

SSEStream::WaitResult SSEStream::WaitForEvent(std::string& out_event, int timeout_seconds) {
    std::unique_lock<std::mutex> lock(queue_mutex);

    auto pred = [this]() {
        return !events.empty() || closed.load() || client_disconnected.load();
    };

    if (queue_cv.wait_for(lock, std::chrono::seconds(timeout_seconds), pred)) {
        // Condition met
        if (closed.load() || client_disconnected.load()) {
            // Drain remaining events first
            if (!events.empty()) {
                out_event = std::move(events.front());
                events.pop();
                return WaitResult::EVENT;
            }
            return WaitResult::CLOSED;
        }
        if (!events.empty()) {
            out_event = std::move(events.front());
            events.pop();
            return WaitResult::EVENT;
        }
        return WaitResult::CLOSED;
    }

    // Timeout — send keepalive
    return WaitResult::TIMEOUT;
}

} // namespace mcp
