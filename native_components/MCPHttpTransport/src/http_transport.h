#pragma once

#include "pending_requests.h"
#include "sse_stream.h"
#include "event_queue.h"

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

// Forward declare httplib types to avoid including heavy header here
namespace httplib {
    class Server;
    struct Request;
    struct Response;
}

namespace mcp {

// Callback for sending ExternalEvent to 1C
// Parameters: source, event_type, json_data
using ExternalEventCallback = std::function<bool(const std::string& source,
                                                  const std::string& event,
                                                  const std::string& data)>;

class HttpTransport {
public:
    HttpTransport();
    ~HttpTransport();

    // Start HTTP server bound to bind_address:port (bind_address e.g. "0.0.0.0", "127.0.0.1",
    // or a concrete interface IPv4). Empty bind_address is treated as "0.0.0.0".
    bool Start(int port, const std::string& bind_address, ExternalEventCallback callback);
    bool StartQueued(int port, const std::string& bind_address);
    // timeout_ms is validated by Native API; callers must use [0, MaxWaitMs].
    std::string WaitNextEvent(int timeout_ms);

    // Stop HTTP server
    bool Stop();

    // Enumerate local IPv4 network interfaces as JSON array [{"id","displayName","address"}].
    // Windows: GetAdaptersAddresses; Linux/macOS: getifaddrs. Loopback excluded, IPv6 excluded.
    std::string GetLocalAddresses();

    bool IsRunning() const { return running_.load(); }
    int GetPort() const { return port_.load(); }

    // Response methods (called from 1C thread via component)
    bool SendResponse(const std::string& request_id, int status_code,
                      const std::string& headers_json, const std::string& body);
    bool SendSSEEvent(const std::string& request_id, const std::string& event_data,
                      const std::string& headers_json, const std::string& event_type = "message");
    bool CloseSSEStream(const std::string& request_id);
    std::string SendResponseAndWait(const std::string& request_id, int status_code,
        const std::string& headers_json, const std::string& body, int timeout_ms);
    std::string SendSSEEventAndWait(const std::string& request_id, const std::string& event_data,
        const std::string& headers_json, const std::string& event_type, int timeout_ms);
    std::string GetRequestBody(const std::string& request_id);
    // Comma-separated internal request UUIDs, at most 16; empty input probes v1.
    std::string GetRequestStates(const std::string& request_ids);

    // Properties
    int GetRequestTimeout() const { return request_timeout_sec_.load(); }
    void SetRequestTimeout(int seconds) { request_timeout_sec_.store(seconds); }
    int GetMaxConcurrentRequests() const { return max_concurrent_.load(); }
    void SetMaxConcurrentRequests(int max) { max_concurrent_.store(max); }

#ifdef MCPHTTPTRANSPORT_SMOKE_TEST
    friend struct SmokeTest;
#endif

private:
    bool StartLocked(int port, const std::string& bind_address, ExternalEventCallback callback, bool queued);
    bool StopLocked();
    bool Deliver(const std::string& kind, const std::shared_ptr<PendingRequest>& request);
    void NotifyClosed(const std::string& id);
    static bool ExpirePendingLocked(PendingRequest& req);
    std::shared_ptr<PendingRequest> Admit(const httplib::Request& req, httplib::Response& res, bool counted);
    // Route handlers
    void HandleMCPPost(const httplib::Request& req, httplib::Response& res);
    void HandleMCPGet(const httplib::Request& req, httplib::Response& res);
    void HandleLegacySSEMessage(const httplib::Request& req, httplib::Response& res);
    void HandleRequest(const httplib::Request& req, httplib::Response& res,
                       const std::string& event_type);

    // Helper: create PendingRequest from httplib::Request
    std::shared_ptr<PendingRequest> CreatePendingRequest(const httplib::Request& req);

    // Helper: build JSON for ExternalEvent data
    std::string BuildEventJson(const std::shared_ptr<PendingRequest>& req, size_t* budget_offset = nullptr);

    // Helper: build event JSON for legacy SSE /mcp/message requests (full body, no truncation)
    std::string BuildLegacyMessageEventJson(const httplib::Request& req, const std::string& id,
                                           size_t* budget_offset = nullptr);

    // Helper: parse query string to JSON {"key":["val1","val2"]}
    static std::string QueryToJson(const httplib::Request& req);

    // Helper: parse headers to JSON with lowercase keys
    static std::string HeadersToJson(const httplib::Request& req);

    // Helper: apply headers from JSON to httplib::Response
    static void ApplyHeaders(httplib::Response& res, const std::string& headers_json);

    // Helper: generate UUID
    static std::string GenerateUUID();

    // Helper: JSON-escape a string
    static std::string JsonEscape(const std::string& s);

    // Encoding detection for /api/ REST requests
    static std::string AutoDecodeToUTF8(const std::string& raw_bytes,
                                         const std::string& content_type = "");
    static std::string QueryToJsonDecoded(const httplib::Request& req);
    static bool IsValidUTF8(const std::string& s);
    static int CyrillicScore(const std::wstring& s);
    static std::string ExtractCharset(const std::string& content_type);
    static std::wstring DecodeWithCodePage(const std::string& s, unsigned int codepage);
    static std::string WStringToUTF8(const std::wstring& s);

    // Wait for 1C decision on a request (blocks the httplib thread)
    // Returns true when HTTP response owns cleanup (SSE or terminal REST).
    bool WaitForResponse(std::shared_ptr<PendingRequest> req, httplib::Response& res);

    // Wait for 1C decision on SSE connect (blocks, then enters keepalive)
    // Returns true if SSE mode was entered (cleanup deferred to close callback)
    bool WaitForSSEDecision(std::shared_ptr<PendingRequest> req, httplib::Response& res);

    // Set up chunked content provider for SSE streaming
    // close_callback is called when the stream actually ends (client disconnect, Close, Stop)
    void SetupSSEContentProvider(httplib::Response& res,
                                  std::shared_ptr<SSEStream> stream,
                                  std::function<void()> close_callback);

    // Send error JSON response
    static void SendErrorResponse(httplib::Response& res, int status, const std::string& error);

    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{true};
    std::atomic<int> port_{0};
    // Never held by HTTP callbacks or WaitNextEvent. Start/Stop may join HTTP threads.
    std::mutex lifecycle_mutex_;
    std::mutex admission_mutex_;
    std::atomic<bool> queued_{false}; // immutable from thread launch through join
    EventQueue events_;

    PendingRequestStore store_;
    ExternalEventCallback event_callback_;

    std::atomic<int> request_timeout_sec_{180};
    std::atomic<int> max_concurrent_{10};

    static constexpr size_t MAX_BODY_IN_EVENT = 64 * 1024;  // 64KB
    static constexpr const char* SOURCE_NAME = "MCPHttpTransport";
};

} // namespace mcp
