#include <linkerbot/modbus/rtu.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace linkerbot::modbus {
    namespace {
        using transport::Clock;
        bool is_crc_error(const std::exception_ptr &error) {
            if (!error) {
                return false;
            }
            try {
                std::rethrow_exception(error);
            } catch (const CrcError &) {
                return true;
            } catch (...) {
                return false;
            }
        }
        void check_options(const TransactionOptions &options) {
            options.cancellation.throw_if_cancelled();
            if (Clock::now() >= options.deadline) {
                throw serial::TimeoutError("RTU transaction deadline expired");
            }
        }
        void receive_exact(serial::ByteStream &stream, Bytes &bytes, size_t total,
                           const TransactionOptions &options, Timing timing) {
            if (total > 256) {
                throw ProtocolError("RTU response exceeds 256 bytes");
            }
            while (bytes.size() < total) {
                check_options(options);
                uint8_t buffer[256]{};
                const bool started = !bytes.empty();
                const auto deadline =
                    started ? std::min(options.deadline, Clock::now() + timing.inter_character)
                            : options.deadline;
                const auto count =
                    stream.read_some(buffer, total - bytes.size(), deadline, options.cancellation);
                if (count == 0) {
                    throw serial::TimeoutError(
                        "RTU response timed out or has an inter-character gap");
                }
                if (count > total - bytes.size()) {
                    throw ProtocolError("Serial backend exceeded read capacity");
                }
                bytes.insert(bytes.end(), buffer, buffer + count);
            }
        }
    } // namespace

    struct RtuSession::Impl {
        RtuConfig config;
        Factory factory;
        Timing timing;
        std::shared_ptr<serial::ByteStream> stream;
        mutable std::mutex mutex;
        std::mutex lifecycle;
        std::condition_variable changed;
        std::deque<std::shared_ptr<int>> queue;
        bool active = false;
        std::thread::id active_thread;
        RtuHealth health;

        Impl(RtuConfig config_value, Factory factory_value)
            : config(std::move(config_value)), factory(std::move(factory_value)),
              timing(rtu_timing(config.serial)) {
            if (!factory || config.queue_capacity == 0) {
                throw std::invalid_argument(
                    "RTU requires a stream factory and a positive queue capacity");
            }
            stream = factory(config.serial);
            if (!stream) {
                throw std::invalid_argument("RTU stream factory returned null");
            }
            health.open = true;
        }
        void require_ready() const {
            if (!health.open) {
                throw serial::ClosedError("RTU session closed");
            }
            if (health.faulted || health.recovering) {
                throw FaultedError("RTU session requires recovery");
            }
        }
        void require_external() const {
            if (active && active_thread == std::this_thread::get_id()) {
                throw std::logic_error("RTU close/recover must run outside serial callbacks");
            }
        }
        TransactionResult execute(const Request &request, const Bytes &wire,
                                  const TransactionOptions &options, Deadline accepted_at) {
            TransactionResult result;
            result.generation =
                health.generation; // Active operation excludes recovery publication.
            result.accepted_at = accepted_at;
            bool attempted = false;
            bool desynchronized = false;
            try {
                // Reject queued or delayed bytes rather than assigning them to a new request.
                uint8_t unexpected = 0;
                check_options(options);
                const auto idle_until = Clock::now() + timing.inter_frame;
                if (idle_until >= options.deadline) {
                    throw serial::TimeoutError("No budget for RTU frame interval");
                }
                if (stream->read_some(&unexpected, 1, idle_until, options.cancellation) != 0) {
                    desynchronized = true;
                    throw ProtocolError(
                        "Unexpected input before RTU request; line requires recovery");
                }
                while (result.bytes_sent < wire.size()) {
                    check_options(options);
                    attempted = true; // A throwing driver call may already have sent a prefix.
                    const auto count = stream->write_some(wire.data() + result.bytes_sent,
                                                          wire.size() - result.bytes_sent,
                                                          options.deadline, options.cancellation);
                    if (count == 0 || count > wire.size() - result.bytes_sent) {
                        throw ProtocolError("Serial backend returned an invalid write count");
                    }
                    result.bytes_sent += count;
                }
                stream->drain_output_until(options.deadline, options.cancellation);
                result.request_dispatched = true;
                if (config.discard_local_echo) {
                    Bytes echo;
                    receive_exact(*stream, echo, wire.size(), options, timing);
                    if (echo != wire) {
                        throw ProtocolError("RTU local echo does not match request");
                    }
                }
                Bytes response;
                receive_exact(*stream, response, 3, options, timing);
                size_t total = 8;
                if ((response[1] & 0x80U) != 0) {
                    total = 5;
                } else if (response[1] == 3 || response[1] == 4) {
                    total = size_t{response[2]} + 5;
                }
                receive_exact(*stream, response, total, options, timing);
                result.data = decode_response(request, response);
                check_options(options);
                result.outcome = Outcome::kResponse;
            } catch (const ExceptionResponse &) {
                result.outcome = Outcome::kException;
                result.error = std::current_exception();
            } catch (const transport::CancelledError &) {
                result.outcome = Outcome::kCancelled;
                result.error = std::current_exception();
                desynchronized = desynchronized || attempted;
            } catch (const serial::TimeoutError &) {
                result.outcome = Outcome::kExpired;
                result.error = std::current_exception();
                desynchronized = desynchronized || attempted;
            } catch (...) {
                result.outcome = Outcome::kFailed;
                result.error = std::current_exception();
                desynchronized = true;
            }
            result.delivery_uncertain = attempted && result.outcome != Outcome::kResponse &&
                                        result.outcome != Outcome::kException;
            result.completed_at = Clock::now();
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if ((!health.open || health.recovering) && !result.error) {
                    try {
                        throw serial::ClosedError("RTU generation ended before result publication");
                    } catch (...) {
                        result.error = std::current_exception();
                    }
                    result.outcome = Outcome::kFailed;
                    result.delivery_uncertain = attempted;
                }
                health.faulted = health.faulted || desynchronized;
                ++health.transactions;
                if (result.outcome == Outcome::kException) {
                    ++health.exceptions;
                }
                if (result.outcome == Outcome::kExpired) {
                    ++health.timeouts;
                }
                if (is_crc_error(result.error)) {
                    ++health.crc_errors;
                }
            }
            return result;
        }
    };
    RtuSession::RtuSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<RtuSession> RtuSession::create(RtuConfig config, Factory factory) {
        return std::shared_ptr<RtuSession>(
            new RtuSession(std::make_unique<Impl>(std::move(config), std::move(factory))));
    }
    RtuSession::~RtuSession() {
        try {
            close();
        } catch (...) {
            // Explicit close reports driver cleanup errors; destruction cannot.
            return;
        }
    }
    TransactionResult RtuSession::transact(const Request &request, TransactionOptions options) {
        const auto wire = encode_request(request);
        if (options.deadline == Deadline::max()) {
            throw std::invalid_argument("RTU transactions require a finite deadline");
        }
        check_options(options);
        const auto ticket = std::make_shared<int>(0);
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->require_ready();
        if (impl_->queue.size() >= impl_->config.queue_capacity) {
            throw QueueFullError("RTU waiting queue is full");
        }
        const auto accepted_at = Clock::now();
        impl_->queue.push_back(ticket);
        impl_->health.pending = impl_->queue.size();
        impl_->health.high_water = std::max(impl_->health.high_water, impl_->health.pending);
        try {
            while (impl_->active || impl_->queue.front() != ticket) {
                impl_->require_ready();
                check_options(options);
                impl_->changed.wait_until(
                    lock, std::min(options.deadline, Clock::now() + std::chrono::milliseconds{5}));
            }
            impl_->require_ready();
            check_options(options);
        } catch (...) {
            const auto found = std::find(impl_->queue.begin(), impl_->queue.end(), ticket);
            impl_->queue.erase(found);
            impl_->health.pending = impl_->queue.size();
            impl_->changed.notify_all();
            throw;
        }
        impl_->queue.pop_front();
        impl_->health.pending = impl_->queue.size();
        impl_->active = true;
        impl_->active_thread = std::this_thread::get_id();
        lock.unlock();
        // execute catches all I/O/codec errors so ownership is always released.
        auto result = impl_->execute(request, wire, options, accepted_at);
        lock.lock();
        impl_->active = false;
        impl_->active_thread = {};
        impl_->changed.notify_all();
        return result;
    }
    RtuHealth RtuSession::health() const {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->health;
    }
    uint64_t RtuSession::recover(uint64_t observed_generation) {
        {
            const std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->require_external();
        }
        const std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->require_external();
        if (!impl_->health.open) {
            throw serial::ClosedError("Cannot recover a closed RTU session");
        }
        if (impl_->health.generation != observed_generation && !impl_->health.faulted) {
            return impl_->health.generation;
        }
        if (impl_->health.generation == std::numeric_limits<uint64_t>::max()) {
            throw FaultedError("RTU generation exhausted");
        }
        impl_->health.recovering = true;
        impl_->health.faulted = true;
        impl_->changed.notify_all();
        // Lifecycle ownership keeps the stream alive while backend diagnostics
        // re-enter health(). Never invoke application code under the state lock.
        lock.unlock();
        impl_->stream->cancel();
        lock.lock();
        impl_->changed.wait(lock, [&] { return !impl_->active && impl_->queue.empty(); });
        lock.unlock();
        try {
            impl_->stream->close();
            auto stream = impl_->factory(impl_->config.serial);
            if (!stream) {
                throw std::invalid_argument("RTU stream factory returned null during recovery");
            }
            lock.lock();
            impl_->stream = std::move(stream);
            ++impl_->health.generation;
            ++impl_->health.recoveries;
            impl_->health.recovering = false;
            impl_->health.faulted = false;
        } catch (...) {
            if (!lock.owns_lock()) {
                lock.lock();
            }
            impl_->health.recovering = false;
            impl_->changed.notify_all();
            throw;
        }
        impl_->changed.notify_all();
        return impl_->health.generation;
    }
    void RtuSession::close() {
        {
            const std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->require_external();
        }
        const std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->require_external();
        impl_->health.open = false;
        impl_->changed.notify_all();
        lock.unlock();
        impl_->stream->cancel();
        lock.lock();
        impl_->changed.wait(lock, [&] { return !impl_->active && impl_->queue.empty(); });
        lock.unlock();
        impl_->stream->close();
    }

    Client::Client(std::shared_ptr<RtuSession> session, uint8_t unit_id)
        : session_(std::move(session)), unit_id_(unit_id) {
        if (!session_ || unit_id == 0 || unit_id > 247) {
            throw std::invalid_argument("Modbus client requires a session and a unit ID in 1..247");
        }
    }
    std::vector<uint16_t> Client::read(uint8_t function, uint16_t address, uint16_t count,
                                       TransactionOptions options) {
        auto result = session_->transact({unit_id_, function, address, count, {}}, options);
        result.rethrow_if_error();
        std::vector<uint16_t> values;
        for (size_t offset = 0; offset < result.data.size(); offset += 2) {
            values.push_back(static_cast<uint16_t>((uint16_t{result.data[offset]} << 8) |
                                                   result.data[offset + 1]));
        }
        return values;
    }
    std::vector<uint16_t> Client::read_holding_registers(uint16_t address, uint16_t count,
                                                         TransactionOptions options) {
        return read(3, address, count, options);
    }
    std::vector<uint16_t> Client::read_input_registers(uint16_t address, uint16_t count,
                                                       TransactionOptions options) {
        return read(4, address, count, options);
    }
    TransactionResult Client::write_single_register(uint16_t address, uint16_t value,
                                                    TransactionOptions options) {
        return session_->transact({unit_id_, 6, address, 1, {value}}, options);
    }
    TransactionResult Client::write_multiple_registers(uint16_t address,
                                                       std::vector<uint16_t> values,
                                                       TransactionOptions options) {
        if (values.empty() || values.size() > 123) {
            throw std::invalid_argument("Modbus write requires 1..123 registers");
        }
        const auto count = static_cast<uint16_t>(values.size());
        return session_->transact({unit_id_, 16, address, count, std::move(values)}, options);
    }
} // namespace linkerbot::modbus
