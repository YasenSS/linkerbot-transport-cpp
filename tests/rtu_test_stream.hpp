#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <linkerbot/modbus/rtu.hpp>

namespace linkerbot::test_support {
    // A stream reply is injected only after a full request is drained. Reads and
    // writes fragment independently, exercising framing rather than packet I/O.
    struct RtuWire {
        std::mutex mutex;
        std::condition_variable changed;
        modbus::Bytes sent;
        modbus::Bytes current;
        std::deque<uint8_t> input;
        size_t read_chunk = 256;
        size_t write_chunk = 256;
        size_t writes = 0;
        bool closed = false;
        bool cancelled = false;
        bool block_drain = false;
        bool fail_write = false;
        bool zero_write = false;
        bool oversized_write = false;
        bool oversized_read = false;
        std::function<modbus::Bytes(const modbus::Bytes &)> reply;
    };
    class RtuStream final : public serial::ByteStream {
        std::shared_ptr<RtuWire> wire_;

      public:
        explicit RtuStream(std::shared_ptr<RtuWire> wire) : wire_(std::move(wire)) {}
        size_t read_some(uint8_t *data, size_t capacity, transport::Deadline deadline,
                         transport::CancellationToken token) override {
            std::unique_lock<std::mutex> lock(wire_->mutex);
            while (wire_->input.empty()) {
                token.throw_if_cancelled();
                if (wire_->closed || wire_->cancelled) {
                    throw serial::ClosedError("fake stream closed");
                }
                if (transport::Clock::now() >= deadline) {
                    return 0;
                }
                wire_->changed.wait_until(
                    lock,
                    std::min(deadline, transport::Clock::now() + std::chrono::milliseconds{1}));
            }
            if (wire_->oversized_read) {
                return capacity + 1;
            }
            const auto count = std::min({capacity, wire_->input.size(), wire_->read_chunk});
            for (size_t index = 0; index < count; ++index) {
                data[index] = wire_->input.front();
                wire_->input.pop_front();
            }
            return count;
        }
        size_t write_some(const uint8_t *data, size_t size, transport::Deadline deadline,
                          transport::CancellationToken token) override {
            token.throw_if_cancelled();
            if (transport::Clock::now() >= deadline) {
                throw serial::TimeoutError("fake write deadline");
            }
            const std::lock_guard<std::mutex> lock(wire_->mutex);
            if (wire_->closed || wire_->cancelled) {
                throw serial::ClosedError("fake stream closed");
            }
            ++wire_->writes;
            wire_->changed.notify_all();
            if (wire_->fail_write) {
                throw serial::IOError("fake partial write");
            }
            if (wire_->zero_write) {
                return 0;
            }
            if (wire_->oversized_write) {
                return size + 1;
            }
            const auto count = std::min(size, wire_->write_chunk);
            wire_->current.insert(wire_->current.end(), data, data + count);
            wire_->sent.insert(wire_->sent.end(), data, data + count);
            return count;
        }
        void drain_output_until(transport::Deadline deadline,
                                transport::CancellationToken token) override {
            std::unique_lock<std::mutex> lock(wire_->mutex);
            wire_->changed.notify_all();
            while (wire_->block_drain) {
                token.throw_if_cancelled();
                if (wire_->closed || wire_->cancelled) {
                    throw serial::ClosedError("fake drain cancelled");
                }
                if (transport::Clock::now() >= deadline) {
                    throw serial::TimeoutError("fake drain deadline");
                }
                wire_->changed.wait_for(lock, std::chrono::milliseconds{1});
            }
            auto reply = wire_->reply;
            auto request = std::move(wire_->current);
            wire_->current.clear();
            lock.unlock();
            const auto response = reply ? reply(request) : modbus::Bytes{};
            lock.lock();
            wire_->input.insert(wire_->input.end(), response.begin(), response.end());
            wire_->changed.notify_all();
        }
        void cancel() noexcept override {
            const std::lock_guard<std::mutex> lock(wire_->mutex);
            wire_->cancelled = true;
            wire_->changed.notify_all();
        }
        void close() override {
            const std::lock_guard<std::mutex> lock(wire_->mutex);
            wire_->closed = true;
            wire_->changed.notify_all();
        }
    };
    inline modbus::Bytes with_crc(modbus::Bytes data) {
        const auto crc = modbus::crc16(data.data(), data.size());
        data.push_back(static_cast<uint8_t>(crc & 0xffU));
        data.push_back(static_cast<uint8_t>(crc >> 8));
        return data;
    }
    inline auto rtu_config() -> modbus::RtuConfig {
        modbus::RtuConfig config;
        config.serial.port = "fake";
        return config;
    }
    inline auto options(std::chrono::milliseconds timeout = std::chrono::milliseconds{
                            500}) -> modbus::TransactionOptions {
        return {transport::Clock::now() + timeout, {}};
    }
    inline auto make_session(const std::shared_ptr<RtuWire> &wire,
                             modbus::RtuConfig config = rtu_config()) {
        return modbus::RtuSession::create(std::move(config), [wire](const serial::Config &) {
            return std::make_unique<RtuStream>(wire);
        });
    }
    inline auto normal_reply(const modbus::Bytes &request) -> modbus::Bytes {
        if (request[1] == 3 || request[1] == 4) {
            modbus::Bytes response{request[0], request[1], static_cast<uint8_t>(request[5] * 2)};
            for (unsigned i = 0; i < request[5]; ++i) {
                response.push_back(0x12);
                response.push_back(static_cast<uint8_t>(0x30U + i));
            }
            return with_crc(std::move(response));
        }
        return with_crc({request.begin(), request.begin() + 6});
    }
} // namespace linkerbot::test_support
