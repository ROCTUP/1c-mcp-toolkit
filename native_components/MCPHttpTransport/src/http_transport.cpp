#include "http_transport.h"
#include "encoding_utils.h"

// Define before including httplib
#define CPPHTTPLIB_NO_EXCEPTIONS
#include "httplib.h"

#include <sstream>
#include <random>
#include <algorithm>
#include <cctype>
#include <map>
#include <vector>

#if defined(_WIN32) || defined(_WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX   // Prevent min/max macro conflicts with std::min/std::max
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <sys/types.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#endif

namespace mcp {

HttpTransport::HttpTransport() = default;

HttpTransport::~HttpTransport() {
    Stop();
}

bool HttpTransport::Start(int port, const std::string& bind_address, ExternalEventCallback callback) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return StartLocked(port, bind_address, std::move(callback), false);
}

bool HttpTransport::StartQueued(int port, const std::string& bind_address) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return StartLocked(port, bind_address, {}, true);
}

std::string HttpTransport::WaitNextEvent(int timeout_ms) {
    return events_.Wait(std::max(0, std::min(timeout_ms, EventQueue::MaxWaitMs)));
}

bool HttpTransport::StartLocked(int port, const std::string& bind_address,
                                ExternalEventCallback callback, bool queued) {
    if (running_.load()) return false;
    StopLocked(); // also join a listener that ended unexpectedly

    event_callback_ = std::move(callback);
    queued_ = queued;
    if (queued) events_.Start();
    port_ = port;
    stopped_.store(false);

    server_ = std::make_unique<httplib::Server>();

    // POST /mcp — MCP Streamable HTTP (accepts /mcp and /mcp/)
    server_->Post(R"(/mcp/?)", [this](const httplib::Request& req, httplib::Response& res) {
        HandleMCPPost(req, res);
    });

    // GET /mcp — SSE notification stream (accepts /mcp and /mcp/)
    server_->Get(R"(/mcp/?)", [this](const httplib::Request& req, httplib::Response& res) {
        HandleMCPGet(req, res);
    });

    // POST /mcp/message — Legacy SSE transport message endpoint
    server_->Post("/mcp/message", [this](const httplib::Request& req, httplib::Response& res) {
        HandleLegacySSEMessage(req, res);
    });

    // DELETE /mcp — session termination (accepts /mcp and /mcp/)
    server_->Delete(R"(/mcp/?)", [this](const httplib::Request& req, httplib::Response& res) {
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

    // Default body for 404 responses; preserve errors returned by request handlers.
    server_->set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == 404 && res.body.empty()) {
            res.set_content(R"({"error":"Not Found"})", "application/json");
        }
    });

    // Bind synchronously: a busy port is a failed start, never a false running state.
    std::string listen_host = bind_address.empty() ? std::string("0.0.0.0") : bind_address;
    bool bound = false;
    if (port == 0) {
        port_ = server_->bind_to_any_port(listen_host);
        bound = port_.load() > 0;
    } else {
        bound = port > 0 && port <= 65535 && server_->bind_to_port(listen_host, port);
    }
    if (!bound) { StopLocked(); return false; }
    running_.store(true);
    server_thread_ = std::thread([this]() {
        server_->listen_after_bind();
        running_.store(false);
        events_.Stop();
    });

    // Wait briefly for server to start
    for (int i = 0; i < 50; ++i) {
        if (server_->is_running()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Failed to start
    StopLocked();
    return false;
}

bool HttpTransport::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return StopLocked();
}

bool HttpTransport::StopLocked() {
    // Wake the BSL waiter before joining any HTTP thread.
    events_.Stop();
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        stopped_.store(true);
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

std::shared_ptr<PendingRequest> HttpTransport::Admit(const httplib::Request& req,
                                                   httplib::Response& res, bool counted) {
    std::lock_guard<std::mutex> lock(admission_mutex_);
    if (stopped_.load() || (counted && store_.IsAtCapacity(max_concurrent_.load()))) {
        SendErrorResponse(res, 503, "Server is busy or stopping");
        return {};
    }
    auto pending = CreatePendingRequest(req);
    if (counted) store_.IncrementActive();
    return pending;
}

bool HttpTransport::Deliver(const std::string& kind, const std::shared_ptr<PendingRequest>& request) {
    size_t budget_offset;
    auto data = BuildEventJson(request, &budget_offset);
    if (!queued_) return event_callback_ && event_callback_(SOURCE_NAME, kind, data);
    EventQueue::Event event{kind, std::move(data), request->id, request->deadline, {}, budget_offset};
    std::weak_ptr<PendingRequest> weak = request;
    event.live = [weak] {
        auto req = weak.lock();
        if (!req) return false;
        std::lock_guard<std::mutex> lock(req->state_mutex);
        if (req->state != RequestState::PENDING) return false;
        return !ExpirePendingLocked(*req);
    };
    return events_.Push(std::move(event));
}

bool HttpTransport::ExpirePendingLocked(PendingRequest& req) {
    if (req.state != RequestState::PENDING || EventQueue::Clock::now() < req.deadline) return false;
    req.state = RequestState::COMPLETED;
    req.response_status = 504;
    req.response_headers_json = R"({"Content-Type":"application/json"})";
    req.response_body = R"({"error":"Gateway Timeout"})";
    req.cv.notify_all();
    return true;
}

void HttpTransport::NotifyClosed(const std::string& id) {
    if (queued_) {
        events_.CloseStream(id); // admission reserved this slot, even if data is full
    } else if (event_callback_) {
        event_callback_(SOURCE_NAME, "SSE_CLOSED", "{\"id\":\"" + JsonEscape(id) + "\"}");
    }
}

// ============================================================================
// Local network interface enumeration
// ============================================================================

std::string HttpTransport::GetLocalAddresses() {
    // Build JSON array [{"id":..,"displayName":..,"address":..}] of local IPv4 interfaces.
    // Loopback and IPv6 are excluded (loopback is offered by the form as an artificial item).
    std::string json = "[";
    bool first = true;

#if defined(_WIN32) || defined(_WINDOWS)
    ULONG family = AF_INET;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15 * 1024;
    std::vector<unsigned char> buffer(size);
    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG ret = GetAdaptersAddresses(family, flags, nullptr, adapters, &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(family, flags, nullptr, adapters, &size);
    }
    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = adapters; a != nullptr; a = a->Next) {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (a->OperStatus != IfOperStatusUp) continue;

            // id: stable AdapterName (GUID string). displayName: FriendlyName (user-facing).
            std::string id = a->AdapterName ? std::string(a->AdapterName) : std::string();
            std::string display_name;
            if (a->FriendlyName) {
                display_name = WStringToUTF8(std::wstring(a->FriendlyName));
            }

            for (PIP_ADAPTER_UNICAST_ADDRESS ua = a->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                if (!ua->Address.lpSockaddr) continue;
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                char ip[INET_ADDRSTRLEN] = {0};
                if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;

                if (!first) json += ",";
                first = false;
                json += "{\"id\":\"" + JsonEscape(id) +
                        "\",\"displayName\":\"" + JsonEscape(display_name) +
                        "\",\"address\":\"" + JsonEscape(ip) + "\"}";
            }
        }
    }
#else
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0) {
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;

            sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            char ip[INET_ADDRSTRLEN] = {0};
            if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;

            // POSIX: ifa_name serves as both stable id and display name.
            std::string name = ifa->ifa_name ? std::string(ifa->ifa_name) : std::string();

            if (!first) json += ",";
            first = false;
            json += "{\"id\":\"" + JsonEscape(name) +
                    "\",\"displayName\":\"" + JsonEscape(name) +
                    "\",\"address\":\"" + JsonEscape(ip) + "\"}";
        }
        freeifaddrs(ifaddr);
    }
#endif

    json += "]";
    return json;
}

// ============================================================================
// Route handlers
// ============================================================================

void HttpTransport::HandleMCPPost(const httplib::Request& req, httplib::Response& res) {
    auto pending = Admit(req, res, true);
    if (!pending) return;
    if (!Deliver("MCP_POST", pending)) {
        store_.DecrementActive();
        store_.Remove(pending->id);
        SendErrorResponse(res, 503, "Event delivery unavailable");
        return;
    }

    // Wait for 1C decision (blocks this httplib thread)
    bool deferred_cleanup = WaitForResponse(pending, res);

    if (!deferred_cleanup) {
        // Normal response or timeout — clean up immediately
        store_.DecrementActive();
        store_.Remove(pending->id);
    }
    // SSE mode: cleanup happens in content provider's close callback
}

void HttpTransport::HandleMCPGet(const httplib::Request& req, httplib::Response& res) {
    // SSE_CONNECT: does NOT count toward active_count (per plan)
    auto pending = Admit(req, res, false);
    if (!pending) return;

    // Detect legacy SSE: if none of the Streamable HTTP headers are present → legacy
    bool is_legacy = !req.has_header("mcp-session-id") &&
                     !req.has_header("mcp-protocol-version") &&
                     !req.has_header("last-event-id");

    if (!Deliver(is_legacy ? "SSE_LEGACY_CONNECT" : "SSE_CONNECT", pending)) {
        store_.Remove(pending->id);
        SendErrorResponse(res, 503, "Event delivery unavailable");
        return;
    }

    // Wait for 1C decision, then potentially enter SSE keepalive
    bool sse_mode = WaitForSSEDecision(pending, res);

    if (!sse_mode) {
        // A rejected handshake will never produce a stream-close callback.
        if (queued_) events_.ReleaseStream(pending->id);
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
    const auto deadline = EventQueue::Clock::now() + std::chrono::seconds(request_timeout_sec_.load());
    std::string id = GenerateUUID();
    size_t budget_offset;
    std::string event_json = BuildLegacyMessageEventJson(req, id, &budget_offset);
    const bool accepted = !stopped_.load() && (queued_
        ? events_.Push({"SSE_LEGACY_MESSAGE", std::move(event_json), id, deadline, {}, budget_offset})
        : event_callback_ && event_callback_(SOURCE_NAME, "SSE_LEGACY_MESSAGE", event_json));
    if (!accepted) {
        SendErrorResponse(res, 503, "Event delivery unavailable");
        return;
    }
    res.status = 202;
}

void HttpTransport::HandleRequest(const httplib::Request& req, httplib::Response& res,
                                   const std::string& event_type) {
    auto pending = Admit(req, res, true);
    if (!pending) return;
    if (!Deliver(event_type, pending)) {
        store_.DecrementActive();
        store_.Remove(pending->id);
        SendErrorResponse(res, 503, "Event delivery unavailable");
        return;
    }

    if (!WaitForResponse(pending, res)) {
        store_.DecrementActive();
        store_.Remove(pending->id);
    }
}

// ============================================================================
// Wait for 1C decision (normal requests: REQUEST, MCP_POST)
// ============================================================================

bool HttpTransport::WaitForResponse(std::shared_ptr<PendingRequest> req, httplib::Response& res) {
    std::unique_lock<std::mutex> lock(req->state_mutex);

    bool signaled = req->cv.wait_until(lock, req->deadline, [&req]() {
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
        if (req->completion) {
            auto completion = req->completion;
            auto body = std::make_shared<std::string>(req->response_body);
            auto written = std::make_shared<size_t>(0);
            auto content_type = res.get_header_value("Content-Type");
            if (content_type.empty()) content_type = "application/json";
            res.headers.erase("Content-Length");
            res.headers.erase("Transfer-Encoding");
            res.set_content_provider(body->size(), content_type,
                [body, written, completion](size_t offset, size_t length, httplib::DataSink& sink) {
                    {
                        std::lock_guard<std::mutex> lock(completion->mutex);
                        if (completion->result == "failed") return false;
                    }
                    if (offset != *written || offset > body->size()) return false;
                    // Bound each blocking socket write; a slow peer must not trap Stop
                    // in one multi-megabyte send after the caller's wait has expired.
                    const auto count = (std::min)({length, body->size() - offset, size_t(16384)});
                    if (!count || !sink.write(body->data() + offset, count)) return false;
                    *written += count;
                    return true;
                },
                [this, req, completion, body, written](bool success) {
                    completion->Complete(success && *written == body->size());
                    store_.DecrementActive();
                    store_.Remove(req->id);
                });
            return true;
        }
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
    // Preserve the old callback handshake; queued admission keeps its original deadline.
    // After the handshake, an established SSE stream has no request timeout.
    std::unique_lock<std::mutex> lock(req->state_mutex);

    auto decided = [&req]() { return req->state != RequestState::PENDING; };
    if (queued_) {
        if (!req->cv.wait_until(lock, req->deadline, decided)) ExpirePendingLocked(*req);
    } else {
        req->cv.wait(lock, decided);
    }

    if (req->state == RequestState::COMPLETED) {
        if (req->sse_stream) {
            // Race: SendSSEEvent opened SSE, then CloseSSEStream completed
            // before we woke up. Drain queued events via content provider.
            auto stream = req->sse_stream;
            std::string req_id = req->id;

            ApplyHeaders(res, stream->initial_headers_json);
            res.status = 200;

            SetupSSEContentProvider(res, stream, [this, req_id]() {
                store_.Remove(req_id);
                NotifyClosed(req_id);
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
            store_.Remove(req_id);
            NotifyClosed(req_id);
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
                    size_t offset = 0;
                    while (offset < event.size()) {
                        // A terminal send can attach while an earlier event is being written.
                        std::shared_ptr<ResponseCompletion> completion;
                        {
                            std::lock_guard<std::mutex> lock(stream->queue_mutex);
                            completion = stream->completion;
                        }
                        if (completion) {
                            std::lock_guard<std::mutex> lock(completion->mutex);
                            if (completion->result == "failed") return false;
                        }
                        const auto count = (std::min)(size_t(16384), event.size() - offset);
                        if (!sink.write(event.data() + offset, count)) {
                            stream->client_disconnected.store(true);
                            return false;
                        }
                        offset += count;
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
                    stream->drained.store(true);
                    return true;
                }
            }
        },
        [stream, cb = std::move(close_callback)](bool success) {
            // This runs when the stream ACTUALLY ends (after content provider exits)
            stream->client_disconnected.store(true);
            stream->Close();
            {
                std::lock_guard<std::mutex> lock(stream->queue_mutex);
                if (stream->completion)
                    stream->completion->Complete(success && stream->drained.load());
            }
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
    if (queued_ && ExpirePendingLocked(*req)) return false;

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
        if (queued_ && ExpirePendingLocked(*req)) return false;
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
            return req->sse_stream->PushEvent(event_data, event_type);
        }
        return true;
    }

    return false;
}

std::string HttpTransport::SendResponseAndWait(const std::string& request_id, int status_code,
        const std::string& headers_json, const std::string& body, int timeout_ms) {
    if (timeout_ms < 1 || timeout_ms > 10000 || body.empty() ||
        status_code < 200 || status_code > 599 || status_code == 204 || status_code == 304)
        return "rejected";
    auto req = store_.Get(request_id);
    if (!req) return "rejected";
    auto completion = std::make_shared<ResponseCompletion>();
    {
        std::lock_guard<std::mutex> lock(req->state_mutex);
        // GET handshakes and HEAD cannot carry this terminal REST body.
        if (req->method == "HEAD" ||
            (req->method == "GET" && (req->path == "/mcp" || req->path == "/mcp/")) ||
            req->state != RequestState::PENDING || req->completion ||
            stopped_.load() || (queued_ && ExpirePendingLocked(*req))) return "rejected";
        req->completion = completion;
        req->response_status = status_code;
        req->response_headers_json = headers_json;
        req->response_body = body;
        req->state = RequestState::COMPLETED;
        req->cv.notify_all();
    }
    return completion->Wait(timeout_ms);
}

std::string HttpTransport::SendSSEEventAndWait(const std::string& request_id,
        const std::string& event_data, const std::string& headers_json,
        const std::string& event_type, int timeout_ms) {
    if (timeout_ms < 1 || timeout_ms > 10000 || event_data.empty() ||
        event_type.find_first_of("\r\n") != std::string::npos) return "rejected";
    auto req = store_.Get(request_id);
    if (!req) return "rejected";
    auto completion = std::make_shared<ResponseCompletion>();
    {
        std::lock_guard<std::mutex> lock(req->state_mutex);
        if (req->state == RequestState::COMPLETED || req->completion || stopped_.load() ||
            (queued_ && ExpirePendingLocked(*req))) return "rejected";
        auto stream = req->sse_stream;
        if (!stream) {
            stream = std::make_shared<SSEStream>();
            stream->initial_headers_json = headers_json;
            stream->headers_sent = true;
        }
        if (!stream->PushEvent(event_data, event_type.empty() ? "message" : event_type, completion))
            return "rejected";
        req->sse_stream = stream;
        req->completion = completion;
        req->state = RequestState::COMPLETED;
        req->cv.notify_all();
    }
    return completion->Wait(timeout_ms);
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
    pending->deadline = pending->created_at + std::chrono::seconds(request_timeout_sec_.load());

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

std::string HttpTransport::BuildEventJson(const std::shared_ptr<PendingRequest>& req, size_t* budget_offset) {
    std::ostringstream ss;
    ss << "{\"id\":\"" << JsonEscape(req->id) << "\""
       << ",\"method\":\"" << JsonEscape(req->method) << "\""
       << ",\"path\":\"" << JsonEscape(req->path) << "\""
       << ",\"query\":" << req->query_json
       << ",\"headers\":" << req->headers_json;
    ss << ",\"request_state_version\":1,\"remaining_ms\":";
    if (budget_offset) *budget_offset = static_cast<size_t>(ss.tellp());
    ss << std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
              req->deadline - std::chrono::steady_clock::now()).count());

    if (req->body_truncated) {
        ss << ",\"body\":null,\"bodyTruncated\":true";
    } else {
        ss << ",\"body\":\"" << JsonEscape(req->body) << "\",\"bodyTruncated\":false";
    }

    ss << "}";
    return ss.str();
}

std::string HttpTransport::BuildLegacyMessageEventJson(const httplib::Request& req, const std::string& id,
                                                     size_t* budget_offset) {
    std::ostringstream ss;
    ss << "{\"id\":\"" << JsonEscape(id) << "\""
       << ",\"method\":\"" << JsonEscape(req.method) << "\""
       << ",\"path\":\"" << JsonEscape(req.path) << "\""
       << ",\"query\":" << QueryToJson(req)
       << ",\"headers\":" << HeadersToJson(req)
       << ",\"request_state_version\":1,\"remaining_ms\":";
    if (budget_offset) *budget_offset = static_cast<size_t>(ss.tellp());
    ss << request_timeout_sec_.load() * 1000LL
       // Full body always included, no truncation for legacy SSE messages
       << ",\"body\":\"" << JsonEscape(req.body) << "\",\"bodyTruncated\":false"
       << "}";
    return ss.str();
}

std::string HttpTransport::GetRequestStates(const std::string& request_ids) {
    if (request_ids.size() > 16 * 37) return "{\"version\":1,\"error\":\"Too many request IDs\"}";
    std::istringstream input(request_ids);
    std::ostringstream output;
    output << "{\"version\":1,\"terminal_response_wait_version\":1,\"items\":[";
    std::string id;
    bool first = true;
    int count = 0;
    while (std::getline(input, id, ',')) {
        if (++count > 16 || id.empty() || id.size() > 36 ||
            id.find_first_not_of("0123456789abcdefABCDEF-") != std::string::npos)
            return "{\"version\":1,\"error\":\"Invalid request IDs\"}";
        bool active = false;
        int64_t remaining = 0;
        auto req = store_.Get(id);
        if (req) {
            std::lock_guard<std::mutex> lock(req->state_mutex);
            if (req->state == RequestState::SSE_ACTIVE) {
                active = true;
                remaining = -1; // Stream lifetime; the BSL operation retains its own deadline.
            } else if (req->state == RequestState::PENDING) {
                remaining = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                    req->deadline - std::chrono::steady_clock::now()).count());
                active = remaining > 0;
            }
        }
        if (!first) output << ',';
        first = false;
        output << "{\"id\":\"" << id << "\",\"active\":" << (active ? "true" : "false")
               << ",\"remaining_ms\":" << remaining << '}';
    }
    output << "]}";
    return output.str();
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
    return mcp::WstrToUtf8(s);
#endif
}

bool HttpTransport::IsValidUTF8(const std::string& s) {
#if defined(_WIN32) || defined(_WINDOWS)
    if (s.empty()) return true;
    int r = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                 s.c_str(), (int)s.size(), nullptr, 0);
    return r > 0;
#else
    return mcp::IsValidUtf8(s);
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
    return mcp::DecodeCP1x(s, (int)codepage);
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
        grouped[AutoDecodeToUTF8(key, "")].push_back(AutoDecodeToUTF8(value, ""));
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
