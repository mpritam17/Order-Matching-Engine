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
    lob::MatchingEngineRuntime engine(1 << 24, 1 << 20);

    std::cout << "Preparing " << iterations << " limit and cross orders for throughput test...\n";

    // Timing start right as we blast
    const auto start_time = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < iterations; ++i) {
        bool enqueued = false;

        // Alternate filling bids and asks so the book doesn't go fully one-sided
        // We use add instead of limit to prevent waiting on the egress loop per-order.
        if (i % 2 == 0) {
            lob::Price px = 100 + (i % 10);
            while (!engine.submit_add(1000ULL + i, lob::Side::Buy, px, 1)) {
                // If SPSC ring full, brief yield to let matcher consumer catch up
                // std::this_thread::yield();
            }
        } else {
            lob::Price px = 100 + (i % 10);
            while (!engine.submit_add(1000ULL + i, lob::Side::Sell, px, 1)) {
                // std::this_thread::yield();
            }
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