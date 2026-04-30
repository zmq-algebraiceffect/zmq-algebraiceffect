#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace zmqae::detail {

// MPSC (Multi-Producer Single-Consumer) mutex-based queue.
// Producers: any thread (push). Consumer: single thread (pop/drain).
template <typename T>
class mpsc_queue {
    std::queue<T> queue_;
    mutable std::mutex mutex_;

public:
    mpsc_queue() = default;
    mpsc_queue(const mpsc_queue &) = delete;
    mpsc_queue &operator=(const mpsc_queue &) = delete;
    mpsc_queue(mpsc_queue &&) = delete;
    mpsc_queue &operator=(mpsc_queue &&) = delete;

    // any thread
    void push(T item) {
        std::lock_guard<std::mutex> lock{mutex_};
        queue_.push(std::move(item));
    }

    // consumer thread
    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock{mutex_};
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    // consumer thread — drains all items at once (for poll())
    std::vector<T> drain() {
        std::lock_guard<std::mutex> lock{mutex_};
        std::vector<T> result;
        result.reserve(queue_.size());
        while (!queue_.empty()) {
            result.push_back(std::move(queue_.front()));
            queue_.pop();
        }
        return result;
    }

    // consumer thread
    bool empty() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return queue_.empty();
    }
};

}
