#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <stop_token>
#include <vector>

#include "lob/order_book.hpp"

namespace lob {

struct BookSnapshot {
    std::size_t live_orders{};
    Price best_bid{};
    Price best_ask{};
};

class MatchingEngineRuntime {
public:
    explicit MatchingEngineRuntime(
        std::size_t expected_orders = 1 << 20,
        std::size_t queue_capacity = 1 << 14);
    ~MatchingEngineRuntime();

    MatchingEngineRuntime(const MatchingEngineRuntime&) = delete;
    MatchingEngineRuntime& operator=(const MatchingEngineRuntime&) = delete;

    bool submit_add(OrderId id, Side side, Price price, Quantity qty);
    bool submit_cancel(OrderId id);
    bool submit_modify(OrderId id, Quantity new_qty);
    bool submit_replace(OrderId id, Price new_price, Quantity new_qty);
    MatchResult submit_limit_order(OrderId id, Side side, Price price, Quantity qty);
    MatchResult submit_market_order(OrderId id, Side side, Quantity qty);

    void sync();
    BookSnapshot snapshot();
    BookSnapshot shutdown_and_snapshot();
    void shutdown();

    [[nodiscard]] std::size_t accepted_command_count() const noexcept;
    [[nodiscard]] std::size_t rejected_full_count() const noexcept;
    [[nodiscard]] std::size_t rejected_stopped_count() const noexcept;

private:
    enum class CommandType : std::uint8_t {
        Add,
        Cancel,
        Modify,
        Replace,
        Limit,
        Market,
        Barrier,
    };

    struct Command {
        CommandType type{};
        OrderId id{};
        Side side{Side::Buy};
        Price price{};
        Quantity qty{};
    };

    struct EgressMessage {
        CommandType type{};
        bool bool_reply{};
        MatchResult match_reply{};
    };

    bool enqueue(Command cmd);
    bool try_dequeue(Command& cmd);
    bool enqueue_egress(EgressMessage msg);
    bool try_dequeue_egress(EgressMessage& msg);
    [[nodiscard]] bool queue_empty() const noexcept;
    void run();

    std::vector<Command> ring_{};
    std::size_t ring_mask_{};
    std::atomic<std::size_t> write_idx_{0};
    std::atomic<std::size_t> read_idx_{0};

    std::vector<EgressMessage> egress_ring_{};
    std::size_t egress_ring_mask_{};
    std::atomic<std::size_t> egress_write_idx_{0};
    std::atomic<std::size_t> egress_read_idx_{0};

    OrderBook book_;
    std::atomic<std::size_t> live_orders_{0};
    std::atomic<Price> best_bid_{0};
    std::atomic<Price> best_ask_{0};
    std::atomic<std::size_t> accepted_commands_{0};
    std::atomic<std::size_t> rejected_full_{0};
    std::atomic<std::size_t> rejected_stopped_{0};

    std::jthread worker_;
};

}  // namespace lob
