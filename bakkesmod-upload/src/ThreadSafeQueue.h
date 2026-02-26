#pragma once
#include <mutex>
#include <queue>
#include <optional>
#include <condition_variable>

/**
 * A thread-safe bounded queue for passing data between threads.
 * Used for audio samples and encoded packets.
 */
template<typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t maxSize = 256) : maxSize_(maxSize) {}

    /** Push an item. Drops oldest if queue is full (non-blocking). */
    bool push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= maxSize_) {
            queue_.pop(); // Drop oldest to prevent unbounded growth
            dropped_++;
        }
        queue_.push(std::move(item));
        cv_.notify_one();
        return true;
    }

    /** Try to pop an item without blocking. */
    std::optional<T> tryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    /** Pop with timeout. Returns nullopt if timed out. */
    std::optional<T> popWait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    /** Clear all items. */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    uint64_t droppedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    size_t maxSize_;
    uint64_t dropped_ = 0;
};
