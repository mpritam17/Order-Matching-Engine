#include "lob/matching_engine_runtime.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <utility>
#include <pthread.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#define CPU_PAUSE() do {} while(0)
#endif

namespace lob {

namespace {

std::size_t normalize_capacity(std::size_t requested) {
    const std::size_t clamped = std::max<std::size_t>(2, requested);
    return std::bit_ceil(clamped);
}

}  // namespace

MatchingEngineRuntime::MatchingEngineRuntime(std::size_t expected_orders, std::size_t queue_capacity, int worker_core_id)
    : ring_(normalize_capacity(queue_capacity)),
      ring_mask_(ring_.size() - 1),
      egress_ring_(normalize_capacity(queue_capacity)),
      egress_ring_mask_(egress_ring_.size() - 1),
      book_(expected_orders),
      worker_core_id_(worker_core_id),
      worker_([this] { run(); }) {}

MatchingEngineRuntime::~MatchingEngineRuntime() {
    shutdown();
}

bool MatchingEngineRuntime::submit_add(OrderId id, Side side, Price price, Quantity qty) {
    Command cmd{};
    cmd.type = CommandType::Add;
    cmd.id = id;
    cmd.side = side;
    cmd.price = price;
    cmd.qty = qty;
    return enqueue(std::move(cmd));
}

bool MatchingEngineRuntime::submit_cancel(OrderId id) {
    Command cmd{};
    cmd.type = CommandType::Cancel;
    cmd.id = id;
    return enqueue(std::move(cmd));
}

bool MatchingEngineRuntime::submit_modify(OrderId id, Quantity new_qty) {
    Command cmd{};
    cmd.type = CommandType::Modify;
    cmd.id = id;
    cmd.qty = new_qty;
    return enqueue(std::move(cmd));
}

bool MatchingEngineRuntime::submit_replace(OrderId id, Price new_price, Quantity new_qty) {
    Command cmd{};
    cmd.type = CommandType::Replace;
    cmd.id = id;
    cmd.price = new_price;
    cmd.qty = new_qty;

    if (!enqueue(std::move(cmd))) {
        return false;
    }

    EgressMessage reply{};
    while (!try_dequeue_egress(reply)) {
        CPU_PAUSE();
    }
    return reply.bool_reply;
}

MatchResult MatchingEngineRuntime::submit_limit_order(OrderId id, Side side, Price price, Quantity qty) {
    Command cmd{};
    cmd.type = CommandType::Limit;
    cmd.id = id;
    cmd.side = side;
    cmd.price = price;
    cmd.qty = qty;

    if (!enqueue(std::move(cmd))) {
        MatchResult failed{};
        failed.remaining_qty = qty;
        return failed;
    }

    EgressMessage reply{};
    while (!try_dequeue_egress(reply)) {
        CPU_PAUSE();
    }
    return reply.match_reply;
}

MatchResult MatchingEngineRuntime::submit_market_order(OrderId id, Side side, Quantity qty) {
    Command cmd{};
    cmd.type = CommandType::Market;
    cmd.id = id;
    cmd.side = side;
    cmd.qty = qty;

    if (!enqueue(std::move(cmd))) {
        MatchResult failed{};
        failed.remaining_qty = qty;
        return failed;
    }

    EgressMessage reply{};
    while (!try_dequeue_egress(reply)) {
        CPU_PAUSE();
    }
    return reply.match_reply;
}

void MatchingEngineRuntime::sync() {
    while (!worker_.get_stop_token().stop_requested()) {
        Command cmd{};
        cmd.type = CommandType::Barrier;

        if (enqueue(std::move(cmd))) {
            EgressMessage reply{};
            while (!try_dequeue_egress(reply)) {
                if (worker_.get_stop_token().stop_requested()) {
                    return;
                }
                CPU_PAUSE();
            }
            return;
        }

        std::this_thread::yield();
    }
}

BookSnapshot MatchingEngineRuntime::snapshot() {
    sync();
    BookSnapshot snap{};
    snap.live_orders = live_orders_.load(std::memory_order_relaxed);
    snap.best_bid = best_bid_.load(std::memory_order_relaxed);
    snap.best_ask = best_ask_.load(std::memory_order_relaxed);
    return snap;
}

void MatchingEngineRuntime::shutdown() {
    if (!worker_.joinable()) {
        return;
    }

    worker_.request_stop();
    worker_.join();
}

BookSnapshot MatchingEngineRuntime::shutdown_and_snapshot() {
    shutdown();
    BookSnapshot snap{};
    snap.live_orders = live_orders_.load(std::memory_order_relaxed);
    snap.best_bid = best_bid_.load(std::memory_order_relaxed);
    snap.best_ask = best_ask_.load(std::memory_order_relaxed);
    return snap;
}

std::size_t MatchingEngineRuntime::accepted_command_count() const noexcept {
    return accepted_commands_.load(std::memory_order_relaxed);
}

std::size_t MatchingEngineRuntime::rejected_full_count() const noexcept {
    return rejected_full_.load(std::memory_order_relaxed);
}

std::size_t MatchingEngineRuntime::rejected_stopped_count() const noexcept {
    return rejected_stopped_.load(std::memory_order_relaxed);
}

bool MatchingEngineRuntime::enqueue(Command cmd) {
    if (worker_.get_stop_token().stop_requested()) {
        rejected_stopped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::size_t write = write_idx_.load(std::memory_order_relaxed);
    const std::size_t read = read_idx_.load(std::memory_order_acquire);
    if ((write - read) >= ring_.size()) {
        rejected_full_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    ring_[write & ring_mask_] = std::move(cmd);
    write_idx_.store(write + 1, std::memory_order_release);
    accepted_commands_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool MatchingEngineRuntime::try_dequeue(Command& cmd) {
    const std::size_t read = read_idx_.load(std::memory_order_relaxed);
    const std::size_t write = write_idx_.load(std::memory_order_acquire);
    if (read == write) {
        return false;
    }

    Command& slot = ring_[read & ring_mask_];
    cmd = std::move(slot);
    slot = Command{};
    read_idx_.store(read + 1, std::memory_order_release);
    return true;
}

bool MatchingEngineRuntime::enqueue_egress(EgressMessage msg) {
    const std::size_t write = egress_write_idx_.load(std::memory_order_relaxed);
    const std::size_t read = egress_read_idx_.load(std::memory_order_acquire);
    if ((write - read) >= egress_ring_.size()) {
        return false;
    }

    egress_ring_[write & egress_ring_mask_] = std::move(msg);
    egress_write_idx_.store(write + 1, std::memory_order_release);
    return true;
}

bool MatchingEngineRuntime::try_dequeue_egress(EgressMessage& msg) {
    const std::size_t read = egress_read_idx_.load(std::memory_order_relaxed);
    const std::size_t write = egress_write_idx_.load(std::memory_order_acquire);
    if (read == write) {
        return false;
    }

    EgressMessage& slot = egress_ring_[read & egress_ring_mask_];
    msg = std::move(slot);
    slot = EgressMessage{};
    egress_read_idx_.store(read + 1, std::memory_order_release);
    return true;
}

bool MatchingEngineRuntime::queue_empty() const noexcept {
    const std::size_t read = read_idx_.load(std::memory_order_acquire);
    const std::size_t write = write_idx_.load(std::memory_order_acquire);
    return read == write;
}

void MatchingEngineRuntime::run() {
#if defined(__linux__) && !defined(__ANDROID__)
    if (worker_core_id_ >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(worker_core_id_, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#endif

    while (true) {
        Command cmd{};

        while (!try_dequeue(cmd)) {
            if (worker_.get_stop_token().stop_requested() && queue_empty()) {
                return;
            }
            CPU_PAUSE();
        }

        switch (cmd.type) {
            case CommandType::Add:
                (void)book_.add_order(cmd.id, cmd.side, cmd.price, cmd.qty);
                break;
            case CommandType::Cancel:
                (void)book_.cancel_order(cmd.id);
                break;
            case CommandType::Modify:
                (void)book_.modify_order(cmd.id, cmd.qty);
                break;
            case CommandType::Replace: {
                EgressMessage reply{};
                reply.type = cmd.type;
                reply.bool_reply = book_.replace_order(cmd.id, cmd.price, cmd.qty);
                while (!enqueue_egress(reply)) {
                    if (worker_.get_stop_token().stop_requested()) return;
                    CPU_PAUSE();
                }
                break;
            }
            case CommandType::Limit: {
                EgressMessage reply{};
                reply.type = cmd.type;
                reply.match_reply = book_.process_limit_order(cmd.id, cmd.side, cmd.price, cmd.qty);
                while (!enqueue_egress(std::move(reply))) {
                    if (worker_.get_stop_token().stop_requested()) return;
                    CPU_PAUSE();
                }
                break;
            }
            case CommandType::Market: {
                EgressMessage reply{};
                reply.type = cmd.type;
                reply.match_reply = book_.process_market_order(cmd.id, cmd.side, cmd.qty);
                while (!enqueue_egress(std::move(reply))) {
                    if (worker_.get_stop_token().stop_requested()) return;
                    CPU_PAUSE();
                }
                break;
            }
            case CommandType::Barrier: {
                EgressMessage reply{};
                reply.type = cmd.type;
                while (!enqueue_egress(reply)) {
                    if (worker_.get_stop_token().stop_requested()) return;
                    CPU_PAUSE();
                }
                break;
            }
        }

        live_orders_.store(book_.live_order_count(), std::memory_order_relaxed);
        best_bid_.store(book_.best_bid(), std::memory_order_relaxed);
        best_ask_.store(book_.best_ask(), std::memory_order_relaxed);
    }
}

}  // namespace lob
