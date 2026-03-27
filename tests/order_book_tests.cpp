#include <cstdlib>
#include <iostream>
#include <vector>

#include "lob/order_book.hpp"

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void test_order_lifecycle() {
    lob::OrderBook book(256);

    expect(book.add_order(1, lob::Side::Buy, 100, 10), "add order 1");
    expect(book.add_order(2, lob::Side::Sell, 105, 15), "add order 2");
    expect(book.live_order_count() == 2, "live count after add");
    expect(book.best_bid() == 100, "best bid after add");
    expect(book.best_ask() == 105, "best ask after add");

    expect(book.modify_order(1, 7), "modify order 1 qty");
    const lob::OrderNode* order = book.find_order(1);
    expect(order != nullptr, "find modified order");
    expect(order->qty == 7, "modified qty persisted");

    expect(book.cancel_order(1), "cancel order 1");
    expect(book.find_order(1) == nullptr, "order removed after cancel");
    expect(book.live_order_count() == 1, "live count after cancel");
    expect(!book.cancel_order(999), "cancel unknown fails");
}

void test_fifo_and_aggressive_matching() {
    lob::OrderBook book(256);

    expect(book.add_order(10, lob::Side::Sell, 101, 4), "resting sell 10");
    expect(book.add_order(11, lob::Side::Sell, 101, 6), "resting sell 11");
    expect(book.add_order(12, lob::Side::Sell, 102, 5), "resting sell 12");

    const std::vector<lob::OrderId> fifo_before = book.orders_at_level(lob::Side::Sell, 101);
    expect(fifo_before.size() == 2, "two orders on same level");
    expect(fifo_before[0] == 10 && fifo_before[1] == 11, "fifo insertion order");

    const lob::MatchResult match = book.process_limit_order(1000, lob::Side::Buy, 101, 8);
    expect(match.filled_qty == 8, "aggressive fill quantity");
    expect(match.remaining_qty == 0, "no remaining for fully matched aggressive");
    expect(match.fill_count == 2, "two passive fills expected");
    expect(match.fills[0].passive_order_id == 10, "first fill hits oldest passive");
    expect(match.fills[0].qty == 4, "first fill qty correct");
    expect(match.fills[1].passive_order_id == 11, "second fill hits next passive");
    expect(match.fills[1].qty == 4, "second fill qty correct");

    const lob::OrderNode* remaining = book.find_order(11);
    expect(remaining != nullptr, "partially matched passive remains");
    expect(remaining->qty == 2, "remaining passive qty updated");

    const lob::MatchResult partial = book.process_limit_order(2000, lob::Side::Buy, 102, 20);
    expect(partial.filled_qty == 7, "fills remaining passive liquidity");
    expect(partial.remaining_qty == 13, "unfilled aggressive remainder");

    const lob::OrderNode* resting_buy = book.find_order(2000);
    expect(resting_buy != nullptr, "remaining aggressive rests on book");
    expect(resting_buy->qty == 13, "resting remainder quantity");
    expect(resting_buy->price == 102, "resting remainder price");
}

void test_replace_semantics() {
    lob::OrderBook book(256);

    expect(book.add_order(31, lob::Side::Buy, 100, 10), "add buy 31");
    expect(book.add_order(32, lob::Side::Buy, 100, 8), "add buy 32");

    expect(book.replace_order(31, 100, 6), "same-price decrease replace");
    auto fifo = book.orders_at_level(lob::Side::Buy, 100);
    expect(fifo.size() == 2, "two orders still on level 100");
    expect(fifo[0] == 31 && fifo[1] == 32, "priority preserved for size decrease");

    expect(book.replace_order(31, 100, 12), "same-price increase replace");
    fifo = book.orders_at_level(lob::Side::Buy, 100);
    expect(fifo.size() == 2, "two orders on level after reset");
    expect(fifo[0] == 32 && fifo[1] == 31, "priority reset for size increase");

    expect(book.replace_order(32, 99, 8), "price-change replace");
    const auto fifo_100 = book.orders_at_level(lob::Side::Buy, 100);
    const auto fifo_99 = book.orders_at_level(lob::Side::Buy, 99);
    expect(fifo_100.size() == 1 && fifo_100[0] == 31, "moved order removed from old level");
    expect(fifo_99.size() == 1 && fifo_99[0] == 32, "moved order inserted at new level");
}

void test_market_order_matching() {
    lob::OrderBook book(256);

    expect(book.add_order(41, lob::Side::Sell, 101, 5), "add sell 41");
    expect(book.add_order(42, lob::Side::Sell, 102, 7), "add sell 42");

    const lob::MatchResult m = book.process_market_order(9000, lob::Side::Buy, 9);
    expect(m.filled_qty == 9, "market buy filled qty");
    expect(m.remaining_qty == 0, "market buy no remainder");
    expect(m.fill_count == 2, "market buy two fills");
    expect(m.fills[0].passive_order_id == 41, "market buy consumes best ask first");
    expect(m.fills[0].qty == 5, "market buy first fill qty");
    expect(m.fills[1].passive_order_id == 42, "market buy consumes next ask");
    expect(m.fills[1].qty == 4, "market buy second fill qty");

    const lob::OrderNode* rest = book.find_order(42);
    expect(rest != nullptr, "partially filled second ask remains");
    expect(rest->qty == 3, "remaining qty on second ask updated");
    expect(book.find_order(9000) == nullptr, "market order never rests on book");
}

}  // namespace

int main() {
    test_order_lifecycle();
    test_fifo_and_aggressive_matching();
    test_replace_semantics();
    test_market_order_matching();

    std::cout << "All order book tests passed.\n";
    return 0;
}
