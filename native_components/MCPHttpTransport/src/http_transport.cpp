#include "http_transport.h"

// Define before including httplib
#define CPPHTTPLIB_NO_EXCEPTIONS
#include "httplib.h"

#include <sstream>
#include <random>
#include <algorithm>
#include <cctype>
#include <map>

#if defined(_WIN32) || defined(_WINDOWS)
#define NOMINMAX   // Prevent min/max macro conflicts with std::min/std::max
#include <windows.h>
#endif

namespace mcp {

HttpTransport::HttpTransport() = default;

HttpTransport::~HttpTransport() {
    Stop();
}

bool HttpTransport::Start(int port, ExternalEventCallback callback) {
    if (running_.load()) return false;

    event_callback_ = std::move(callback);
    port_ = port;
    stopped_.store(false);

    server_ = std::make_unique<httplib::Server>();

    // POST /mcp — MCP Streamable HTTP
    server_->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        HandleMCPPost(req, res);
    });

    // GET /mcp — SSE notification stream
    server_->Get("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        HandleMCPGet(req, res);
    });

    // POST /mcp/message — Legacy SSE transport message endpoint
    server_->Post("/mcp/message", [this](const httplib::Request& req, httplib::Response& res) {
        HandleLegacySSEMessage(req, res);
    });

    // DELETE /mcp — session termination
    server_->Delete("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        HandleRequest(req, res, "REQUEST");
    });

    // GET /health
    server_->Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
        HandleRequest(req, res, "REQUEST");
    });

    // REST API: /api/*
    server_->Get(R"(/api/.*)", [this](const httplib::Request& req, httplib::Response& res) {
        HandleRequest(req, res, "REQUEST");
    });
    server_->Post(R"(/api/.*)", [this](const httplib::Request& req, httplib::Response& res) {
        HandleRequest(req, res, "REQUEST");
    });

    // Catch-all for unmatched routes
    server_->set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == 404) {
            res.set_content(R"({"error":"Not Found"})", "application/json");
        }
    });

    // Set running BEFORE launching thread to ensure memory visibility
    running_.store(true);

    // Start server in a separate thread
    server_thread_ = std::thread([this, port]() {
        server_->listen("0.0.0.0", port);
        running_.store(false);
    });

    // Wait briefly for server to start
    for (int i = 0; i < 50; ++i) {
        if (server_->is_running()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Failed to start
    Stop();
    return false;
}

bool HttpTransport::Stop() {
    // Guard against double-Stop
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return true;  // Already stopped
    }

    if (server_) {
        server_->stop();
    }

    // Signal all pending requests to complete
    store_.RemoveAll();

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    server_.reset();
    running_.store(false);
    return true;
}

// ============================================================================
// Route handlers
// ============================================================================

void HttpTransport::HandleMCPPost(const httplib::Request& req, httplib::Response& res) {
    // Check concurrent request limit (MCP_POST counts toward limit)
    if (store_.IsAtCapacity(max_concurrent_.load())) {
        SendErrorResponse(res, 503, "Server is busy");
        return;
    }

    store_.IncrementActive();

    auto pending = CreatePendingRequest(req);
    std::string event_json = BuildEventJson(pending);

    // Fire ExternalEvent to 1C
    if (event_callback_) {
        event_callback_(SOURCE_NAME, "MCP_POST", event_json);
    }

    // Wait for 1C decision (blocks this httplib thread)
    bool sse_mode = WaitForResponse(pending, res);

    if (!sse_mode) {
        // Normal response or timeout — clean up immediately
        store_.DecrementActive();
        store_.Remove(pending->id);
    }
    // SSE mode: cleanup happens in content provider's close callback
}

void HttpTransport::HandleMCPGet(const httplib::Request& req, httplib::Response& res) {
    // SSE_CONNECT: does NOT count toward active_count (per plan)
    auto pending = CreatePendingRequest(req);
    std::string event_json = BuildEventJson(pending);

    // Detect legacy SSE: if none of the Streamable HTTP headers are present → legacy
    bool is_legacy = !req.has_header("mcp-session-id") &&
                     !req.has_header("mcp-protocol-version") &&
                     !req.has_header("last-event-id");

    // Fire ExternalEvent to 1C
    if (event_callback_) {
        event_callback_(SOURCE_NAME, is_legacy ? "SSE_LEGACY_CONNECT" : "SSE_CONNECT", event_json);
    }

    // Wait for 1C decision, then potentially enter SSE keepalive
    bool sse_mode = WaitForSSEDecision(pending, res);

    if (!sse_mode) {
        // 1C sent error response (400, 404, 406, 409, etc.) — clean up
        store_.Remove(pending->id);
    }
    // SSE mode: cleanup (Remove + SSE_CLOSED) happens in content provider's close callback
}

void HttpTransport::HandleLegacySSEMessage(const httplib::Request& req, httplib::Response& res) {
    // Validate session_id query param
    if (req.params.find("session_id") == req.params.end()) {
        SendErrorResponse(res, 400, "session_id is required");
        return;
    }

    // Load guard: reject if server is already at capacity with blocking requests
    if (store_.IsAtCapacity(max_concurrent_.load())) {
        SendErrorResponse(res, 429, "Too many concurrent requests");
        return;
    }

    // Body size limit: 1 MB
    if (req.body.size() > 1 * 1024 * 1024) {
        SendErrorResponse(res, 413, "Request body too large");
        return;
    }

    // Build event JSON and fire to 1C (fire-and-forget, no store_ entry)
    std::string id = GenerateUUID();
    std::string event_json = BuildLegacyMessageEventJson(req, id);
    if (event_callback_) {
        event_callback_(SOURCE_NAME, "SSE_LEGACY_MESSAGE", event_json);
    }

    res.status = 202;
}

void HttpTransport::HandleRequest(const httplib::Request& req, httplib::Response& res,
                                   const std::string& event_type) {
    // Check concurrent request limit
    if (store_.IsAtCapacity(max_concurrent_.load())) {
        SendErrorResponse(res, 503, "Server is busy");
        return;
    }

    store_.IncrementActive();

    auto pending = CreatePendingRequest(req);
    std::string event_json = BuildEventJson(pending);

    if (event_callback_) {
        event_callback_(SOURCE_NAME, event_type, event_json);
    }

    // Regular requests always get normal response (not SSE)
    WaitForResponse(pending, res);

    store_.DecrementActive();
    store_.Remove(pending->id);
}

// ============================================================================
// Wait for 1C decision (normal requests: REQUEST, MCP_POST)
// ============================================================================

bool HttpTransport::WaitForResponse(std::shared_ptr<PendingRequest> req, httplib::Response& res) {
    std::unique_lock<std::mutex> lock(req->state_mutex);

    int timeout = request_timeout_sec_.load();
    bool signaled = req->cv.wait_for(lock, std::chrono::seconds(timeout), [&req]() {
        return req->state != RequestState::PENDING;
    });

    if (!signaled) {
        // Timeout
        req->state = RequestState::COMPLETED;
        SendErrorResponse(res, 504, "Gateway Timeout");
        return false;
    }

    if (req->state == RequestState::COMPLETED) {
        if (req->sse_stream) {
            // Race: SendSSEEvent set up SSE, then CloseSSEStream completed
            // before we woke up. Stream has queued events — drain them via content provider.
            auto stream = req->sse_stream;
            std::string req_id = req->id;

            ApplyHeaders(res, stream->initial_headers_json);
            res.status = 200;

            SetupSSEContentProvider(res, stream, [this, req_id]() {
                store_.DecrementActive();
                store_.Remove(req_id);
            });
            return true;  // SSE mode — caller must NOT clean up
        }

        // Normal response via SendResponse (no SSE was ever set up)
        ApplyHeaders(res, req->response_headers_json);
        res.status = req->response_status;
        res.body = req->response_body;
        return false;
    }

    if (req->state == RequestState::SSE_ACTIVE) {
        // SSE mode (for MCP_POST request → SSE response)
        auto stream = req->sse_stream;
        if (!stream) {
            SendErrorResponse(res, 500, "Internal error: SSE stream not initialized");
            return false;
        }

        std::string req_id = req->id;

        // Set SSE headers
        ApplyHeaders(res, stream->initial_headers_json);
        res.status = 200;

        // Cleanup runs when the stream actually ends (after content provider finishes)
        SetupSSEContentProvider(res, stream, [this, req_id]() {
            store_.DecrementActive();
            store_.Remove(req_id);
        });
        return true;  // SSE mode — caller must NOT clean up
    }

    return false;
}

// ============================================================================
// Wait for SSE connect decision (GET /mcp)
// ============================================================================

bool HttpTransport::WaitForSSEDecision(std::shared_ptr<PendingRequest> req, httplib::Response& res) {
    // Wait indefinitely for 1C decision (no RequestTimeout for GET /mcp SSE)
    std::unique_lock<std::mutex> lock(req->state_mutex);

    req->cv.wait(lock, [&req]() {
        return req->state != RequestState::PENDING;
    });

    if (req->state == RequestState::COMPLETED) {
        if (req->sse_stream) {
            // Race: SendSSEEvent opened SSE, then CloseSSEStream completed
            // before we woke up. Drain queued events via content provider.
            auto stream = req->sse_stream;
            std::string req_id = req->id;

            ApplyHeaders(res, stream->initial_headers_json);
            res.status = 200;

            SetupSSEContentProvider(res, stream, [this, req_id]() {
                if (event_callback_) {
                    std::string close_json = "{\"id\":\"" + JsonEscape(req_id) + "\"}";
                    event_callback_(SOURCE_NAME, "SSE_CLOSED", close_json);
                }
                store_.Remove(req_id);
            });
            return true;  // SSE mode — caller must NOT clean up
        }

        // 1C sent SendResponse (error: 400, 404, 406, 409, etc.)
        ApplyHeaders(res, req->response_headers_json);
        res.status = req->response_status;
        res.body = req->response_body;
        return false;
    }

    if (req->state == RequestState::SSE_ACTIVE) {
        auto stream = req->sse_stream;
        if (!stream) {
            SendErrorResponse(res, 500, "Internal error: SSE stream not initialized");
            return false;
        }

        std::string req_id = req->id;

        // Set SSE headers
        ApplyHeaders(res, stream->initial_headers_json);
        res.status = 200;

        // Cleanup runs when the stream actually ends:
        // client disconnect, CloseSSEStream from 1C, or Stop()
        SetupSSEContentProvider(res, stream, [this, req_id]() {
            // Fire SSE_CLOSED to 1C — stream has really closed now
            if (event_callback_) {
                std::string close_json = "{\"id\":\"" + JsonEscape(req_id) + "\"}";
                event_callback_(SOURCE_NAME, "SSE_CLOSED", close_json);
            }
            store_.Remove(req_id);
        });
        return true;  // SSE mode — caller must NOT clean up
    }

    return false;
}

// ============================================================================
// SSE content provider setup (shared between WaitForResponse and WaitForSSEDecision)
// ============================================================================

void HttpTransport::SetupSSEContentProvider(httplib::Response& res,
                                             std::shared_ptr<SSEStream> stream,
                                             std::function<void()> close_callback) {
    res.set_chunked_content_provider(
        "text/event-stream",
        [stream](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            while (true) {
                std::string event;
                auto result = stream->WaitForEvent(event, 30);

                if (result == SSEStream::WaitResult::EVENT) {
                    if (!sink.write(event.data(), event.size())) {
                        stream->client_disconnected.store(true);
                        return false;  // Client disconnected
                    }
                } else if (result == SSEStream::WaitResult::TIMEOUT) {
                    // Send keepalive ping
                    const char* ping = ": ping\n\n";
                    if (!sink.write(ping, strlen(ping))) {
                        stream->client_disconnected.store(true);
                        return false;
                    }
                } else {
                    // CLOSED — stream ended (CloseSSEStream, Stop, or drain complete)
                    sink.done();
                    return false;
                }
            }
        },
        [stream, cb = std::move(close_callback)](bool) {
            // This runs when the stream ACTUALLY ends (after content provider exits)
            stream->client_disconnected.store(true);
            stream->Close();
            if (cb) cb();
        }
    );
}

// ============================================================================
// Response methods (called from 1C thread)
// ============================================================================

bool HttpTransport::SendResponse(const std::string& request_id, int status_code,
                                  const std::string& headers_json, const std::string& body) {
    auto req = store_.Get(request_id);
    if (!req) return false;

    std::lock_guard<std::mutex> lock(req->state_mutex);

    // Can only send response from PENDING state
    if (req->state != RequestState::PENDING) return false;

    req->state = RequestState::COMPLETED;
    req->response_status = status_code;
    req->response_headers_json = headers_json;
    req->response_body = body;
    req->cv.notify_all();
    return true;
}

bool HttpTransport::SendSSEEvent(const std::string& request_id, const std::string& event_data,
                                  const std::string& headers_json, const std::string& event_type) {
    auto req = store_.Get(request_id);
    if (!req) return false;

    std::lock_guard<std::mutex> lock(req->state_mutex);

    if (req->state == RequestState::COMPLETED) return false;

    if (req->state == RequestState::PENDING) {
        // First call: transition to SSE_ACTIVE
        auto stream = std::make_shared<SSEStream>();
        stream->initial_headers_json = headers_json;
        stream->headers_sent = true;
        req->sse_stream = stream;
        req->state = RequestState::SSE_ACTIVE;

        // If event_data is not empty, push the first event
        if (!event_data.empty()) {
            stream->PushEvent(event_data, event_type);
        }

        req->cv.notify_all();  // Wake up the waiting httplib thread
        return true;
    }

    if (req->state == RequestState::SSE_ACTIVE) {
        // Subsequent calls: just push event (headers_json ignored)
        if (req->sse_stream && !event_data.empty()) {
            req->sse_stream->PushEvent(event_data, event_type);
        }
        return true;
    }

    return false;
}

bool HttpTransport::CloseSSEStream(const std::string& request_id) {
    auto req = store_.Get(request_id);
    if (!req) return false;

    std::lock_guard<std::mutex> lock(req->state_mutex);

    if (req->state != RequestState::SSE_ACTIVE) return false;

    req->state = RequestState::COMPLETED;
    if (req->sse_stream) {
        req->sse_stream->Close();
    }
    return true;
}

std::string HttpTransport::GetRequestBody(const std::string& request_id) {
    auto req = store_.Get(request_id);
    if (!req) return "";
    return req->body;
}

// ============================================================================
// Helpers
// ============================================================================

std::shared_ptr<PendingRequest> HttpTransport::CreatePendingRequest(const httplib::Request& req) {
    std::string id = GenerateUUID();
    auto pending = store_.Add(id);

    pending->method = req.method;
    pending->path = req.path;
    pending->headers_json = HeadersToJson(req);

    // Auto-detect encoding only for REST API (/api/...) requests
    bool is_rest = (req.path.size() >= 5 && req.path.substr(0, 5) == "/api/");
    if (is_rest) {
        std::string ct = req.get_header_value("Content-Type");
        // Decode body first, then calculate size limit on decoded body
        pending->body = AutoDecodeToUTF8(req.body, ct);
        pending->query_json = QueryToJsonDecoded(req);
    } else {
        pending->body = req.body;
        pending->query_json = QueryToJson(req);
    }
    // Calculate limit on final (decoded) body to prevent oversized events
    pending->body_truncated = (pending->body.size() > MAX_BODY_IN_EVENT);

    return pending;
}

std::string HttpTransport::BuildEventJson(const std::shared_ptr<PendingRequest>& req) {
    std::ostringstream ss;
    ss << "{\"id\":\"" << JsonEscape(req->id) << "\""
       << ",\"method\":\"" << JsonEscape(req->method) << "\""
       << ",\"path\":\"" << JsonEscape(req->path) << "\""
       << ",\"query\":" << req->query_json
       << ",\"headers\":" << req->headers_json;

    if (req->body_truncated) {
        ss << ",\"body\":null,\"bodyTruncated\":true";
    } else {
        ss << ",\"body\":\"" << JsonEscape(req->body) << "\",\"bodyTruncated\":false";
    }

    ss << "}";
    return ss.str();
}

std::string HttpTransport::BuildLegacyMessageEventJson(const httplib::Request& req, const std::string& id) {
    std::ostringstream ss;
    ss << "{\"id\":\"" << JsonEscape(id) << "\""
       << ",\"method\":\"" << JsonEscape(req.method) << "\""
       << ",\"path\":\"" << JsonEscape(req.path) << "\""
       << ",\"query\":" << QueryToJson(req)
       << ",\"headers\":" << HeadersToJson(req)
       // Full body always included, no truncation for legacy SSE messages
       << ",\"body\":\"" << JsonEscape(req.body) << "\",\"bodyTruncated\":false"
       << "}";
    return ss.str();
}

std::string HttpTransport::QueryToJson(const httplib::Request& req) {
    if (req.params.empty()) return "{}";

    // httplib::Params is multimap<string, string>
    // We need {"key": ["val1", "val2"]}
    std::map<std::string, std::vector<std::string>> grouped;
    for (auto& [key, value] : req.params) {
        grouped[key].push_back(value);
    }

    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (auto& [key, values] : grouped) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << JsonEscape(key) << "\":[";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) ss << ",";
            ss << "\"" << JsonEscape(values[i]) << "\"";
        }
        ss << "]";
    }
    ss << "}";
    return ss.str();
}

std::string HttpTransport::HeadersToJson(const httplib::Request& req) {
    // Deduplicate: for multimap, take the last value per key (lowercase)
    std::map<std::string, std::string> deduped;
    for (auto& [key, value] : req.headers) {
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        deduped[lower_key] = value;
    }

    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (auto& [key, value] : deduped) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << JsonEscape(key) << "\":\"" << JsonEscape(value) << "\"";
    }
    ss << "}";
    return ss.str();
}

void HttpTransport::ApplyHeaders(httplib::Response& res, const std::string& headers_json) {
    if (headers_json.empty()) return;

    // Simple JSON parser for {"key":"value", ...}
    // This is a minimal parser sufficient for our well-formed header JSON
    size_t pos = 0;
    auto skip_ws = [&]() {
        while (pos < headers_json.size() && std::isspace(static_cast<unsigned char>(headers_json[pos]))) ++pos;
    };

    auto read_string = [&]() -> std::string {
        std::string result;
        if (pos >= headers_json.size() || headers_json[pos] != '"') return result;
        ++pos; // skip opening quote
        while (pos < headers_json.size() && headers_json[pos] != '"') {
            if (headers_json[pos] == '\\' && pos + 1 < headers_json.size()) {
                ++pos;
                switch (headers_json[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case 'r': result += '\r'; break;
                    default: result += headers_json[pos]; break;
                }
            } else {
                result += headers_json[pos];
            }
            ++pos;
        }
        if (pos < headers_json.size()) ++pos; // skip closing quote
        return result;
    };

    skip_ws();
    if (pos >= headers_json.size() || headers_json[pos] != '{') return;
    ++pos;

    while (pos < headers_json.size()) {
        skip_ws();
        if (pos >= headers_json.size()) break;
        if (headers_json[pos] == '}') break;
        if (headers_json[pos] == ',') { ++pos; continue; }

        std::string key = read_string();
        skip_ws();
        if (pos < headers_json.size() && headers_json[pos] == ':') ++pos;
        skip_ws();
        std::string value = read_string();

        if (!key.empty()) {
            // Don't override Content-Type if set by chunked_content_provider
            std::string lower_key = key;
            std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower_key != "content-type") {
                res.set_header(key, value);
            } else {
                // For non-SSE responses, set Content-Type
                // For SSE, the chunked_content_provider sets it
                res.set_header(key, value);
            }
        }
    }
}

std::string HttpTransport::GenerateUUID() {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    thread_local std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    auto rand_hex = [](uint32_t value, int digits) {
        static const char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(digits);
        for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
            result += hex[(value >> i) & 0xF];
        }
        return result;
    };

    uint32_t a = dist(gen);
    uint32_t b = dist(gen);
    uint32_t c = dist(gen);
    uint32_t d = dist(gen);

    // UUID v4 format: xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx
    b = (b & 0xFFFF0FFF) | 0x00004000;  // version 4
    c = (c & 0x3FFFFFFF) | 0x80000000;  // variant 1

    return rand_hex(a, 8) + "-" +
           rand_hex(b >> 16, 4) + "-" +
           rand_hex(b & 0xFFFF, 4) + "-" +
           rand_hex(c >> 16, 4) + "-" +
           rand_hex(c & 0xFFFF, 4) + rand_hex(d, 8);
}

std::string HttpTransport::JsonEscape(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }
    return result;
}

// ===== Encoding Detection Functions (for /api/ REST requests) =====

std::string HttpTransport::WStringToUTF8(const std::wstring& s) {
#if defined(_WIN32) || defined(_WINDOWS)
    if (s.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                    nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                         &result[0], size, nullptr, nullptr);
    return result;
#else
    return "";
#endif
}

bool HttpTransport::IsValidUTF8(const std::string& s) {
#if defined(_WIN32) || defined(_WINDOWS)
    if (s.empty()) return true;
    int r = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                 s.c_str(), (int)s.size(), nullptr, 0);
    return r > 0;
#else
    return true;
#endif
}

int HttpTransport::CyrillicScore(const std::wstring& s) {
    int score = 0;
    for (wchar_t c : s) {
        if ((c >= 0x0410 && c <= 0x044F) || c == 0x0401 || c == 0x0451)
            score += 2;   // Russian Cyrillic
        else if (c >= 0x2500 && c <= 0x25FF)
            score -= 15;  // Box drawing (CP866 artifact)
        else if (c == 0x2219 || c == 0x221A)
            score -= 5;   // · √ (CP866 artifact)
    }
    return score;
}

std::wstring HttpTransport::DecodeWithCodePage(const std::string& s, unsigned int codepage) {
#if defined(_WIN32) || defined(_WINDOWS)
    if (s.empty()) return L"";
    int size = MultiByteToWideChar((UINT)codepage, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(size, 0);
    MultiByteToWideChar((UINT)codepage, 0, s.c_str(), (int)s.size(), &result[0], size);
    return result;
#else
    return L"";
#endif
}

std::string HttpTransport::ExtractCharset(const std::string& content_type) {
    std::string lower = content_type;
    // (unsigned char) cast: prevents UB for chars > 127 (signed char with std::tolower)
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    // Search for "charset=" (with "=" to avoid matching random words)
    auto pos = lower.find("charset=");
    if (pos == std::string::npos) return "";
    pos += 8;  // len("charset=")
    // Skip whitespace and quotes
    while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '"' || lower[pos] == '\''))
        ++pos;
    std::string charset;
    while (pos < lower.size() && lower[pos] != ';' && lower[pos] != ' '
           && lower[pos] != '"' && lower[pos] != '\'')
        charset += lower[pos++];
    return charset;
}

std::string HttpTransport::AutoDecodeToUTF8(const std::string& raw, const std::string& ct) {
    if (raw.empty()) return raw;

    // 1. Explicit charset from Content-Type
    std::string charset = ExtractCharset(ct);
    if (!charset.empty() && charset != "utf-8" && charset != "utf8") {
        unsigned int cp = (charset == "windows-1251" || charset == "cp1251") ? 1251 :
                          (charset == "cp866"        || charset == "ibm866") ? 866  : 0;
        if (cp != 0) {
            std::wstring wide = DecodeWithCodePage(raw, cp);
            if (!wide.empty()) return WStringToUTF8(wide);
        }
    }

    // 2. UTF-8 fast path
    if (IsValidUTF8(raw)) return raw;

    // 3. CP1251 vs CP866 scoring
    std::wstring w1251 = DecodeWithCodePage(raw, 1251);
    std::wstring w866  = DecodeWithCodePage(raw, 866);
    std::wstring best  = (CyrillicScore(w1251) >= CyrillicScore(w866)) ? w1251 : w866;
    return WStringToUTF8(best);
}

std::string HttpTransport::QueryToJsonDecoded(const httplib::Request& req) {
    if (req.params.empty()) return "{}";

    std::map<std::string, std::vector<std::string>> grouped;
    for (auto& [key, value] : req.params) {
        grouped[key].push_back(AutoDecodeToUTF8(value, ""));  // no Content-Type
    }

    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (auto& [key, values] : grouped) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << JsonEscape(key) << "\":[";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) ss << ",";
            ss << "\"" << JsonEscape(values[i]) << "\"";
        }
        ss << "]";
    }
    ss << "}";
    return ss.str();
}

void HttpTransport::SendErrorResponse(httplib::Response& res, int status, const std::string& error) {
    res.status = status;
    res.set_content("{\"error\":\"" + JsonEscape(error) + "\"}", "application/json");
}

} // namespace mcp
