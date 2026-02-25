#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <atomic>

namespace mcp {

enum class RequestState {
    PENDING,
    SSE_ACTIVE,
    COMPLETED
};

// Forward declaration
struct SSEStream;

// Represents a single pending HTTP request waiting for 1C decision
struct PendingRequest {
    std::string id;
    std::string method;    // HTTP method
    std::string path;
    std::string query_json; // query string as JSON
    std::string headers_json;
    std::string body;
    bool body_truncated = false;

    // State management
    mutable std::mutex state_mutex;
    std::condition_variable cv;
    RequestState state = RequestState::PENDING;

    // Response data (filled by SendResponse)
    int response_status = 0;
    std::string response_headers_json;
    std::string response_body;

    // SSE stream (filled by SendSSEEvent when entering SSE mode)
    std::shared_ptr<SSEStream> sse_stream;

    // Timestamp for timeout
    std::chrono::steady_clock::time_point created_at;

    PendingRequest() : created_at(std::chrono::steady_clock::now()) {}
};

// Thread-safe storage for pending requests
class PendingRequestStore {
public:
    PendingRequestStore() = default;

    // Add a new pending request, returns the request pointer
    std::shared_ptr<PendingRequest> Add(const std::string& id);

    // Get a pending request by ID
    std::shared_ptr<PendingRequest> Get(const std::string& id) const;

    // Remove a request by ID
    bool Remove(const std::string& id);

    // Remove all requests and notify them (for shutdown)
    void RemoveAll();

    // Get count of active (non-SSE) requests
    int GetActiveCount() const;

    // Increment/decrement active request counter
    void IncrementActive();
    void DecrementActive();

    // Check if active count exceeds limit
    bool IsAtCapacity(int max_concurrent) const;

private:
    mutable std::mutex map_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingRequest>> requests_;
    std::atomic<int> active_count_{0};  // REQUEST + MCP_POST only, not SSE_CONNECT
};

} // namespace mcp
