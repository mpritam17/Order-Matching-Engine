#include <cstdlib>
#include <iostream>

#include "lob/matching_engine_runtime.hpp"

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void test_backpressure_on_tiny_queue() {
    lob::MatchingEngineRuntime engine(1 << 14, 2);

    constexpr std::size_t kBurst = 50000;
    std::size_t submit_ok = 0;
    for (std::size_t i = 0; i < kBurst; ++i) {
        if (engine.submit_add(1000000 + i, lob::Side::Buy, 100, 1)) {
            ++submit_ok;
        }
    }

    const lob::BookSnapshot final = engine.shutdown_and_snapshot();

    expect(submit_ok > 0, "at least some commands accepted");
    expect(engine.rejected_full_count() > 0, "queue backpressure should reject some commands");
    expect(engine.accepted_command_count() >= submit_ok, "accepted metric tracks successful submissions");
    expect(final.live_orders == submit_ok, "shutdown drains accepted tiny-queue commands");
}

void test_shutdown_drains_queue() {
    lob::MatchingEngineRuntime engine(1 << 14, 64);

    constexpr std::size_t kOrders = 5000;
    std::size_t submit_ok = 0;
    for (std::size_t i = 0; i < kOrders; ++i) {
        if (engine.submit_add(2000000 + i, lob::Side::Buy, 101, 1)) {
            ++submit_ok;
        }
    }

    const lob::BookSnapshot final = engine.shutdown_and_snapshot();

    expect(final.live_orders == submit_ok, "shutdown should drain accepted commands");
    expect(final.best_bid == 101, "final best bid after drained adds");
    expect(engine.rejected_stopped_count() == 0, "no stopped rejection before shutdown call");
}

void test_runtime_week2_order_flow() {
    lob::MatchingEngineRuntime engine(1 << 14, 256);

    expect(engine.submit_add(3001, lob::Side::Sell, 101, 5), "seed sell 3001");
    expect(engine.submit_add(3002, lob::Side::Sell, 101, 4), "seed sell 3002");

    const lob::MatchResult limit = engine.submit_limit_order(4001, lob::Side::Buy, 101, 6);
    expect(limit.filled_qty == 6, "runtime limit order filled qty");
    expect(limit.remaining_qty == 0, "runtime limit has no remainder");
    expect(limit.fill_count == 2, "runtime limit has two fills");
    expect(limit.fills[0].passive_order_id == 3001, "runtime limit consumes FIFO first");

    const lob::MatchResult market = engine.submit_market_order(5001, lob::Side::Buy, 10);
    expect(market.filled_qty == 3, "runtime market consumes remaining ask qty");
    expect(market.remaining_qty == 7, "runtime market reports unfilled remainder");

    expect(engine.submit_add(6001, lob::Side::Buy, 99, 5), "add buy 6001");
    expect(engine.submit_add(6002, lob::Side::Buy, 99, 5), "add buy 6002");
    expect(engine.submit_replace(6001, 99, 8), "replace same-price increase should succeed");

    const lob::MatchResult check_fifo = engine.submit_limit_order(7001, lob::Side::Sell, 99, 5);
    expect(check_fifo.filled_qty == 5, "sell crosses bid level");
    expect(check_fifo.fill_count > 0, "has fill for fifo check");
    expect(check_fifo.fills[0].passive_order_id == 6002, "replace reset priority behind existing order");

    const lob::BookSnapshot snap = engine.shutdown_and_snapshot();
    expect(snap.best_bid == 99, "best bid remains after partial matching");
}

}  // namespace

int main() {
    test_backpressure_on_tiny_queue();
    test_shutdown_drains_queue();
    test_runtime_week2_order_flow();

    std::cout << "All runtime tests passed.\n";
    return 0;
}
