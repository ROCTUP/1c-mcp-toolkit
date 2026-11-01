#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace syntax_help {

// No platform interfaces cross this boundary. The search function owns its index.
class SearchExecutor {
public:
    using Search = std::function<std::string(const std::vector<std::string>&, bool,
                                            const std::atomic_bool*)>;
    struct Result {
        std::string id, state, body, error;
        int64_t started_at = 0; // UTC Unix seconds; zero means execution never started.
    };
    explicit SearchExecutor(Search search);
    ~SearchExecutor();
    SearchExecutor(const SearchExecutor&) = delete;
    SearchExecutor& operator=(const SearchExecutor&) = delete;
    bool Start();
    std::string Submit(const std::string& id, std::vector<std::string> keywords, bool all);
    std::vector<Result> States() const;
    Result Read(const std::string& id) const;
    bool Acknowledge(const std::string& id);
    void Cancel(const std::string& id);
    void BeginShutdown();
    bool ShutdownComplete();
    bool Empty() const;
    static constexpr size_t MaxWorkers = 8;
    static constexpr size_t MaxRequests = 10;
    static constexpr size_t MaxResultBytes = 64 * 1024 * 1024;
private:
    struct Job {
        std::string id, state = "queued", body, error;
        std::vector<std::string> keywords;
        bool all = true;
        int64_t started_at = 0;
        std::atomic_bool cancel{false};
    };
    void Run();
    void Join();
    Search search_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::vector<std::thread> threads_;
    bool started_ = false, stopping_ = false;
    size_t alive_ = 0, result_bytes_ = 0;
};

} // namespace syntax_help
