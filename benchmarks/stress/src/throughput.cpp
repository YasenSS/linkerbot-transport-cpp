// Linux-only offline load generator. The bounded echo transport deliberately
// keeps no send history, so the harness does not accumulate memory with runtime.
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/can/socketcan_transport.hpp>
#include <linkerbot/transport/session.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
namespace can = linkerbot::can;
namespace tr = linkerbot::transport;
using Clock = std::chrono::steady_clock;

namespace {
    void check(bool value, const char *message) {
        if (!value)
            throw std::runtime_error(message);
    }

    class EchoTransport final : public can::CANTransport {
      public:
        explicit EchoTransport(bool native_batch) : native_batch_(native_batch) {}
        std::optional<can::Message> read() override {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, 10ms, [&] { return closed_ || count_ != 0; });
            if (closed_)
                throw can::ClosedError("echo closed");
            if (count_ == 0)
                return std::nullopt;
            auto frame = ring_[head_];
            head_ = (head_ + 1) % ring_.size();
            --count_;
            return frame;
        }
        void write(const can::Message &frame) override {
            {
                std::lock_guard lock(mutex_);
                push(frame);
            }
            cv_.notify_one();
        }
        void write_batch(const std::vector<can::Message> &frames) override {
            {
                std::lock_guard lock(mutex_);
                for (const auto &frame : frames)
                    push(frame);
            }
            cv_.notify_one();
        }
        bool prefers_write_batch() const override {
            return native_batch_;
        }
        void close() override {
            {
                std::lock_guard lock(mutex_);
                closed_ = true;
            }
            cv_.notify_all();
        }

      private:
        void push(const can::Message &frame) {
            if (closed_)
                throw can::ClosedError("echo closed");
            if (count_ == ring_.size())
                throw can::QueueFullError("echo ring full");
            ring_[(head_ + count_) % ring_.size()] = frame;
            ++count_;
        }
        std::array<can::Message, 8192> ring_{};
        size_t head_ = 0;
        size_t count_ = 0;
        bool closed_ = false;
        bool native_batch_;
        std::mutex mutex_;
        std::condition_variable cv_;
    };

    uint64_t rss_kib() {
        std::ifstream input("/proc/self/statm");
        uint64_t virtual_pages = 0, resident_pages = 0;
        check(static_cast<bool>(input >> virtual_pages >> resident_pages), "cannot read RSS");
        return resident_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) / 1024;
    }

    uint64_t cpu_ns() {
        timespec time{};
        check(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) == 0, "cannot read CPU time");
        return static_cast<uint64_t>(time.tv_sec) * 1000000000ULL + time.tv_nsec;
    }

    struct Worker {
        // 10 us histogram buckets, capped at 200 ms. Max is tracked separately.
        std::vector<uint64_t> histogram = std::vector<uint64_t>(20001, 0);
        std::atomic<uint64_t> received{0};
        uint64_t sent = 0;
        uint64_t max_us = 0;
    };

    can::Message plain_frame(uint32_t id) {
        can::Message frame;
        frame.id = id;
        frame.dlc = 8;
        return frame;
    }

    void contracts() {
        // Deterministic overload, rather than relying on scheduler timing.
        can::FakeTransport *fake = nullptr;
        auto session = tr::Session::create(
            [&] {
                auto transport = std::make_unique<can::FakeTransport>();
                fake = transport.get();
                return transport;
            },
            128);
        fake->arm_tx_gate();
        auto active = session->submit({{plain_frame(0x100)}});
        if (!fake->wait_until_tx_entered(2s)) {
            fake->release_tx_gate();
            throw std::runtime_error("writer did not enter gate");
        }
        std::vector<tr::SendReceipt> pending;
        size_t rejected = 0;
        try {
            for (size_t i = 0; i < 1000; ++i) {
                try {
                    pending.push_back(session->submit({{plain_frame(0x100)}}));
                } catch (const can::QueueFullError &) {
                    ++rejected;
                }
            }
        } catch (...) {
            fake->release_tx_gate();
            throw;
        }
        fake->release_tx_gate();
        check(pending.size() == 128 && rejected == 872, "TX capacity accounting failed");
        check(active.wait_until(Clock::now() + 5s).sent_frames == 1, "active send failed");
        for (auto &receipt : pending)
            check(receipt.wait_until(Clock::now() + 5s).sent_frames == 1, "queued send failed");
        check(session->health().pending_tx == 0, "TX did not drain");

        auto inbox = session->subscribe_routes({{0x101, false}}, 16);
        auto barrier = session->subscribe_routes({{0x102, false}}, 1);
        for (size_t i = 0; i < 1000; ++i)
            fake->inject(plain_frame(0x101));
        fake->inject(plain_frame(0x102));
        check(barrier->receive_until(Clock::now() + 5s).has_value(), "RX barrier timed out");
        check(inbox->health().overflow == 984 && inbox->health().pending == 16,
              "RX overflow accounting failed");
        size_t drained = 0;
        while (inbox->try_receive())
            ++drained;
        check(drained == 16, "RX bounded inbox drain failed");
        session->close();

        // Recreate transports after injected write failures. Raw pointer is used
        // only while its generation is live; recover replaces it synchronously.
        auto recoverable = tr::Session::create([&] {
            auto transport = std::make_unique<can::FakeTransport>();
            transport->on_request(
                0x100, [](const can::Message &m) { return std::vector<can::Message>{m}; });
            fake = transport.get();
            return transport;
        });
        for (size_t i = 0; i < 100; ++i) {
            auto route = recoverable->subscribe_routes({{0x100, false}});
            auto good = recoverable->submit({{plain_frame(0x100)}});
            check(good.wait_until(Clock::now() + 2s).sent_frames == 1, "recovered send failed");
            check(route->receive_until(Clock::now() + 2s).has_value(), "recovered reply failed");
            const auto generation = recoverable->health().generation;
            fake->fail_writes("offline stress injected write failure");
            auto bad = recoverable->submit({{plain_frame(0x100)}});
            bool failed = false;
            try {
                (void)bad.wait_until(Clock::now() + 2s);
            } catch (const can::TransportIOError &) {
                failed = true;
            }
            check(failed && bad.result().outcome == tr::SendOutcome::failed,
                  "write failure was not reported");
            check(recoverable->recover(generation) > generation, "generation did not advance");
            check(route->health().closed, "recovery did not close old route");
        }
        check(recoverable->health().recoveries == 100, "recovery count mismatch");
        recoverable->close();
        tr::Session::drain_deferred_cleanup();
        std::cout << "{\"pass\":true,\"tx_accepted\":129,\"tx_rejected\":872,"
                     "\"rx_kept\":16,\"rx_overflow\":984,\"recoveries\":100}\n";
    }
} // namespace

int main(int argc, char **argv) {
    try {
        size_t threads = 8, batch_size = 8, window = 8;
        int seconds = 30;
        bool fd = false, native_batch = false;
        std::string samples_path, vcan_interface;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--contracts") {
                contracts();
                return 0;
            }
            if (arg == "--fd") {
                fd = true;
                continue;
            }
            if (arg == "--native-batch") {
                native_batch = true;
                continue;
            }
            check(i + 1 < argc, "missing argument value");
            const std::string value = argv[++i];
            if (arg == "--seconds")
                seconds = std::stoi(value);
            else if (arg == "--threads")
                threads = std::stoul(value);
            else if (arg == "--batch")
                batch_size = std::stoul(value);
            else if (arg == "--window")
                window = std::stoul(value);
            else if (arg == "--samples")
                samples_path = value;
            else if (arg == "--vcan")
                vcan_interface = value;
            else
                throw std::runtime_error("unknown argument: " + arg);
        }
        check(seconds > 0 && seconds <= 3600, "seconds must be 1..3600");
        check(threads > 0 && threads <= 64, "threads must be 1..64");
        check(batch_size > 0 && batch_size <= 128 && window > 0 && window <= 32,
              "batch must be 1..128 and window must be 1..32");
        check(threads * batch_size * window <= 4096 && threads * window <= 1024,
              "outstanding traffic exceeds bounded test capacity");
        check(vcan_interface.empty() || vcan_interface == "vcan0",
              "virtual mode permits only vcan0; use a dedicated network namespace");
        check(vcan_interface.empty() || !native_batch,
              "SocketCAN uses individual frame writes; --native-batch is in-memory only");
        std::ofstream samples;
        if (!samples_path.empty()) {
            samples.open(samples_path);
            check(samples.good(), "cannot open sample output");
            samples << "elapsed_s,received_frames,rss_kib,cpu_ns,pending_tx,queue_high_water\n";
        }
        auto session = tr::Session::create([&] {
            if (!vcan_interface.empty())
                return std::unique_ptr<can::CANTransport>(
                    std::make_unique<can::SocketCANTransport>(vcan_interface, fd));
            return std::unique_ptr<can::CANTransport>(
                std::make_unique<EchoTransport>(native_batch));
        });
        std::vector<std::shared_ptr<tr::RouteInbox>> inboxes;
        for (size_t t = 0; t < threads; ++t)
            inboxes.push_back(session->subscribe_routes(
                {{static_cast<uint32_t>((vcan_interface.empty() ? 0x100 : 0x500) + t), false}},
                std::max<size_t>(128, window * batch_size)));
        std::vector<Worker> workers(threads);
        std::atomic<bool> go{false}, failed{false};
        Clock::time_point start;
        std::string error;
        std::mutex error_mutex;
        std::atomic<bool> peer_stop{false};
        std::atomic<uint64_t> peer_received{0}, peer_sent{0};
        std::unique_ptr<can::SocketCANTransport> peer;
        std::thread peer_thread;
        // The dedicated namespace contains just these two CAN sockets. Raw CAN
        // defaults exclude a socket's own TX, so requests and replies do not loop.
        if (!vcan_interface.empty()) {
            peer = std::make_unique<can::SocketCANTransport>(vcan_interface, fd);
            peer_thread = std::thread([&] {
                try {
                    while (!peer_stop.load()) {
                        auto frame = peer->read();
                        if (!frame)
                            continue;
                        check(frame->id >= 0x100 && frame->id < 0x100 + threads,
                              "unexpected frame on dedicated vcan interface");
                        ++peer_received;
                        frame->id += 0x400;
                        peer->write(*frame);
                        ++peer_sent;
                    }
                } catch (const std::exception &exception) {
                    failed.store(true);
                    std::lock_guard lock(error_mutex);
                    if (error.empty())
                        error = exception.what();
                }
            });
        }
        std::vector<std::thread> senders;
        for (size_t t = 0; t < threads; ++t) {
            senders.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(1ms);
                try {
                    uint32_t next_send = 0, next_receive = 0;
                    std::vector<tr::SendReceipt> receipts;
                    receipts.reserve(window);
                    while (!failed.load() && Clock::now() < start + std::chrono::seconds(seconds)) {
                        receipts.clear();
                        for (size_t w = 0; w < window; ++w) {
                            tr::MessageBatch batch;
                            batch.frames.reserve(batch_size);
                            for (size_t b = 0; b < batch_size; ++b) {
                                auto frame = plain_frame(static_cast<uint32_t>(0x100 + t));
                                frame.is_fd = fd;
                                frame.is_brs = fd;
                                frame.dlc = fd ? 64 : 8;
                                const uint32_t sequence = next_send++;
                                const auto stamp = static_cast<uint32_t>(
                                    std::chrono::duration_cast<std::chrono::microseconds>(
                                        Clock::now() - start)
                                        .count());
                                std::memcpy(frame.data.data(), &sequence, 4);
                                std::memcpy(frame.data.data() + 4, &stamp, 4);
                                for (size_t k = 8; k < frame.dlc; ++k)
                                    frame.data[k] = static_cast<uint8_t>(sequence + k + t);
                                batch.frames.push_back(frame);
                            }
                            receipts.push_back(session->submit(std::move(batch)));
                            workers[t].sent += batch_size;
                        }
                        for (auto &receipt : receipts) {
                            const auto result = receipt.wait_until(Clock::now() + 5s);
                            check(result.outcome == tr::SendOutcome::dispatched &&
                                      result.sent_frames == batch_size,
                                  "incomplete send receipt");
                        }
                        for (size_t n = 0; n < window * batch_size; ++n) {
                            auto routed = inboxes[t]->receive_until(Clock::now() + 5s);
                            check(routed.has_value(), "reply timeout");
                            const auto &frame = routed->message;
                            check(frame.id == (vcan_interface.empty() ? 0x100 : 0x500) + t &&
                                      !frame.is_extended && frame.dlc == (fd ? 64 : 8) &&
                                      frame.is_fd == fd && frame.is_brs == fd,
                                  "reply frame metadata corrupted");
                            uint32_t sequence = 0, stamp = 0;
                            std::memcpy(&sequence, frame.data.data(), 4);
                            std::memcpy(&stamp, frame.data.data() + 4, 4);
                            check(sequence == next_receive++, "lost, duplicate or reordered reply");
                            for (size_t k = 8; k < frame.dlc; ++k)
                                check(frame.data[k] == static_cast<uint8_t>(sequence + k + t),
                                      "reply payload corrupted");
                            const auto now_us = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                                      start)
                                    .count());
                            check(now_us >= stamp, "reply timestamp invalid");
                            const auto latency = now_us - stamp;
                            ++workers[t].histogram[std::min<uint64_t>(latency / 10, 20000)];
                            workers[t].max_us = std::max(workers[t].max_us, latency);
                            workers[t].received.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                } catch (const std::exception &exception) {
                    failed.store(true);
                    std::lock_guard lock(error_mutex);
                    if (error.empty())
                        error = exception.what();
                }
            });
        }
        const auto cpu_start = cpu_ns();
        start = Clock::now();
        go.store(true, std::memory_order_release);
        for (int second = 1; second <= seconds && !failed.load(); ++second) {
            std::this_thread::sleep_until(start + std::chrono::seconds(second));
            uint64_t received = 0;
            for (const auto &worker : workers)
                received += worker.received.load();
            const auto health = session->health();
            if (samples.is_open()) {
                samples << second << ',' << received << ',' << rss_kib() << ','
                        << cpu_ns() - cpu_start << ',' << health.pending_tx << ','
                        << health.queue_high_water << '\n';
                samples.flush();
            }
            if (second % 30 == 0)
                std::cerr << "progress " << second << '/' << seconds << "s, replies=" << received
                          << '\n';
        }
        for (auto &sender : senders)
            sender.join();
        const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        const auto cpu_elapsed = cpu_ns() - cpu_start;
        peer_stop.store(true);
        if (peer_thread.joinable())
            peer_thread.join();
        if (peer)
            peer->close();
        uint64_t sent = 0, received = 0, max_us = 0;
        std::vector<uint64_t> histogram(20001, 0);
        for (const auto &worker : workers) {
            sent += worker.sent;
            received += worker.received.load();
            max_us = std::max(max_us, worker.max_us);
            for (size_t k = 0; k < histogram.size(); ++k)
                histogram[k] += worker.histogram[k];
        }
        const auto health = session->health();
        bool complete = !failed.load() && sent > 0 && sent == received &&
                        sent == health.tx_frames && sent == health.rx_frames &&
                        (vcan_interface.empty() ||
                         (peer_received.load() == sent && peer_sent.load() == received)) &&
                        health.overflows == 0 && health.route_misses == 0 &&
                        health.invalid_frames == 0 && health.pending_tx == 0 &&
                        health.last_error.empty();
        uint64_t inbox_pending = 0, inbox_overflows = 0;
        for (const auto &inbox : inboxes) {
            inbox_pending += inbox->health().pending;
            inbox_overflows += inbox->health().overflow;
        }
        complete = complete && inbox_pending == 0 && inbox_overflows == 0;
        if (!complete)
            std::cerr << "FAIL: " << (error.empty() ? "frame accounting or health mismatch" : error)
                      << '\n';
        auto percentile = [&](uint64_t percent) {
            const auto target = (received * percent + 99) / 100;
            uint64_t count = 0;
            for (size_t k = 0; k < histogram.size(); ++k) {
                count += histogram[k];
                if (count >= target)
                    return k == 20000 ? max_us : static_cast<uint64_t>(k * 10 + 9);
            }
            return max_us;
        };
        const auto closing = Clock::now();
        session->close();
        tr::Session::drain_deferred_cleanup();
        const auto close_us =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - closing).count();
        std::cout << std::setprecision(12) << "{\"pass\":" << (complete ? "true" : "false")
                  << ",\"threads\":" << threads << ",\"batch\":" << batch_size
                  << ",\"window\":" << window << ",\"fd\":" << (fd ? "true" : "false")
                  << ",\"native_batch\":" << (native_batch ? "true" : "false") << ",\"backend\":\""
                  << (vcan_interface.empty() ? "memory" : "vcan") << "\""
                  << ",\"peer_received\":" << peer_received.load()
                  << ",\"peer_sent\":" << peer_sent.load() << ",\"seconds\":" << elapsed
                  << ",\"sent\":" << sent << ",\"received\":" << received
                  << ",\"frames_per_second\":" << static_cast<double>(received) / elapsed
                  << ",\"cpu_percent_one_core\":"
                  << static_cast<double>(cpu_elapsed) / (elapsed * 1e7)
                  << ",\"roundtrip_p50_us_upper\":" << percentile(50)
                  << ",\"roundtrip_p95_us_upper\":" << percentile(95)
                  << ",\"roundtrip_p99_us_upper\":" << percentile(99)
                  << ",\"roundtrip_max_us\":" << max_us
                  << ",\"queue_high_water\":" << health.queue_high_water
                  << ",\"overflows\":" << health.overflows << ",\"tx_frames\":" << health.tx_frames
                  << ",\"rx_frames\":" << health.rx_frames
                  << ",\"route_misses\":" << health.route_misses
                  << ",\"invalid_frames\":" << health.invalid_frames
                  << ",\"inbox_pending\":" << inbox_pending
                  << ",\"inbox_overflows\":" << inbox_overflows
                  << ",\"rss_final_kib\":" << rss_kib() << ",\"close_us\":" << close_us << "}\n";
        return complete ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
