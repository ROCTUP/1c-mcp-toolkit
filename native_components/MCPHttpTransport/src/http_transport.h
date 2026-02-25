#pragma once

#include "pending_requests.h"
#include "sse_stream.h"

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

    // Start HTTP server on 127.0.0.1:port
    bool Start(int port, ExternalEventCallback callback);

    // Stop HTTP server
    bool Stop();

    bool IsRunning() const { return running_.load(); }
    int GetPort() const { return port_; }

    // Response methods (called from 1C thread via component)
    bool SendResponse(const std::string& request_id, int status_code,
                      const std::string& headers_json, const std::string& body);
    bool SendSSEEvent(const std::string& request_id, const std::string& event_data,
                      const std::string& headers_json, const std::string& event_type = "message");
    bool CloseSSEStream(const std::string& request_id);
    std::string GetRequestBody(const std::string& request_id);

    // Properties
    int GetRequestTimeout() const { return request_timeout_sec_.load(); }
    void SetRequestTimeout(int seconds) { request_timeout_sec_.store(seconds); }
    int GetMaxConcurrentRequests() const { return max_concurrent_.load(); }
    void SetMaxConcurrentRequests(int max) { max_concurrent_.store(max); }

private:
    // Route handlers
    void HandleMCPPost(const httplib::Request& req, httplib::Response& res);
    void HandleMCPGet(const httplib::Request& req, httplib::Response& res);
    void HandleLegacySSEMessage(const httplib::Request& req, httplib::Response& res);
    void HandleRequest(const httplib::Request& req, httplib::Response& res,
                       const std::string& event_type);

    // Helper: create PendingRequest from httplib::Request
    std::shared_ptr<PendingRequest> CreatePendingRequest(const httplib::Request& req);

    // Helper: build JSON for ExternalEvent data
    std::string BuildEventJson(const std::shared_ptr<PendingRequest>& req);

    // Helper: build event JSON for legacy SSE /mcp/message requests (full body, no truncation)
    std::string BuildLegacyMessageEventJson(const httplib::Request& req, const std::string& id);

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
    // Returns true if SSE mode was entered (cleanup deferred to close callback)
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
    std::atomic<bool> stopped_{false};
    int port_ = 0;

    PendingRequestStore store_;
    ExternalEventCallback event_callback_;

    std::atomic<int> request_timeout_sec_{180};
    std::atomic<int> max_concurrent_{10};

    static constexpr size_t MAX_BODY_IN_EVENT = 64 * 1024;  // 64KB
    static constexpr const char* SOURCE_NAME = "MCPHttpTransport";
};

} // namespace mcp
