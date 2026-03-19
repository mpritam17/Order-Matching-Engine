#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "lob/matching_engine_runtime.hpp"

namespace {

std::size_t parse_iterations(int argc, char** argv) {
    if (argc < 2) {
        return 10'000'000;
    }
    const int parsed = std::atoi(argv[1]);
    if (parsed <= 0) {
        return 10'000'000;
    }
    return static_cast<std::size_t>(parsed);
}

void benchmark_throughput(std::size_t iterations) {
    // Large sizes so the queues/pools don't become the bottleneck
    // Use worker thread on unpinned core by default
    lob::MatchingEngineRuntime engine(1 << 24, 1 << 20);

    // Warm up the book with some initial resting liquidity to allow crossing, partial fills and cancels
    std::cout << "Seeding initial LOB liquidity...\n";
    for(std::size_t i = 0; i < 500'000; ++i) {
        lob::Price px = 1000 + (i % 100);
        engine.submit_add(100ULL + i, lob::Side::Sell, px, 50);
        engine.submit_add(2000000ULL + i, lob::Side::Buy, 999 - (i % 100), 50);
    }
    engine.sync();

    std::cout << "Preparing " << iterations << " mixed-scenario orders for throughput test...\n";

    // Timing start right as we blast
    const auto start_time = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < iterations; ++i) {
        // Pattern: 
        // 0: Add passive Buy
        // 1: Add passive Sell
        // 2: Aggressive Buy (Partial Fill crossing resting liquidity)
        // 3: Aggressive Sell (Full Fill sweeping resting liquidity)
        // 4: Cancel a previously posted order

        const std::size_t mod = i % 5;
        const lob::OrderId oid = 10000000ULL + i;

        if (mod == 0) {
            lob::Price px = 900 + (i % 50);
            while (!engine.submit_add(oid, lob::Side::Buy, px, 25)) {}
        } else if (mod == 1) {
            lob::Price px = 1100 + (i % 50);
            while (!engine.submit_add(oid, lob::Side::Sell, px, 25)) {}
        } else if (mod == 2) {
            // Aggressive Limit Buy - intentionally crosses the spread
            // Egress queue will flush fills back to us, so we must handle them to unblock the engine
            lob::Price px = 1100 + (i % 50); 
            engine.submit_limit_order(oid, lob::Side::Buy, px, 40); // 40 qty (partial fill or full)
        } else if (mod == 3) {
            lob::Price px = 999 - (i % 50);
            engine.submit_limit_order(oid, lob::Side::Sell, px, 100); // larger sweep
        } else if (mod == 4) {
            // Cancel an order that we posted previously (from mod 0 or 1)
            lob::OrderId target = oid - 4; 
            while (!engine.submit_cancel(target)) {}
        }
    }

    // Barrier sync to strictly guarantee matcher has finished consuming ALL generated commands
    engine.sync();

    const auto end_time = std::chrono::steady_clock::now();
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    const double seconds = static_cast<double>(duration_ms) / 1000.0;
    const double ops = static_cast<double>(iterations) / seconds;

    std::cout << "-------------------------------------------\n";
    std::cout << "Throughput Benchmark Complete\n";
    std::cout << "Total Orders Processed: " << iterations << '\n';
    std::cout << "Total Time Taken:       " << duration_ms << " ms\n";
    std::cout << "Avg Throughput:         " << ops << " orders/sec\n";
    std::cout << "                        (" << static_cast<std::size_t>(ops / 1'000'000.0 * 100.0) / 100.0 << " M/sec)\n";
    std::cout << "-------------------------------------------\n";

    engine.shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t iterations = parse_iterations(argc, argv);
    std::cout << "Tip: Run in Release build for actual throughput numbers (-DCMAKE_BUILD_TYPE=Release)\n";
    benchmark_throughput(iterations);
    return 0;
}