#include <iostream>

#include "lob/matching_engine_runtime.hpp"

int main() {
    lob::MatchingEngineRuntime engine;

    engine.submit_add(1001, lob::Side::Buy, 10050, 10);
    engine.submit_add(1002, lob::Side::Buy, 10050, 20);
    engine.submit_add(1003, lob::Side::Sell, 10080, 15);

    engine.submit_modify(1002, 25);
    engine.submit_cancel(1001);

    const lob::BookSnapshot snap = engine.snapshot();

    std::cout << "Live orders: " << snap.live_orders << '\n';
    std::cout << "Best bid: " << snap.best_bid << '\n';
    std::cout << "Best ask: " << snap.best_ask << '\n';

    engine.shutdown();
    return 0;
}
