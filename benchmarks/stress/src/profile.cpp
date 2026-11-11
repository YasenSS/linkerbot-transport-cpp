// External consumer of the local source tree selected by run.py.
#include <linkerbot/can/socketcan_transport.hpp>
#include <linkerbot/transport/session.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace can = linkerbot::can;
namespace tr = linkerbot::transport;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Distribution {
    std::vector<uint64_t> buckets = std::vector<uint64_t>(20001);
    uint64_t count = 0, max_us = 0, sum_us = 0;
    void add(uint64_t us) {
        ++buckets[std::min<uint64_t>(us / 10, 20000)];
        ++count;
        sum_us += us;
        max_us = std::max(max_us, us);
    }
    void merge(const Distribution &other) {
        for (size_t i = 0; i < buckets.size(); ++i)
            buckets[i] += other.buckets[i];
        count += other.count;
        sum_us += other.sum_us;
        max_us = std::max(max_us, other.max_us);
    }
    uint64_t quantile(unsigned permille) const {
        if (!count)
            return 0;
        const auto target = (count * permille + 999) / 1000;
        uint64_t n = 0;
        for (size_t i = 0; i < buckets.size(); ++i) {
            n += buckets[i];
            if (n >= target)
                return i == 20000 ? max_us : i * 10 + 9;
        }
        return max_us;
    }
};
uint64_t rss_kib() {
    std::ifstream input("/proc/self/statm");
    uint64_t total, resident;
    input >> total >> resident;
    return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) / 1024;
}
double time_us(const timeval &t) {
    return static_cast<double>(t.tv_sec) * 1e6 + static_cast<double>(t.tv_usec);
}
struct RawSocket {
    int fd = -1;
    ~RawSocket() {
        if (fd >= 0)
            ::close(fd);
    }
};
int main(int argc, char **argv) {
    try {
        unsigned seconds = 15, threads = 8, batch_size = 8, window = 8, peer_batch = 1;
        bool fd = false, raw_peer = false, latency = true;
        std::string samples_path;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--fd") {
                fd = true;
                continue;
            }
            if (arg == "--raw-peer") {
                raw_peer = true;
                continue;
            }
            if (arg == "--no-latency") {
                latency = false;
                continue;
            }
            if (arg == "--samples" && i + 1 < argc) {
                samples_path = argv[++i];
                continue;
            }
            if (i + 1 == argc)
                throw std::runtime_error("missing option value");
            const auto value = std::stoul(argv[++i]);
            if (value > 3600)
                throw std::runtime_error("option value too large");
            if (arg == "--seconds")
                seconds = value;
            else if (arg == "--threads")
                threads = value;
            else if (arg == "--batch")
                batch_size = value;
            else if (arg == "--window")
                window = value;
            else if (arg == "--peer-batch")
                peer_batch = value;
            else
                throw std::runtime_error("unknown option: " + arg);
        }
        if (!seconds || !threads || threads > 32 || !batch_size || batch_size > 128 || !window ||
            window > 32 || threads * batch_size * window > 4096 || !peer_batch || peer_batch > 32)
            throw std::runtime_error("invalid workload bounds");

        // First CAN socket = Session, second = simulated peer, for socket_probe.c.
        auto session = tr::Session::create(
            [&] { return std::make_unique<can::SocketCANTransport>("vcan0", fd); });
        std::vector<std::shared_ptr<tr::RouteInbox>> inboxes;
        for (unsigned t = 0; t < threads; ++t)
            inboxes.push_back(session->subscribe_routes({{0x500 + t, false}},
                                                        std::max(128U, batch_size * window)));
        std::unique_ptr<can::SocketCANTransport> peer;
        RawSocket raw;
        if (!raw_peer)
            peer = std::make_unique<can::SocketCANTransport>("vcan0", fd);
        else {
            // Same second socket, no protocol parsing or transport bookkeeping at the simulated
            // peer.
            raw.fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
            if (raw.fd < 0)
                throw std::runtime_error("raw peer socket failed");
            int enable = 1;
            if (fd && ::setsockopt(raw.fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)))
                throw std::runtime_error("raw peer FD setup failed");
            timeval timeout{0, 10000};
            if (::setsockopt(raw.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)))
                throw std::runtime_error("raw peer timeout setup failed");
            ifreq ifr{};
            std::strcpy(ifr.ifr_name, "vcan0");
            if (::ioctl(raw.fd, SIOCGIFINDEX, &ifr))
                throw std::runtime_error("raw peer interface failed");
            sockaddr_can addr{};
            addr.can_family = AF_CAN;
            addr.can_ifindex = ifr.ifr_ifindex;
            if (::bind(raw.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
                throw std::runtime_error("raw peer bind failed");
        }
        std::vector<Distribution> roundtrips(threads), queues(threads), dispatches(threads);
        std::ofstream samples(samples_path);
        if (samples.is_open())
            samples << "elapsed_s,validated,rss_kib,pending_tx,tx_high_water,inbox_high_water\n";
        rusage usage_start{}, usage_end{};
        std::vector<rusage> producer_usage(threads);
        rusage peer_usage{};
        std::atomic<bool> failed{false}, go{false}, stop_peer{false};
        std::atomic<uint64_t> peer_rx{0}, peer_tx{0}, validated{0}, submitted{0};
        std::mutex error_mutex;
        std::string error;
        const auto fail = [&](const std::string &message) {
            std::lock_guard lock(error_mutex);
            if (error.empty())
                error = message;
            failed.store(true);
        };
        std::thread echo([&] {
            pthread_setname_np(pthread_self(), "echo-peer");
            try {
                std::array<canfd_frame, 32> frames{};
                std::array<iovec, 32> iovs{};
                std::array<mmsghdr, 32> messages{};
                for (unsigned i = 0; i < peer_batch; ++i) {
                    iovs[i] = {&frames[i], sizeof(canfd_frame)};
                    messages[i].msg_hdr.msg_iov = &iovs[i];
                    messages[i].msg_hdr.msg_iovlen = 1;
                }
                while (!stop_peer.load()) {
                    if (raw_peer && peer_batch > 1) {
                        for (unsigned i = 0; i < peer_batch; ++i)
                            iovs[i].iov_len = sizeof(canfd_frame);
                        const int n = ::recvmmsg(raw.fd, messages.data(), peer_batch,
                                                 MSG_WAITFORONE, nullptr);
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                            continue;
                        if (n < 0)
                            throw std::runtime_error("recvmmsg failed");
                        for (int i = 0; i < n; ++i) {
                            if (messages[i].msg_len != (fd ? CANFD_MTU : CAN_MTU) ||
                                frames[i].can_id < 0x100 || frames[i].can_id >= 0x100 + threads)
                                throw std::runtime_error("batch peer unexpected frame");
                            ++peer_rx;
                            frames[i].can_id += 0x400;
                            iovs[i].iov_len = messages[i].msg_len;
                        }
                        int sent = 0;
                        while (sent < n) {
                            const int k = ::sendmmsg(raw.fd, messages.data() + sent, n - sent, 0);
                            if (k <= 0)
                                throw std::runtime_error("sendmmsg failed");
                            sent += k;
                            peer_tx.fetch_add(k);
                        }
                        continue;
                    }
                    if (raw_peer) {
                        canfd_frame f{};
                        const ssize_t n = ::read(raw.fd, &f, sizeof(f));
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                            continue;
                        if (n != static_cast<ssize_t>(fd ? CANFD_MTU : CAN_MTU))
                            throw std::runtime_error("raw peer read failed");
                        if (f.can_id < 0x100 || f.can_id >= 0x100 + threads)
                            throw std::runtime_error("raw peer bad id");
                        ++peer_rx;
                        f.can_id += 0x400;
                        if (::write(raw.fd, &f, n) != n)
                            throw std::runtime_error("raw peer write failed");
                        ++peer_tx;
                        continue;
                    }
                    auto message = peer->read();
                    if (!message)
                        continue;
                    if (message->id < 0x100 || message->id >= 0x100 + threads)
                        throw std::runtime_error("unexpected request ID");
                    ++peer_rx;
                    message->id += 0x400;
                    peer->write(*message);
                    ++peer_tx;
                }
            } catch (const std::exception &e) {
                fail(std::string("peer: ") + e.what());
            }
            getrusage(RUSAGE_THREAD, &peer_usage);
        });
        Clock::time_point start;
        std::vector<std::thread> producers;
        for (unsigned t = 0; t < threads; ++t) {
            producers.emplace_back([&, t] {
                const auto thread_name = "producer-" + std::to_string(t);
                pthread_setname_np(pthread_self(), thread_name.c_str());
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                try {
                    uint32_t next_send = 0, next_receive = 0;
                    while (!failed.load() && Clock::now() < start + std::chrono::seconds(seconds)) {
                        std::vector<tr::SendReceipt> receipts;
                        receipts.reserve(window);
                        for (unsigned w = 0; w < window; ++w) {
                            tr::MessageBatch batch;
                            batch.frames.reserve(batch_size);
                            for (unsigned b = 0; b < batch_size; ++b) {
                                can::Message message;
                                message.id = 0x100 + t;
                                message.dlc = fd ? 64 : 8;
                                message.is_fd = message.is_brs = fd;
                                const uint32_t sequence = next_send++;
                                std::memcpy(message.data.data(), &sequence, sizeof(sequence));
                                const uint32_t stamp =
                                    latency
                                        ? static_cast<uint32_t>(
                                              std::chrono::duration_cast<std::chrono::microseconds>(
                                                  Clock::now() - start)
                                                  .count())
                                        : 0;
                                std::memcpy(message.data.data() + 4, &stamp, 4);
                                for (unsigned k = 8; k < message.dlc; ++k)
                                    message.data[k] = static_cast<uint8_t>(sequence + k + t);
                                batch.frames.push_back(message);
                            }
                            receipts.push_back(session->submit(std::move(batch)));
                            submitted.fetch_add(batch_size, std::memory_order_relaxed);
                        }
                        for (const auto &receipt : receipts) {
                            const auto result = receipt.wait_until(Clock::now() + 5s);
                            if (result.outcome != tr::SendOutcome::dispatched ||
                                result.sent_frames != batch_size)
                                throw std::runtime_error("incomplete send");
                            if (latency && result.claimed_at && result.completed_at) {
                                queues[t].add(std::chrono::duration_cast<std::chrono::microseconds>(
                                                  *result.claimed_at - result.accepted_at)
                                                  .count());
                                dispatches[t].add(
                                    std::chrono::duration_cast<std::chrono::microseconds>(
                                        *result.completed_at - *result.claimed_at)
                                        .count());
                            }
                        }
                        for (unsigned n = 0; n < window * batch_size; ++n) {
                            const auto reply = inboxes[t]->receive_until(Clock::now() + 5s);
                            if (!reply)
                                throw std::runtime_error("reply timeout");
                            const auto &message = reply->message;
                            if (message.id != 0x500 + t || message.is_extended ||
                                message.dlc != (fd ? 64 : 8) || message.is_fd != fd ||
                                message.is_brs != fd)
                                throw std::runtime_error("reply metadata mismatch");
                            uint32_t sequence;
                            std::memcpy(&sequence, message.data.data(), sizeof(sequence));
                            if (sequence != next_receive)
                                throw std::runtime_error(
                                    "sequence mismatch: expected=" + std::to_string(next_receive) +
                                    ", received=" + std::to_string(sequence));
                            ++next_receive;
                            uint32_t stamp;
                            std::memcpy(&stamp, message.data.data() + 4, 4);
                            if (latency)
                                roundtrips[t].add(
                                    static_cast<uint32_t>(
                                        std::chrono::duration_cast<std::chrono::microseconds>(
                                            Clock::now() - start)
                                            .count()) -
                                    stamp);
                            for (unsigned k = 8; k < message.dlc; ++k)
                                if (message.data[k] != static_cast<uint8_t>(sequence + k + t))
                                    throw std::runtime_error("payload mismatch");
                            validated.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                } catch (const std::exception &e) {
                    fail("producer " + std::to_string(t) + ": " + e.what());
                }
                getrusage(RUSAGE_THREAD, &producer_usage[t]);
            });
        }
        getrusage(RUSAGE_SELF, &usage_start);
        start = Clock::now();
        go.store(true, std::memory_order_release);
        for (unsigned second = 1; second <= seconds && !failed.load(); ++second) {
            std::this_thread::sleep_until(start + std::chrono::seconds(second));
            const auto h = session->health();
            size_t inbox_high = 0;
            for (const auto &inbox : inboxes)
                inbox_high = std::max(inbox_high, inbox->health().high_water);
            if (samples.is_open()) {
                samples << second << ',' << validated.load() << ',' << rss_kib() << ','
                        << h.pending_tx << ',' << h.queue_high_water << ',' << inbox_high << '\n';
                samples.flush();
            }
        }
        for (auto &producer : producers)
            producer.join();
        getrusage(RUSAGE_SELF, &usage_end);
        const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        // Allow any remaining submitted frames to reach the kernel readers.
        // This is shutdown only: receiving is never deliberately delayed under load.
        std::this_thread::sleep_for(30ms);
        stop_peer.store(true);
        echo.join();
        if (peer)
            peer->close();
        if (raw.fd >= 0) {
            ::close(raw.fd);
            raw.fd = -1;
        }
        Distribution rt, queue, dispatch;
        for (unsigned t = 0; t < threads; ++t) {
            rt.merge(roundtrips[t]);
            queue.merge(queues[t]);
            dispatch.merge(dispatches[t]);
        }
        const double user_us = time_us(usage_end.ru_utime) - time_us(usage_start.ru_utime);
        const double system_us = time_us(usage_end.ru_stime) - time_us(usage_start.ru_stime);
        double producer_cpu_us = 0;
        for (const auto &u : producer_usage)
            producer_cpu_us += time_us(u.ru_utime) + time_us(u.ru_stime);
        const auto health = session->health();
        unsigned inbox_pending = 0;
        for (const auto &inbox : inboxes)
            inbox_pending += inbox->health().pending;
        const bool complete =
            !failed.load() && submitted == validated && submitted == health.tx_frames &&
            submitted == health.rx_frames && submitted == peer_rx && submitted == peer_tx &&
            health.pending_tx == 0 && health.overflows == 0 && health.route_misses == 0 &&
            health.invalid_frames == 0 && health.last_error.empty() && inbox_pending == 0;
        session->close();
        tr::Session::drain_deferred_cleanup();
        std::cout << std::setprecision(12) << "{\"pass\":" << (complete ? "true" : "false")
                  << ",\"seconds\":" << elapsed << ",\"submitted\":" << submitted.load()
                  << ",\"tx_frames\":" << health.tx_frames
                  << ",\"peer_received\":" << peer_rx.load() << ",\"peer_sent\":" << peer_tx.load()
                  << ",\"rx_frames\":" << health.rx_frames << ",\"validated\":" << validated.load()
                  << ",\"inbox_pending\":" << inbox_pending << ",\"overflows\":" << health.overflows
                  << ",\"route_misses\":" << health.route_misses
                  << ",\"invalid_frames\":" << health.invalid_frames
                  << ",\"frames_per_second\":" << static_cast<double>(validated.load()) / elapsed
                  << ",\"cpu_percent_one_core\":" << (user_us + system_us) / elapsed / 10000
                  << ",\"user_cpu_percent\":" << user_us / elapsed / 10000
                  << ",\"system_cpu_percent\":" << system_us / elapsed / 10000
                  << ",\"peer_cpu_percent\":"
                  << (time_us(peer_usage.ru_utime) + time_us(peer_usage.ru_stime)) / elapsed / 10000
                  << ",\"producers_cpu_percent\":" << producer_cpu_us / elapsed / 10000
                  << ",\"voluntary_context_switches\":" << usage_end.ru_nvcsw - usage_start.ru_nvcsw
                  << ",\"involuntary_context_switches\":"
                  << usage_end.ru_nivcsw - usage_start.ru_nivcsw
                  << ",\"minor_faults\":" << usage_end.ru_minflt - usage_start.ru_minflt
                  << ",\"major_faults\":" << usage_end.ru_majflt - usage_start.ru_majflt
                  << ",\"rss_peak_kib\":" << usage_end.ru_maxrss
                  << ",\"rss_final_kib\":" << rss_kib() << ",\"roundtrip_count\":" << rt.count
                  << ",\"roundtrip_p50_us_upper\":" << rt.quantile(500)
                  << ",\"roundtrip_p95_us_upper\":" << rt.quantile(950)
                  << ",\"roundtrip_p99_us_upper\":" << rt.quantile(990)
                  << ",\"roundtrip_p999_us_upper\":" << rt.quantile(999)
                  << ",\"roundtrip_max_us\":" << rt.max_us
                  << ",\"batch_queue_p99_us_upper\":" << queue.quantile(990)
                  << ",\"batch_dispatch_p99_us_upper\":" << dispatch.quantile(990)
                  << ",\"queue_high_water\":" << health.queue_high_water
                  << ",\"raw_peer\":" << (raw_peer ? "true" : "false")
                  << ",\"peer_batch\":" << peer_batch
                  << ",\"latency_enabled\":" << (latency ? "true" : "false") << "}\n";
        if (!complete)
            std::cerr << (error.empty() ? "frame accounting or health check failed" : error)
                      << '\n';
        return complete ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 2;
    }
}
