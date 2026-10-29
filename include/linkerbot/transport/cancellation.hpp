#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace linkerbot::transport {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

class CancelledError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CancellationToken {
public:
    CancellationToken() = default;
    [[nodiscard]] bool cancelled() const noexcept {
        return state_ && state_->load(std::memory_order_acquire);
    }
    void throw_if_cancelled() const {
        if (cancelled()) throw CancelledError("operation cancelled");
    }
private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> state)
        : state_(std::move(state)) {}
    std::shared_ptr<std::atomic<bool>> state_;
};

class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<std::atomic<bool>>(false)) {}
    [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }
    void cancel() noexcept { state_->store(true, std::memory_order_release); }
private:
    std::shared_ptr<std::atomic<bool>> state_;
};
} // namespace linkerbot::transport
