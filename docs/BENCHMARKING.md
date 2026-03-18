# Benchmarking Guide

## Executable

`latency_benchmark` is built from `benchmarks/latency_benchmark.cpp`.

## What It Reports

Per scenario, it reports:

- `p50`
- `p99`
- `p99.9`
- `max`

Units are nanoseconds.

## Scenarios

1. `submit_add producer-path`
- Measures call latency of `submit_add` on the producer side.
- Captures queue admission and enqueue overhead.

2. `submit_limit_order round-trip`
- Measures end-to-end latency from submit call to returned `MatchResult`.
- Includes queueing + worker processing + result handoff.

## Running

Debug build (quick sanity):

```bash
cmake -S . -B build
cmake --build build -j
./build/latency_benchmark 5000
```

Release build (recommended):

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
./build-release/latency_benchmark 100000
```

## Interpreting Results

- Use Release numbers for comparisons.
- Focus on `p99` and `p99.9` for tail latency.
- Track `max` carefully; occasional spikes can indicate scheduling or contention events.

## Suggested Next Benchmarks

- Isolated SPSC queue microbenchmark (enqueue/dequeue only).
- End-to-end benchmark with mixed order flow percentages.
- Benchmark with thread pinning and CPU isolation for stable tails.
