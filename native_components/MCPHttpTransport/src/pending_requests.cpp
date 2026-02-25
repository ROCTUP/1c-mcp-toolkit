#include "pending_requests.h"
#include "sse_stream.h"

namespace mcp {

std::shared_ptr<PendingRequest> PendingRequestStore::Add(const std::string& id) {
    auto req = std::make_shared<PendingRequest>();
    req->id = id;

    std::lock_guard<std::mutex> lock(map_mutex_);
    requests_[id] = req;
    return req;
}

std::shared_ptr<PendingRequest> PendingRequestStore::Get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = requests_.find(id);
    if (it != requests_.end()) {
        return it->second;
    }
    return nullptr;
}

bool PendingRequestStore::Remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    return requests_.erase(id) > 0;
}

void PendingRequestStore::RemoveAll() {
    std::lock_guard<std::mutex> lock(map_mutex_);
    for (auto& [id, req] : requests_) {
        std::lock_guard<std::mutex> state_lock(req->state_mutex);
        req->state = RequestState::COMPLETED;
        if (req->sse_stream) {
            // SSE mode — close stream so content providers unblock and exit
            req->sse_stream->Close();
        } else {
            // Normal request — give it a proper shutdown status
            req->response_status = 503;
            req->response_headers_json = R"({"Content-Type":"application/json"})";
            req->response_body = R"({"error":"Server shutting down"})";
        }
        req->cv.notify_all();
    }
    requests_.clear();
    // Do NOT reset active_count_ here — handler threads will call
    // DecrementActive() as they unwind, bringing it back to 0 naturally.
}

int PendingRequestStore::GetActiveCount() const {
    return active_count_.load();
}

void PendingRequestStore::IncrementActive() {
    active_count_.fetch_add(1);
}

void PendingRequestStore::DecrementActive() {
    active_count_.fetch_sub(1);
}

bool PendingRequestStore::IsAtCapacity(int max_concurrent) const {
    return active_count_.load() >= max_concurrent;
}

} // namespace mcp
