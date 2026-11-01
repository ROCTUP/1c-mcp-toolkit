#include "search_executor.h"
#include <chrono>
#include <stdexcept>
#include <utility>

namespace syntax_help {
namespace {
bool Terminal(const std::string& state) {
    return state == "succeeded" || state == "failed" || state == "cancelled";
}
}

SearchExecutor::SearchExecutor(Search search) : search_(std::move(search)) {}
SearchExecutor::~SearchExecutor() { BeginShutdown(); Join(); }

bool SearchExecutor::Start() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (started_) return !stopping_;
    if (!jobs_.empty() || !threads_.empty()) return false;
    stopping_ = false;
    started_ = true;
    try {
        for (size_t i = 0; i < MaxWorkers; ++i) {
            threads_.emplace_back([this] { Run(); });
            ++alive_;
        }
    } catch (...) {
        stopping_ = true;
        changed_.notify_all();
        lock.unlock();
        Join();
        return false;
    }
    return true;
}

std::string SearchExecutor::Submit(const std::string& id, std::vector<std::string> keywords, bool all) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopping_) return "executor_not_running";
    if (id.empty() || id.size() > 256) return "invalid_id";
    if (jobs_.count(id)) return "duplicate_id";
    if (jobs_.size() >= MaxRequests) return "busy";
    auto job = std::make_shared<Job>();
    job->id = id;
    job->keywords = std::move(keywords);
    job->all = all;
    jobs_.emplace(id, job);
    try { queue_.push_back(job); }
    catch (...) { jobs_.erase(id); throw; }
    changed_.notify_one();
    return "";
}

std::vector<SearchExecutor::Result> SearchExecutor::States() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Result> result;
    for (const auto& pair : jobs_) result.push_back({pair.first, pair.second->state, "", "", pair.second->started_at});
    return result;
}

SearchExecutor::Result SearchExecutor::Read(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return {id, "unknown", "", "unknown_id"};
    const auto& job = *it->second;
    return {job.id, job.state, job.body, job.error, job.started_at};
}

bool SearchExecutor::Acknowledge(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return true;
    if (!Terminal(it->second->state)) return false;
    result_bytes_ -= it->second->body.size();
    jobs_.erase(it);
    return true;
}

void SearchExecutor::Cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    auto& job = *it->second;
    job.cancel = true;
    if (job.state != "running") {
        result_bytes_ -= job.body.size();
        job.body.clear();
        job.error.clear();
        job.state = "cancelled";
        for (auto q = queue_.begin(); q != queue_.end();) {
            if (*q == it->second) q = queue_.erase(q); else ++q;
        }
    }
}

void SearchExecutor::BeginShutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    for (auto& pair : jobs_) {
        auto& job = *pair.second;
        job.cancel = true;
        if (job.state == "queued") job.state = "cancelled";
    }
    queue_.clear();
    changed_.notify_all();
}

bool SearchExecutor::ShutdownComplete() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_ || alive_) return false;
    }
    Join();
    return true;
}

void SearchExecutor::Join() {
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
    threads_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

bool SearchExecutor::Empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.empty();
}

void SearchExecutor::Run() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) { --alive_; return; }
            job = queue_.front();
            queue_.pop_front();
            job->state = "running";
            job->started_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        std::string body, error;
        try { body = search_(job->keywords, job->all, &job->cancel); }
        catch (...) { error = "search_failed"; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (job->cancel) {
                job->state = "cancelled";
            } else if (!error.empty()) {
                job->state = "failed";
                job->error = std::move(error);
            } else if (body.size() > MaxResultBytes - result_bytes_) {
                job->state = "failed";
                job->error = "result_budget_exceeded";
            } else {
                job->body = std::move(body);
                result_bytes_ += job->body.size();
                job->state = "succeeded";
            }
        }
    }
}
} // namespace syntax_help
