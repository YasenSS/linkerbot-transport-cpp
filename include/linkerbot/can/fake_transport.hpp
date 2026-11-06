#pragma once

#include <linkerbot/can/frame.hpp>
#include <linkerbot/can/message.hpp>
#include <linkerbot/can/transport.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace linkerbot::can {

    // In-memory transport for unit tests. Replies are matched by arbitration ID.
    //
    // Handlers run inline on the writer's thread while the internal lock is held,
    // so a handler must not call back into the transport.
    class FakeTransport : public CANTransport {
      public:
        using Handler = std::function<std::vector<Message>(const Message &)>;

        FakeTransport() = default;

        ~FakeTransport() override {
            try {
                {
                    const std::scoped_lock lock(mutex_);
                    fail_close_count_ = 0;
                }
                FakeTransport::close();
            } catch (...) { // NOLINT(bugprone-empty-catch) destructor must not throw
            }
        }

        FakeTransport(const FakeTransport &) = delete;
        FakeTransport &operator=(const FakeTransport &) = delete;

        auto on_request(uint32_t id, Handler handler) -> void {
            const std::scoped_lock lock(mutex_);
            handlers_[id] = std::move(handler);
        }

        // Echo: for sense (dlc==1) reply cmd + n fill bytes; for control echo payload.
        auto echo_cmd(uint32_t id, uint8_t cmd, size_t n, uint8_t fill = 0) -> void {
            if (n > kMaxDataLen - 1) {
                throw ValidationError("fake echo reply payload exceeds 64 bytes");
            }
            on_request(id, [id, cmd, n, fill](const Message &req) -> std::vector<Message> {
                const auto flags = resolve_tx_flags_with_default(LinkKind::kCanFd, req, false);
                if (req.dlc < 1 || req.data[0] != cmd) {
                    return {};
                }
                if (!flags.is_fd && 1 + n > 8) {
                    throw ValidationError("fake echo reply exceeds classic CAN payload length");
                }
                Message reply;
                reply.id = id;
                reply.is_extended = req.is_extended;
                reply.is_fd = flags.is_fd;
                reply.is_brs = flags.is_brs;
                reply.data[0] = cmd;
                if (req.dlc > 1) {
                    const size_t copy_n = std::min<size_t>(req.dlc - 1, n);
                    for (size_t i = 0; i < copy_n; ++i) {
                        reply.data[i + 1] = req.data[i + 1];
                    }
                    for (size_t i = copy_n; i < n; ++i) {
                        reply.data[i + 1] = fill;
                    }
                } else {
                    for (size_t i = 0; i < n; ++i) {
                        reply.data[i + 1] = fill;
                    }
                }
                reply.dlc = static_cast<uint8_t>(1 + n);
                return {reply};
            });
        }

        // Queues an unsolicited frame, as if the device had pushed it.
        auto inject(const Message &msg) -> void {
            const std::scoped_lock lock(mutex_);
            if (closed_) {
                return;
            }
            inbox_.push_back(msg);
            cv_.notify_one();
        }

        // Every frame written so far, in order — the test's assertion surface.
        [[nodiscard]] auto sent() const -> std::vector<Message> {
            const std::scoped_lock lock(mutex_);
            return sent_;
        }

        auto clear_sent() -> void {
            const std::scoped_lock lock(mutex_);
            sent_.clear();
        }

        auto fail_next_closes(int count) -> void {
            const std::scoped_lock lock(mutex_);
            fail_close_count_ = count;
        }

        auto fail_writes(std::string message) -> void {
            const std::scoped_lock lock(mutex_);
            write_error_.emplace(std::move(message));
        }

        auto fail_reads(std::string message) -> void {
            {
                const std::scoped_lock lock(mutex_);
                read_error_.emplace(std::move(message));
            }
            cv_.notify_all();
        }

        auto fail_batches(std::string message) -> void {
            const std::scoped_lock lock(mutex_);
            batch_error_.emplace(std::move(message));
        }

        auto set_prefer_batch(bool enabled) -> void {
            const std::scoped_lock lock(mutex_);
            prefer_batch_ = enabled;
        }

        [[nodiscard]] auto prefers_write_batch() const -> bool override {
            const std::scoped_lock lock(mutex_);
            return prefer_batch_;
        }

        [[nodiscard]] auto batch_writes() const -> std::vector<std::vector<Message>> {
            const std::scoped_lock lock(mutex_);
            return batches_;
        }

        [[nodiscard]] auto close_calls() const -> int {
            const std::scoped_lock lock(mutex_);
            return close_calls_;
        }

        auto arm_tx_gate() -> void {
            const std::scoped_lock lock(tx_gate_.mutex);
            tx_gate_.armed = true;
            tx_gate_.released = false;
            tx_gate_.entered = 0;
        }

        auto wait_until_tx_entered(std::chrono::milliseconds timeout) -> bool {
            std::unique_lock lock(tx_gate_.mutex);
            return tx_gate_.entered_cv.wait_for(lock, timeout,
                                                [this] { return tx_gate_.entered > 0; });
        }

        auto release_tx_gate() -> void {
            const std::scoped_lock lock(tx_gate_.mutex);
            tx_gate_.released = true;
            tx_gate_.release_cv.notify_all();
        }

        auto read() -> std::optional<Message> override {
            std::unique_lock lock(mutex_);
            if (read_error_) {
                throw TransportIOError(*read_error_);
            }
            if (closed_) {
                throw ClosedError("fake transport is closed");
            }
            if (inbox_.empty()) {
                cv_.wait_for(lock, std::chrono::milliseconds(10),
                             [this] { return closed_ || !inbox_.empty() || read_error_; });
            }
            if (read_error_) {
                throw TransportIOError(*read_error_);
            }
            if (closed_) {
                throw ClosedError("fake transport is closed");
            }
            if (inbox_.empty()) {
                return std::nullopt;
            }
            auto msg = inbox_.front();
            inbox_.pop_front();
            return msg;
        }

        auto write(const Message &message) -> void override {
            wait_tx_gate();
            std::vector<Message> replies;
            {
                const std::scoped_lock lock(mutex_);
                if (closed_) {
                    throw ClosedError("fake transport is closed");
                }
                if (write_error_) {
                    throw TransportIOError(*write_error_);
                }
                record_write_locked(message, replies);
            }
            if (!replies.empty()) {
                cv_.notify_one();
            }
        }

        auto write_batch(const std::vector<Message> &messages) -> void override {
            if (messages.empty()) {
                return;
            }
            wait_tx_gate();
            std::vector<Message> replies;
            {
                const std::scoped_lock lock(mutex_);
                if (closed_) {
                    throw ClosedError("fake transport is closed");
                }
                if (batch_error_) {
                    throw TransportIOError(*batch_error_);
                }
                batches_.push_back(messages);
                for (const auto &message : messages) {
                    if (write_error_) {
                        throw TransportIOError(*write_error_);
                    }
                    record_write_locked(message, replies);
                }
            }
            if (!replies.empty()) {
                cv_.notify_one();
            }
        }

        auto close() -> void override {
            bool should_fail = false;
            {
                const std::scoped_lock lock(mutex_);
                ++close_calls_;
                if (fail_close_count_ > 0) {
                    --fail_close_count_;
                    should_fail = true;
                } else {
                    closed_ = true;
                }
            }
            cv_.notify_all();
            release_tx_gate();
            if (should_fail) {
                throw TransportIOError("close failed");
            }
        }

      private:
        auto wait_tx_gate() -> void {
            std::unique_lock lock(tx_gate_.mutex);
            if (!tx_gate_.armed) {
                return;
            }
            if (tx_gate_.entered == 0) {
                ++tx_gate_.entered;
                tx_gate_.entered_cv.notify_all();
                tx_gate_.release_cv.wait(lock, [this] { return tx_gate_.released; });
            }
        }

        auto record_write_locked(const Message &message, std::vector<Message> &replies) -> void {
            sent_.push_back(message);
            auto it = handlers_.find(message.id);
            if (it != handlers_.end()) {
                auto produced = it->second(message);
                replies.insert(replies.end(), produced.begin(), produced.end());
                for (const auto &reply : produced) {
                    inbox_.push_back(reply);
                }
            }
        }

        struct TxGate {
            std::mutex mutex;
            std::condition_variable entered_cv;
            std::condition_variable release_cv;
            bool armed = false;
            bool released = false;
            int entered = 0;
        };

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        bool closed_ = false;
        int fail_close_count_ = 0;
        int close_calls_ = 0;
        bool prefer_batch_ = false;
        std::optional<std::string> write_error_;
        std::optional<std::string> read_error_;
        std::optional<std::string> batch_error_;
        std::deque<Message> inbox_;
        std::vector<Message> sent_;
        std::vector<std::vector<Message>> batches_;
        std::unordered_map<uint32_t, Handler> handlers_;
        TxGate tx_gate_;
    };

} // namespace linkerbot::can
