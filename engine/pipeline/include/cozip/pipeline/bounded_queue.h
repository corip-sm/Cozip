#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace cozip::pipeline
{
template <typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(std::size_t capacity)
        : capacity_(capacity)
    {
    }

    bool Push(T value)
    {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [&] { return closed_ || queue_.size() < capacity_; });
        if (closed_)
        {
            return false;
        }

        queue_.push_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    std::optional<T> Pop()
    {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty())
        {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return value;
    }

    void Close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t Capacity() const noexcept
    {
        return capacity_;
    }

private:
    std::size_t capacity_ = 0;
    bool closed_ = false;
    std::deque<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
};
}
