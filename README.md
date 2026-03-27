# Order Matching Engine (C++20)

A low-latency limit order book and matching engine prototype in modern C++.

## Current Scope

- Intrusive FIFO order queues per price level
- Pool-backed `OrderNode` and `PriceLevel` storage skeleton
- Price-time matching for limit and market orders
- Replace semantics with priority rules
- `std::jthread` runtime with bounded SPSC ring buffer command queue
- Unit tests for book behavior and runtime backpressure/drain behavior
- Latency benchmark executable with percentile reporting

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

Demo:

```bash
./build/lob_demo
```

Tests:

```bash
ctest --test-dir build --output-on-failure
```

Latency Benchmark (default 20000 iterations):

```bash
./build/latency_benchmark
```

Latency Benchmark with custom iteration count:

```bash
./build/latency_benchmark 50000
```

Throughput Benchmark (default 1M iterations, 500k initial book size):

```bash
./build/throughput_benchmark
```

Throughput Benchmark with custom parameters (<iterations> <book_size>):

```bash
./build/throughput_benchmark 5000000 10000000
```

## Release Benchmark Run

For realistic latency numbers, use Release mode:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
./build-release/latency_benchmark 100000
```

## Repository Layout

- `include/lob/`: Public headers (`OrderBook`, runtime, types)
- `src/`: Core implementation
- `tests/`: Unit and runtime integration tests
- `benchmarks/`: Latency and throughput benchmark executables
- `docs/`: Design and usage documentation

## Key APIs

Order book API (`include/lob/order_book.hpp`):
- `add_order`, `cancel_order`, `modify_order`, `replace_order`
- `process_limit_order`, `process_market_order`
- `best_bid`, `best_ask`, `live_order_count`

Runtime API (`include/lob/matching_engine_runtime.hpp`):
- `submit_add`, `submit_cancel`, `submit_modify`, `submit_replace`
- `submit_limit_order`, `submit_market_order`
- `sync`, `snapshot`, `shutdown`, `shutdown_and_snapshot`

## Notes

- The design currently prioritizes deterministic behavior and clear evolution steps over full production completeness.
- Order Book indexing paths use `ankerl::unordered_dense` for fast, cache-friendly lookups.
- The consumer worker runs a lock-free spin-wait loop using processor pause instructions to avoid thread context switches.
- Runtime queue is SPSC and bounded. Under load, submissions can be rejected when the queue is full.
