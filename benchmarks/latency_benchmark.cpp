#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "lob/matching_engine_runtime.hpp"

namespace {

struct Stats {
    std::uint64_t p50{};
    std::uint64_t p99{};
    std::uint64_t p999{};
    std::uint64_t max{};
};

std::uint64_t percentile_at(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const std::size_t pos = static_cast<std::size_t>(idx);
    return sorted[pos];
}

Stats compute_stats(std::vector<std::uint64_t> samples_ns) {
    std::sort(samples_ns.begin(), samples_ns.end());
    Stats s{};
    s.p50 = percentile_at(samples_ns, 0.50);
    s.p99 = percentile_at(samples_ns, 0.99);
    s.p999 = percentile_at(samples_ns, 0.999);
    s.max = samples_ns.empty() ? 0 : samples_ns.back();
    return s;
}

void print_stats(const std::string& name, const Stats& stats) {
    std::cout << name << " (ns): "
              << "p50=" << stats.p50 << ", "
              << "p99=" << stats.p99 << ", "
              << "p99.9=" << stats.p999 << ", "
              << "max=" << stats.max << '\n';
}

std::size_t parse_iterations(int argc, char** argv) {
    if (argc < 2) {
        return 20000;
    }
    const int parsed = std::atoi(argv[1]);
    if (parsed <= 0) {
        return 20000;
    }
    return static_cast<std::size_t>(parsed);
}

void benchmark_submit_add(std::size_t iterations) {
    lob::MatchingEngineRuntime engine(1 << 20, 1 << 18);
    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);

    for (std::size_t i = 0; i < iterations; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = engine.submit_add(100000000ULL + i, lob::Side::Buy, 100, 1);
        const auto t1 = std::chrono::steady_clock::now();

        if (!ok) {
            engine.sync();
            continue;
        }

        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples.push_back(static_cast<std::uint64_t>(ns));
    }

    engine.shutdown();
    print_stats("submit_add producer-path", compute_stats(std::move(samples)));
}

void benchmark_limit_round_trip(std::size_t iterations) {
    lob::MatchingEngineRuntime engine(1 << 20, 1 << 14);
    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);

    for (std::size_t i = 0; i < iterations; ++i) {
        const lob::OrderId passive_id = 200000000ULL + i;
        const lob::OrderId aggressive_id = 300000000ULL + i;
        while (!engine.submit_add(passive_id, lob::Side::Sell, 101, 1)) {
            engine.sync();
        }

        const auto t0 = std::chrono::steady_clock::now();
        const lob::MatchResult r = engine.submit_limit_order(aggressive_id, lob::Side::Buy, 101, 1);
        const auto t1 = std::chrono::steady_clock::now();

        if (r.filled_qty != 1) {
            continue;
        }

        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples.push_back(static_cast<std::uint64_t>(ns));
    }

    engine.shutdown();
    print_stats("submit_limit_order round-trip", compute_stats(std::move(samples)));
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t iterations = parse_iterations(argc, argv);

    std::cout << "Latency benchmark iterations=" << iterations << '\n';
    std::cout << "Tip: run Release build for realistic numbers.\n";

    benchmark_submit_add(iterations);
    benchmark_limit_round_trip(iterations);

    return 0;
}
