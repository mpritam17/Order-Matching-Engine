# Order Matching Engine - Optimization Results

This document tracks the detailed architectural changes and latency reductions achieved during the low-latency optimization phases of the order matching engine.

## 1. Data Structure Optimization: Flat Hash Maps
- **Previous State**: The `OrderBook` relied on standard library `std::unordered_map` for mapping `OrderId` to order nodes (`orders_by_id_`) and for indexing price levels (`LevelMap`). This node-based, bucketed approach resulted in unpredictable heap allocations, pointer-chasing, and numerous L1/L2 cache misses.
- **Change Implemented**: Replaced `std::unordered_map` with `ankerl::unordered_dense::map`, a highly efficient, single-header C++ flat hash map.
- **Impact**: All order and price level tracking is now packed into a contiguous `std::vector` backing utilizing Robin Hood hashing. Node storage easily aligns within CPU cache lines (64-byte blocks), preventing fragmentation, maximizing cache locality, and providing much tighter bounds on tail latencies.

## 2. Thread Hot-Polling: Spin-Wait Loop
- **Previous State**: The `MatchingEngineRuntime::run()` consumer thread relied on a mutex and `std::condition_variable_any` to sleep when the inbound command queue was empty. Waking this thread required expensive OS-level kernel context switches.
- **Change Implemented**: The lock and condition variable were entirely removed. The consumer thread now implements a lock-free spin-wait loop that leverages cross-platform processor yield instructions (`_mm_pause()` via `<immintrin.h>` on x86_64, or `yield` on ARM).
- **Impact**: The worker thread remains continuously "hot" on the CPU, preventing context switches. In micro-benchmarks, this dropped the `submit_add` path overhead slightly from ~216ns to ~200ns, and drastically clamped down on extreme maximum latency spikes, as the kernel no longer preempts the thread to sleep states.

## 3. Eradicating `std::promise`: Lock-Free Egress Queue
- **Previous State**: Operations that required synchronous returns from the matchmaking engine (`submit_limit_order`, `submit_market_order`, and `submit_replace`) allocated a `std::shared_ptr<std::promise<...>>`. The producer thread would call `.get()` (an OS-level lock) while the worker thread matched the order and fired `.set_value()`.
- **Change Implemented**: `std::promise` and `std::future` were stripped out entirely. We implemented a secondary bounded SPSC (Single-Producer Single-Consumer) ring buffer specifically for egress messages (`egress_ring_`). The matching thread now computes the results and lock-free pushes an `EgressMessage` back to the producer thread, which spin-waits for its specific response.
- **Impact**: Substantial throughput and latency wins by removing dynamic heap allocations and kernel locks from the critical path. The **round-trip execution time for `submit_limit_order` dropped by approximately 61%** (p50 latency went from roughly `~4381ns` down to `~1688ns`). Tail latencies at the p99 level also tightened exceptionally.

---
*Next planned optimizations: CPU core pinning (`pthread_setaffinity_np`) to prevent process migration, and backing hot arenas with huge pages for TLB optimization.*