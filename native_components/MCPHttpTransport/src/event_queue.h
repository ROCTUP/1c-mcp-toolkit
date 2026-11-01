#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace mcp {

// Bounded native delivery. No platform calls, COM or memory manager on HTTP threads.
class EventQueue {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t MaxEvents = 64;
    static constexpr size_t MaxBytes = 16 * 1024 * 1024;
    static constexpr size_t MaxStreams = 64;
    static constexpr size_t CloseBytes = 256;
    static constexpr int MaxWaitMs = 5000;

    struct Event {
        std::string kind;
        std::string data;
        std::string id;
        Clock::time_point deadline = Clock::time_point::max();
        // Called while the queue is locked. Must never call back into this queue.
        std::function<bool()> live;
        size_t remaining_ms_offset = std::string::npos;
        bool IsConnect() const { return kind == "SSE_CONNECT" || kind == "SSE_LEGACY_CONNECT"; }
        size_t Bytes() const { return kind.size() + data.size() + id.size() + 128; }
    };

    void Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        Clear();
        ++generation_;
        stopped_ = false;
        ready_.notify_all();
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        Clear();
        ready_.notify_all();
    }

    bool Push(Event event) {
        const bool connect = event.IsConnect();
        if (event.kind != "REQUEST" && event.kind != "MCP_POST" &&
            event.kind != "SSE_LEGACY_MESSAGE" && !connect) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto cost = event.Bytes() + (connect ? CloseBytes : 0);
        if (stopped_ || events_.size() >= MaxEvents || cost > MaxBytes - bytes_) return false;
        if (connect) {
            if (event.id.size() != 36 || close_slots_.size() >= MaxStreams || close_slots_.count(event.id))
                return false;
            // A stream keeps its reservation until close is consumed, even when data is full.
            close_slots_.emplace(event.id, false);
        }
        bytes_ += cost;
        events_.push_back(std::move(event));
        ready_.notify_one();
        return true;
    }

    void ReleaseStream(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        ReleaseStreamLocked(id);
    }

    bool CloseStream(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto slot = close_slots_.find(id);
        if (stopped_ || slot == close_slots_.end()) return false;
        if (!slot->second) { slot->second = true; ++closed_count_; }
        ready_.notify_one();
        return true;
    }

    std::string Wait(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto generation = generation_;
        const auto until = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (stopped_ || generation != generation_) return Outcome("stopped");
            // Close notifications have priority; admission reserves their space up front.
            if (closed_count_) {
                for (const auto& slot : close_slots_) {
                    if (!slot.second) continue;
                    auto data = "{\"id\":\"" + slot.first + "\"}";
                    ReleaseStreamLocked(slot.first);
                    return Wrap("SSE_CLOSED", data);
                }
            }
            if (!events_.empty()) {
                auto event = std::move(events_.front());
                events_.pop_front();
                bytes_ -= event.Bytes();
                // live also completes an expired pending request, waking its HTTP handler.
                const bool live = !event.live || event.live();
                const auto now = Clock::now();
                if (!live || now >= event.deadline) {
                    // The deadline can pass between validation and serialization.
                    if (live && event.live) event.live();
                    if (event.IsConnect()) ReleaseStreamLocked(event.id);
                    continue;
                }
                if (event.deadline != Clock::time_point::max() && event.remaining_ms_offset != std::string::npos) {
                    // Same event schema, reduced original budget; waiting never renews it.
                    // The serializer supplies the offset: header/query keys may have the same name.
                    const auto pos = event.remaining_ms_offset;
                    auto end = event.data.find_first_not_of("-0123456789", pos);
                    event.data.replace(pos, end - pos, std::to_string(
                        std::chrono::duration_cast<std::chrono::milliseconds>(event.deadline - now).count()));
                }
                return Wrap(event.kind, event.data);
            }
            if (!ready_.wait_until(lock, until, [&] {
                return stopped_ || generation != generation_ || closed_count_ || !events_.empty();
            })) return Outcome("timeout");
        }
    }

private:
    static std::string Outcome(const char* value) {
        return std::string("{\"version\":1,\"outcome\":\"") + value + "\"}";
    }
    static std::string Wrap(const std::string& kind, const std::string& data) {
        return "{\"version\":1,\"outcome\":\"event\",\"event\":\"" + kind + "\",\"data\":" + data + "}";
    }
    void ReleaseStreamLocked(const std::string& id) {
        auto slot = close_slots_.find(id);
        if (slot == close_slots_.end()) return;
        if (slot->second) --closed_count_;
        close_slots_.erase(slot);
        bytes_ -= CloseBytes;
    }
    void Clear() {
        events_.clear();
        close_slots_.clear();
        bytes_ = closed_count_ = 0;
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Event> events_;
    std::map<std::string, bool> close_slots_;
    size_t bytes_ = 0;
    size_t closed_count_ = 0;
    uint64_t generation_ = 0;
    bool stopped_ = true;
};

} // namespace mcp
